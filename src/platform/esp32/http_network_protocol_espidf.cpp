#include "fujinet/platform/esp32/http_network_protocol_espidf.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "fujinet/core/logging.h"

extern "C" {
#include "esp_err.h"
#include "esp_http_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
}

namespace fujinet::platform::esp32 {

static constexpr const char* TAG = "net.http";

static bool method_supported(std::uint8_t method) {
    // v1: GET(1), POST(2), PUT(3), DELETE(4), HEAD(5)
    switch (method) {
        case 1: // GET
        case 2: // POST
        case 3: // PUT
        case 4: // DELETE
        case 5: // HEAD
            return true;
        default:
            return false;
    }
}


struct HttpNetworkProtocolEspIdfState {
    static constexpr std::size_t header_cap_default = 2048;
    static constexpr std::size_t rb_size = 8192;
    static constexpr TickType_t  wait_step_ticks = pdMS_TO_TICKS(50);

    // stream buffer (bounded, producer->consumer with backpressure)
    StreamBufferHandle_t stream{nullptr};
    StaticStreamBuffer_t stream_storage{};
    std::uint8_t stream_buf[rb_size + 1]{};

    // protects metadata/header string; keep lock scope small (strings allocate)
    SemaphoreHandle_t meta_mutex{nullptr};
    StaticSemaphore_t meta_mutex_storage{};

    // esp_http_client + task
    esp_http_client_handle_t client{nullptr};
    TaskHandle_t task{nullptr};
    SemaphoreHandle_t done_sem{nullptr};
    StaticSemaphore_t done_sem_storage{};

    // request & state
    bool want_headers{false};
    bool header_cap_reached{false};
    std::size_t header_cap{header_cap_default};

    std::uint8_t method{0};

    // POST/PUT request-body streaming state (no buffering)
    bool has_request_body{false};
    std::uint32_t expected_body_len{0};
    std::uint32_t sent_body_len{0};
    bool upload_open{false};

    bool has_http_status{false};
    std::uint16_t http_status{0};

    bool has_content_length{false};
    std::uint64_t content_length{0};

    bool done{false};
    esp_err_t err{ESP_OK};

    std::uint32_t read_cursor{0};

    volatile bool stop_requested{false};

    void reset_session_state()
    {
        want_headers = false;
        header_cap_reached = false;
        header_cap = header_cap_default;
        method = 0;

        has_http_status = false;
        http_status = 0;
        has_content_length = false;
        content_length = 0;

        done = false;
        err = ESP_OK;
        read_cursor = 0;
        stop_requested = false;

        has_request_body = false;
        expected_body_len = 0;
        sent_body_len = 0;
        upload_open = false;

    }

