#include "fujinet/platform/esp32/http_network_protocol_espidf.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "fujinet/core/logging.h"

extern "C" {
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_http_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
}

namespace fujinet::platform::esp32 {

static constexpr const char* TAG = "platform";

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
    static constexpr std::size_t rb_size = 8192;
    static constexpr TickType_t wait_step_ticks = pdMS_TO_TICKS(50);

    bool suppress_on_data = false;

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

    // NEW: response header allowlist (lowercase). If empty => store nothing.
    std::vector<std::string> response_header_names_lower;

    // request & state
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

    void reset_session_state() {
        response_header_names_lower.clear();

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

        suppress_on_data = false;
    }

    std::string headers_block;
};

static std::string to_lower_ascii(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

static bool header_requested(const HttpNetworkProtocolEspIdfState& s, const char* key)
{
    if (!key) return false;
    if (s.response_header_names_lower.empty()) return false;

    const std::string k = to_lower_ascii(key);
    for (const auto& want : s.response_header_names_lower) {
        if (want == k) return true;
    }
    return false;
}

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
        // Store ONLY requested headers. If none requested, store nothing.
        if (evt->header_key && evt->header_value && header_requested(*s, evt->header_key)) {
            const std::size_t klen = std::strlen(evt->header_key);
            const std::size_t vlen = std::strlen(evt->header_value);
    
            take_mutex(s->meta_mutex);
            s->headers_block.append(evt->header_key, klen);
            s->headers_block.append(": ", 2);
            s->headers_block.append(evt->header_value, vlen);
            s->headers_block.append("\r\n");
            give_mutex(s->meta_mutex);
        }
        break;
    }

    case HTTP_EVENT_ON_DATA: {
        // For HEAD, ignore the body entirely (but allow perform() to drain).
        if (s->method == 5) {
            break;
        }
    
        // POST/PUT upload path: the after-upload task manually reads the response
        // using esp_http_client_read(). If we also forward ON_DATA here, the body
        // can be duplicated.
        if (s->suppress_on_data) {
            FN_LOGI(TAG, "event_handler: ON_DATA suppressed (len=%d)", evt->data_len);
            break;
        }
    
        if (evt->data && evt->data_len > 0) {
            FN_LOGI(TAG, "event_handler: ON_DATA forward len=%d", evt->data_len);
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
    _s->headers_block.reserve(256);
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

    // If close() deferred cleanup, we may still be busy. Don't reuse the state.
    if (_s->task || _s->client) {
        // Previous request still in-flight; caller should retry later.
        return fujinet::io::StatusCode::DeviceBusy;
    }

    if (!method_supported(req.method)) {
        return fujinet::io::StatusCode::Unsupported;
    }

    auto fail = [&](fujinet::io::StatusCode sc) -> fujinet::io::StatusCode {
        // Stop any in-flight activity (should be none during open(), but keep it safe).
        _s->stop_requested = true;

        if (_s->client) {
            (void)esp_http_client_close(_s->client);
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

        return sc;
    };


    _s->reset_session_state();
    _s->method = req.method;
    _s->response_header_names_lower = req.responseHeaderNamesLower;


    // reset buffers
    if (_s->stream) {
        (void)xStreamBufferReset(_s->stream);
    }

    take_mutex(_s->meta_mutex);
    _s->headers_block.clear();
    _s->has_http_status = false;
    _s->has_content_length = false;
    _s->done = false;
    _s->err = ESP_OK;
    give_mutex(_s->meta_mutex);

    esp_http_client_config_t cfg{};
    cfg.url = req.url.c_str();
    cfg.event_handler = &event_handler;
    cfg.user_data = _s;

    // RX buffer (response parsing / body chunks)
    cfg.buffer_size = 1024;

    // TX buffer (request headers). ESP-IDF will log:
    // "HTTP_HEADER: Buffer length is small to fit all the headers"
    // if this is too small.
    cfg.buffer_size_tx = 2048;

    cfg.timeout_ms = 15000;

    const bool follow = (req.flags & 0x02) != 0;
    cfg.disable_auto_redirect = follow ? false : true;

    _s->client = esp_http_client_init(&cfg);
    if (!_s->client) {
        return fail(fujinet::io::StatusCode::InternalError);
    }

    switch (req.method) {
        case 1: esp_http_client_set_method(_s->client, HTTP_METHOD_GET); break;
        case 2: esp_http_client_set_method(_s->client, HTTP_METHOD_POST); break;
        case 3: esp_http_client_set_method(_s->client, HTTP_METHOD_PUT); break;
        case 4: esp_http_client_set_method(_s->client, HTTP_METHOD_DELETE); break;
        case 5: esp_http_client_set_method(_s->client, HTTP_METHOD_HEAD); break;
        default:
            return fail(fujinet::io::StatusCode::Unsupported);
    }
    
    for (const auto& kv : req.headers) {
        if (!kv.first.empty()) {
            const esp_err_t err = esp_http_client_set_header(_s->client, kv.first.c_str(), kv.second.c_str());
            if (err != ESP_OK) {
                FN_LOGE(TAG, "open: esp_http_client_set_header failed err=%d key=%s", (int)err, kv.first.c_str());
                return fail(fujinet::io::StatusCode::InvalidRequest);
            }
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
        const esp_err_t e = esp_http_client_open(_s->client, static_cast<int>(_s->expected_body_len));
        if (e != ESP_OK) {
            FN_LOGE(TAG, "open: esp_http_client_open failed err=%d", (int)e);
            return fail(fujinet::io::StatusCode::IOError);
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
        return fail(fujinet::io::StatusCode::InternalError);
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
        FN_LOGE(TAG, "write_body: no state/client");
        return fujinet::io::StatusCode::InternalError;
    }
    if (!_s->has_request_body || !_s->upload_open) {
        FN_LOGW(TAG, "write_body: unsupported (has_body=%d upload_open=%d)",
                    (int)_s->has_request_body, (int)_s->upload_open);
        return fujinet::io::StatusCode::Unsupported;
    }

    // Core enforces sequential offsets/overflow too, but keep backend defensive.
    if (offset != _s->sent_body_len) {
        FN_LOGW(TAG, "write_body: offset mismatch offset=%u sent=%u",
            (unsigned)offset, (unsigned)_s->sent_body_len);
        return fujinet::io::StatusCode::InvalidRequest;
    }
    const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(dataLen);
    if (end > static_cast<std::uint64_t>(_s->expected_body_len)) {
        FN_LOGW(TAG, "write_body: overflow end=%llu expected=%u",
            (unsigned long long)end, (unsigned)_s->expected_body_len);
        return fujinet::io::StatusCode::InvalidRequest;
    }
    if (dataLen > 0 && !data) {
        FN_LOGE(TAG, "write_body: null data with dataLen=%u", (unsigned)dataLen);
        return fujinet::io::StatusCode::InvalidRequest;
    }

    const int64_t t_enter = esp_timer_get_time();
    FN_LOGI(TAG, "write_body: enter off=%u len=%u expected=%u sent=%u",
                 (unsigned)offset, (unsigned)dataLen,
                 (unsigned)_s->expected_body_len, (unsigned)_s->sent_body_len);

    // Stream write to esp_http_client (may accept partial); we loop to send all or return DeviceBusy/IOError.
    std::size_t total = 0;
    while (total < dataLen) {
        if (_s->stop_requested) {
            FN_LOGW(TAG, "write_body: stop_requested");
            return fujinet::io::StatusCode::IOError;
        }

        const int64_t t0 = esp_timer_get_time();
        const int w = esp_http_client_write(
            _s->client,
            reinterpret_cast<const char*>(data + total),
            static_cast<int>(dataLen - total)
        );
        const int64_t t1 = esp_timer_get_time();
        FN_LOGI(TAG, "write_body: esp_http_client_write req=%u ret=%d dt_ms=%lld",
            (unsigned)(dataLen - total), w, (long long)((t1 - t0) / 1000));

        if (w < 0) {
            FN_LOGE(TAG, "write_body: write error ret=%d", w);
            return fujinet::io::StatusCode::IOError;
        }
        if (w == 0) {
            // Backpressure (rare); let host retry.
            written = static_cast<std::uint16_t>(total);
            FN_LOGW(TAG, "write_body: backpressure total=%u", (unsigned)total);
            return fujinet::io::StatusCode::DeviceBusy;
        }
        total += static_cast<std::size_t>(w);
    }

    _s->sent_body_len += static_cast<std::uint32_t>(total);
    written = static_cast<std::uint16_t>(total);

    const int64_t t_exit = esp_timer_get_time();
    FN_LOGI(TAG, "write_body: exit wrote=%u new_sent=%u dt_ms=%lld",
                 (unsigned)written, (unsigned)_s->sent_body_len,
                 (long long)((t_exit - t_enter) / 1000));

    // Body complete: start the "read response" task now.
    if (_s->sent_body_len == _s->expected_body_len) {
        FN_LOGI(TAG, "write_body: body complete, starting response task");
        // We manually read the response in http_task_entry_after_upload(),
        // so suppress event_handler's HTTP_EVENT_ON_DATA forwarding to avoid duplicates.
        _s->suppress_on_data = true;

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

fujinet::io::StatusCode HttpNetworkProtocolEspIdf::info(fujinet::io::NetworkInfo& out)
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

    // NEW: already filtered while receiving; return whole block (may be empty).
    std::string hdr = _s->headers_block;

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
    if (!_s) return;

    // If there is a task, check if it has completed. When complete, we can safely
    // cleanup the esp_http_client handle and reset session state.
    if (_s->task && _s->done_sem) {
        // Non-blocking check
        if (xSemaphoreTake(_s->done_sem, 0) == pdTRUE) {
            // Put it back so read_body() "tiny wait" logic remains harmless,
            // and close() can also observe completion if called later.
            (void)xSemaphoreGive(_s->done_sem);

            // Now it is safe to cleanup (the task has finished and signaled).
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

            FN_LOGD(TAG, "poll: cleaned up completed HTTP task");
        }
    }
}

void HttpNetworkProtocolEspIdf::close()
{
    if (!_s) return;

    _s->stop_requested = true;

    // Best-effort: request the client to close the underlying connection.
    // IMPORTANT: we must NOT call esp_http_client_cleanup() while a task may still
    // be inside esp_http_client_perform(), or we risk UAF crashes.
    if (_s->client) {
        (void)esp_http_client_close(_s->client);
    }

    // If there is no task, we can cleanup immediately.
    if (!_s->task) {
        if (_s->client) {
            esp_http_client_cleanup(_s->client);
            _s->client = nullptr;
        }

        if (_s->stream) {
            (void)xStreamBufferReset(_s->stream);
        }

        take_mutex(_s->meta_mutex);
        _s->headers_block.clear();
        _s->reset_session_state();
        give_mutex(_s->meta_mutex);

        FN_LOGD(TAG, "close: cleaned up (no task)");
        return;
    }

    // There IS a task. Wait a *short* time for completion; if not finished, defer
    // cleanup to poll() to avoid freeing client while perform() is running.
    bool finished = false;
    if (_s->done_sem) {
        if (xSemaphoreTake(_s->done_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
            // Put it back so other code paths can observe completion.
            (void)xSemaphoreGive(_s->done_sem);
            finished = true;
        }
    }

    if (!finished) {
        // Defer cleanup to poll(). Keep client/task pointers intact.
        FN_LOGW(TAG, "close: task still running, deferring cleanup to poll()");
        return;
    }

    // Finished: safe to cleanup now (same logic as poll()).
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

    FN_LOGI(TAG, "HttpNetworkProtocolEspIdf::close: cleaned up (task finished)");
}



} // namespace fujinet::platform::esp32