    std::string headers_block;
};

static void take_mutex(SemaphoreHandle_t m)
{
    if (!m) return;
    (void)xSemaphoreTake(m, portMAX_DELAY);
}

static void give_mutex(SemaphoreHandle_t m)
{
    if (!m) return;
    (void)xSemaphoreGive(m);
}

static void set_status_and_length(HttpNetworkProtocolEspIdfState& s)
{
    if (!s.client) return;

    // Status becomes valid after response headers are parsed by esp_http_client.
    if (!s.has_http_status) {
        const int st = esp_http_client_get_status_code(s.client);
        if (st > 0) {
            s.has_http_status = true;
            s.http_status = static_cast<std::uint16_t>(st);
        }
    }

    if (!s.has_content_length) {
        const long long clen = esp_http_client_get_content_length(s.client);
        if (clen >= 0) {
            s.has_content_length = true;
            s.content_length = static_cast<std::uint64_t>(clen);
        }
    }
}

static bool stream_send_all(HttpNetworkProtocolEspIdfState& s,
                            const std::uint8_t* data,
                            std::size_t len)
{
    if (!s.stream || !data || len == 0) {
        return true;
    }

    const std::uint8_t* p = data;
    std::size_t remaining = len;

    while (remaining > 0) {
        if (s.stop_requested) {
            return false;
        }

        const std::size_t sent = xStreamBufferSend(
            s.stream,
            p,
            remaining,
            HttpNetworkProtocolEspIdfState::wait_step_ticks
        );
        
        if (sent == 0) {
            // No space yet. IMPORTANT: yield/delay to avoid busy-spinning and starving the fujibus/task loop.
            vTaskDelay(HttpNetworkProtocolEspIdfState::wait_step_ticks);
            continue;
        }
        
        p += sent;
        remaining -= sent;
        
    }

    return true;
}

static esp_err_t event_handler(esp_http_client_event_t* evt)
{
    if (!evt || !evt->user_data) {
        return ESP_FAIL;
    }

    auto* s = static_cast<HttpNetworkProtocolEspIdfState*>(evt->user_data);
    if (!s) {
        return ESP_FAIL;
    }

    if (s->stop_requested) {
        return ESP_FAIL;
    }

    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER: {
        take_mutex(s->meta_mutex);
        set_status_and_length(*s);

        if (s->want_headers && !s->header_cap_reached && evt->header_key && evt->header_value) {
            // Append "Key: Value\r\n" with a total cap. If cap would be exceeded,
            // stop collecting further headers (but do not fail the request).
            const std::size_t klen = std::strlen(evt->header_key);
            const std::size_t vlen = std::strlen(evt->header_value);
            const std::size_t needed = klen + 2 + vlen + 2;

            if (s->headers_block.size() + needed <= s->header_cap) {
                s->headers_block.append(evt->header_key, klen);
                s->headers_block.append(": ", 2);
                s->headers_block.append(evt->header_value, vlen);
                s->headers_block.append("\r\n");
            } else {
                s->header_cap_reached = true;
            }
        }

        give_mutex(s->meta_mutex);
        break;
    }

    case HTTP_EVENT_ON_DATA: {
        // For HEAD, ignore the body entirely (but allow perform() to drain).
        if (s->method == 5) {
            break;
        }

        if (evt->data && evt->data_len > 0) {
            const auto* p = static_cast<const std::uint8_t*>(evt->data);
            if (!stream_send_all(*s, p, static_cast<std::size_t>(evt->data_len))) {
                return ESP_FAIL;
            }
        }
        break;
    }

    case HTTP_EVENT_ON_FINISH:
    case HTTP_EVENT_DISCONNECTED: {
        take_mutex(s->meta_mutex);
        set_status_and_length(*s);
        s->done = true;
        give_mutex(s->meta_mutex);
        break;
    }

    default:
        break;
    }

    return ESP_OK;
}

static void http_task_entry(void* arg)
{
    auto* s = static_cast<HttpNetworkProtocolEspIdfState*>(arg);
    if (!s) {
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = ESP_FAIL;
    if (s->client) {
        err = esp_http_client_perform(s->client);
    }

    take_mutex(s->meta_mutex);
    s->err = err;
    set_status_and_length(*s);
    s->done = true;
    give_mutex(s->meta_mutex);

    // Notify close() that we're done.
    if (s->done_sem) {
        (void)xSemaphoreGive(s->done_sem);
    }

    vTaskDelete(nullptr);
}

static void http_task_entry_after_upload(void* arg)
{
    auto* s = static_cast<HttpNetworkProtocolEspIdfState*>(arg);
    if (!s) {
        vTaskDelete(nullptr);
        return;
    }

    esp_err_t err = ESP_FAIL;

    if (s->client) {
        // This parses response headers and makes status/length available.
        (void)esp_http_client_fetch_headers(s->client);
        take_mutex(s->meta_mutex);
        set_status_and_length(*s);
        give_mutex(s->meta_mutex);

        // Read response body and push into stream buffer (bounded backpressure).
        while (!s->stop_requested) {
            std::uint8_t buf[512];
            const int r = esp_http_client_read(s->client, reinterpret_cast<char*>(buf), sizeof(buf));
            if (r < 0) {
                err = ESP_FAIL;
                break;
            }
            if (r == 0) {
                err = ESP_OK;
                break;
            }
            if (!stream_send_all(*s, buf, static_cast<std::size_t>(r))) {
                err = ESP_FAIL;
                break;
            }
        }

        // Close the connection now that response is consumed (or on error).
        (void)esp_http_client_close(s->client);
    }

    take_mutex(s->meta_mutex);
    s->err = err;
    // Status/length already captured immediately after fetch_headers() while client was open.
    s->done = true;
    give_mutex(s->meta_mutex);
    
    if (s->done_sem) {
        (void)xSemaphoreGive(s->done_sem);
    }
    vTaskDelete(nullptr);
}

HttpNetworkProtocolEspIdf::HttpNetworkProtocolEspIdf()
{
    _s = new HttpNetworkProtocolEspIdfState();
    _s->meta_mutex = xSemaphoreCreateMutexStatic(&_s->meta_mutex_storage);
    _s->done_sem = xSemaphoreCreateBinaryStatic(&_s->done_sem_storage);
    _s->stream = xStreamBufferCreateStatic(HttpNetworkProtocolEspIdfState::rb_size,
                                           1,
                                           _s->stream_buf,
                                           &_s->stream_storage);
    _s->headers_block.reserve(HttpNetworkProtocolEspIdfState::header_cap_default);
}

HttpNetworkProtocolEspIdf::~HttpNetworkProtocolEspIdf()
{
    close();
    delete _s;
    _s = nullptr;
}

fujinet::io::StatusCode HttpNetworkProtocolEspIdf::open(const fujinet::io::NetworkOpenRequest& req)
{
    close();

    if (!_s) {
        return fujinet::io::StatusCode::InternalError;
    }

    if (!method_supported(req.method)) {
        return fujinet::io::StatusCode::Unsupported;
    }

    _s->reset_session_state();
    _s->method = req.method;
    _s->want_headers = (req.flags & 0x04) != 0;

    // reset buffers
    if (_s->stream) {
        (void)xStreamBufferReset(_s->stream);
    }

    take_mutex(_s->meta_mutex);
    _s->headers_block.clear();
    _s->header_cap_reached = false;
    _s->has_http_status = false;
    _s->has_content_length = false;
    _s->done = false;
    _s->err = ESP_OK;
    give_mutex(_s->meta_mutex);

    esp_http_client_config_t cfg{};
    cfg.url = req.url.c_str();
    cfg.event_handler = &event_handler;
    cfg.user_data = _s;
    cfg.buffer_size = 1024;
    cfg.timeout_ms = 15000;

    const bool follow = (req.flags & 0x02) != 0;
    cfg.disable_auto_redirect = follow ? false : true;

    _s->client = esp_http_client_init(&cfg);
    if (!_s->client) {
        return fujinet::io::StatusCode::InternalError;
    }

    switch (req.method) {
        case 1: esp_http_client_set_method(_s->client, HTTP_METHOD_GET); break;
        case 2: esp_http_client_set_method(_s->client, HTTP_METHOD_POST); break;
        case 3: esp_http_client_set_method(_s->client, HTTP_METHOD_PUT); break;
        case 4: esp_http_client_set_method(_s->client, HTTP_METHOD_DELETE); break;
        case 5: esp_http_client_set_method(_s->client, HTTP_METHOD_HEAD); break;
        default:
            esp_http_client_cleanup(_s->client);
            _s->client = nullptr;
            return fujinet::io::StatusCode::Unsupported;
    }
    
    for (const auto& kv : req.headers) {
        if (!kv.first.empty()) {
            (void)esp_http_client_set_header(_s->client, kv.first.c_str(), kv.second.c_str());
        }
    }

    const bool is_post_or_put = (req.method == 2 || req.method == 3);
    _s->has_request_body = is_post_or_put && (req.bodyLenHint > 0);
    _s->expected_body_len = _s->has_request_body ? req.bodyLenHint : 0;
    _s->sent_body_len = 0;
    _s->upload_open = false;

    // For POST/PUT with bodyLenHint>0, open a streaming upload connection and defer the task
    // until the final body byte is written (write_body()).
    if (_s->has_request_body) {
        esp_err_t e = esp_http_client_open(_s->client, static_cast<int>(_s->expected_body_len));
        if (e != ESP_OK) {
            esp_http_client_cleanup(_s->client);
            _s->client = nullptr;
            return fujinet::io::StatusCode::IOError;
        }
        _s->upload_open = true;
        // No task started yet; response will be pulled after upload completes.
        return fujinet::io::StatusCode::Ok;
    }

    // No request body: launch task to perform request asynchronously.
    _s->task = nullptr;
    if (_s->done_sem) { (void)xSemaphoreTake(_s->done_sem, 0); }

    BaseType_t ok = xTaskCreate(&http_task_entry, "fn_http", 4096, _s, 3, &_s->task);
    if (ok != pdPASS || !_s->task) {
        esp_http_client_cleanup(_s->client);
        _s->client = nullptr;
        return fujinet::io::StatusCode::InternalError;
    }
    return fujinet::io::StatusCode::Ok;

}

fujinet::io::StatusCode HttpNetworkProtocolEspIdf::write_body(
    std::uint32_t offset,
    const std::uint8_t* data,
    std::size_t dataLen,
    std::uint16_t& written
) {
    written = 0;

    if (!_s || !_s->client) {
        return fujinet::io::StatusCode::InternalError;
    }
    if (!_s->has_request_body || !_s->upload_open) {
        return fujinet::io::StatusCode::Unsupported;
    }

    // Core enforces sequential offsets/overflow too, but keep backend defensive.
    if (offset != _s->sent_body_len) {
        return fujinet::io::StatusCode::InvalidRequest;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(dataLen);
    if (end > static_cast<std::uint64_t>(_s->expected_body_len)) {
        return fujinet::io::StatusCode::InvalidRequest;
    }
    if (dataLen > 0 && !data) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    // Stream write to esp_http_client (may accept partial); we loop to send all or return DeviceBusy/IOError.
    std::size_t total = 0;
    while (total < dataLen) {
        if (_s->stop_requested) {
            return fujinet::io::StatusCode::IOError;
        }

        const int w = esp_http_client_write(
            _s->client,
            reinterpret_cast<const char*>(data + total),
            static_cast<int>(dataLen - total)
        );

        if (w < 0) {
            return fujinet::io::StatusCode::IOError;
        }
        if (w == 0) {
            // Backpressure (rare); let host retry.
            written = static_cast<std::uint16_t>(total);
            return fujinet::io::StatusCode::DeviceBusy;
        }
        total += static_cast<std::size_t>(w);
    }

    _s->sent_body_len += static_cast<std::uint32_t>(total);
    written = static_cast<std::uint16_t>(total);

    // Body complete: start the "read response" task now.
    if (_s->sent_body_len == _s->expected_body_len) {
        _s->task = nullptr;
        if (_s->done_sem) { (void)xSemaphoreTake(_s->done_sem, 0); }

        BaseType_t ok = xTaskCreate(&http_task_entry_after_upload, "fn_http2", 4096, _s, 3, &_s->task);
        if (ok != pdPASS || !_s->task) {
            return fujinet::io::StatusCode::InternalError;
        }
    }

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode HttpNetworkProtocolEspIdf::read_body(std::uint32_t offset,
                                                             std::uint8_t *out,
                                                             std::size_t outLen,
                                                             std::uint16_t &read,
                                                             bool &eof)
{
    read = 0;
    eof = false;

    if (!_s || !out)
    {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    // v1 semantics: sequential offsets only.
    if (offset != _s->read_cursor)
    {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    if (!_s->stream || outLen == 0)
    {
        // If we can't read, treat this as "check done" / "not ready".
        take_mutex(_s->meta_mutex);
        const bool done = _s->done;
        const esp_err_t err = _s->err;
        give_mutex(_s->meta_mutex);

        if (done)
        {
            eof = true;
            return (err == ESP_OK) ? fujinet::io::StatusCode::Ok
                                   : fujinet::io::StatusCode::IOError;
        }
        return fujinet::io::StatusCode::NotReady;
    }

    const std::size_t max_n = std::min<std::size_t>(outLen, 0xFFFFu);

    // Small wait so we don't force the host into "timeout-driven polling".
    constexpr TickType_t kWaitTicks = pdMS_TO_TICKS(20);

    const std::size_t n = xStreamBufferReceive(_s->stream, out, max_n, kWaitTicks);

    if (n > 0)
    {
        _s->read_cursor += static_cast<std::uint32_t>(n);
        read = static_cast<std::uint16_t>(n);

        // If we just drained the buffer, check if the transfer is finished.
        // There is a race: the producer may not have set done=true yet.
        const std::size_t avail = xStreamBufferBytesAvailable(_s->stream);
        if (avail == 0)
        {
            take_mutex(_s->meta_mutex);
            bool done = _s->done;
            esp_err_t err = _s->err;
            give_mutex(_s->meta_mutex);

            if (!done && _s->done_sem)
            {
                // Wait a *tiny* amount for the HTTP task to finish so small responses
                // can return eof=true in the same READ.
                constexpr TickType_t kDoneWait = pdMS_TO_TICKS(5);
                if (xSemaphoreTake(_s->done_sem, kDoneWait) == pdTRUE)
                {
                    // Put it back so close() still works unchanged.
                    (void)xSemaphoreGive(_s->done_sem);

                    take_mutex(_s->meta_mutex);
                    done = _s->done;
                    err = _s->err;
                    give_mutex(_s->meta_mutex);
                }
            }

            if (done)
            {
                eof = true;
                return (err == ESP_OK) ? fujinet::io::StatusCode::Ok
                                       : fujinet::io::StatusCode::IOError;
            }
        }

        eof = false;
        return fujinet::io::StatusCode::Ok;
    }

    // No bytes this instant: if done, EOF; else NotReady.
    take_mutex(_s->meta_mutex);
    const bool done = _s->done;
    const esp_err_t err = _s->err;
    give_mutex(_s->meta_mutex);

    if (done)
    {
        eof = true;
        return (err == ESP_OK) ? fujinet::io::StatusCode::Ok
                               : fujinet::io::StatusCode::IOError;
    }

    return fujinet::io::StatusCode::NotReady;
}

fujinet::io::StatusCode HttpNetworkProtocolEspIdf::info(std::size_t maxHeaderBytes, fujinet::io::NetworkInfo& out)
{
    out = fujinet::io::NetworkInfo{};

    if (!_s) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    take_mutex(_s->meta_mutex);

    const bool has_status = _s->has_http_status;
    const std::uint16_t http_status = _s->http_status;
    const bool has_len = _s->has_content_length;
    const std::uint64_t len = _s->content_length;
    const bool done = _s->done;
    const esp_err_t err = _s->err;

    std::string hdr;
    if (_s->want_headers && !_s->headers_block.empty() && maxHeaderBytes > 0) {
        const std::size_t n = std::min<std::size_t>(_s->headers_block.size(), maxHeaderBytes);
        hdr.assign(_s->headers_block.data(), n);
    }

    give_mutex(_s->meta_mutex);

    if (!has_status) {
        // If the transfer failed before status became known, report IOError once it's done.
        if (done && err != ESP_OK) {
            return fujinet::io::StatusCode::IOError;
        }
        return fujinet::io::StatusCode::NotReady;
    }

    out.hasHttpStatus = true;
    out.httpStatus = http_status;
    out.hasContentLength = has_len;
    out.contentLength = has_len ? len : 0;
    out.headersBlock = std::move(hdr);
    return fujinet::io::StatusCode::Ok;
}

void HttpNetworkProtocolEspIdf::poll()
{
    // Keep lightweight; esp_http_client runs in the dedicated task.
}

void HttpNetworkProtocolEspIdf::close()
{
    if (!_s) {
        return;
    }

    _s->stop_requested = true;

    // Best-effort: close the client to break esp_http_client_perform()
    if (_s->client) {
        (void)esp_http_client_close(_s->client);
    }

    // Wait briefly for task to finish (if any). We use a notify on the task handle itself.
    if (_s->task && _s->done_sem) {
        // Wait up to ~1s total.
        (void)xSemaphoreTake(_s->done_sem, pdMS_TO_TICKS(1000));
    }

    if (_s->client) {
        esp_http_client_cleanup(_s->client);
        _s->client = nullptr;
    }

    _s->task = nullptr;

    if (_s->stream) {
        (void)xStreamBufferReset(_s->stream);
    }

    take_mutex(_s->meta_mutex);
    _s->headers_block.clear();
    _s->reset_session_state();
    give_mutex(_s->meta_mutex);

    FN_LOGD(TAG, "close done");
}

} // namespace fujinet::platform::esp32


