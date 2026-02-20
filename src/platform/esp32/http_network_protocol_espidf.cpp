#include "fujinet/platform/esp32/http_network_protocol_espidf.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "fujinet/core/logging.h"
#include "fujinet/net/test_ca_cert.h"

extern "C" {
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

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
    bool body_unknown_len{false};

    bool has_http_status{false};
    std::uint16_t http_status{0};

    bool has_content_length{false};
    std::uint64_t content_length{0};

    bool done{false};
    esp_err_t err{ESP_OK};

    std::uint32_t read_cursor{0};
    volatile bool stop_requested{false};
    volatile bool cancel_requested{false};
    volatile bool cleanup_pending{false};

    // Intrusive lifetime management:
    // - protocol object owns 1 ref
    // - HTTP task owns 1 ref while running
    // This prevents use-after-free / double-free when sessions are closed/evicted
    // while the esp_http_client task is still running.
    std::atomic<std::uint32_t> refcnt{1};


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
        body_unknown_len = false;

        suppress_on_data = false;
    }

    std::string headers_block;
};

static void state_acquire(HttpNetworkProtocolEspIdfState* s)
{
    if (!s) return;
    s->refcnt.fetch_add(1, std::memory_order_relaxed);
}

static void state_release(HttpNetworkProtocolEspIdfState* s)
{
    if (!s) return;
    const std::uint32_t prev = s->refcnt.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        delete s;
    }
}

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

    // Cache semaphore handle early (it lives in s).
    SemaphoreHandle_t done_sem = s->done_sem;

    esp_http_client_handle_t client = nullptr;

    // Snapshot client pointer under mutex.
    take_mutex(s->meta_mutex);
    client = s->client;
    give_mutex(s->meta_mutex);

    esp_err_t err = ESP_FAIL;
    if (client) {
        err = esp_http_client_perform(client);
    }

    // Update result metadata under mutex (but don't call set_status_and_length if client is gone).
    take_mutex(s->meta_mutex);
    s->err = err;
    set_status_and_length(*s);
    s->done = true;
    give_mutex(s->meta_mutex);

    // TASK-OWNED CLEANUP (critical): do this once and publish nullptrs under mutex.
    if (client) {
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    take_mutex(s->meta_mutex);
    // Only clear if still pointing to the same handle (defensive).
    if (s->client == client) {
        s->client = nullptr;
    }
    s->task = nullptr;
    give_mutex(s->meta_mutex);

    if (done_sem) {
        (void)xSemaphoreGive(done_sem);
    }

    // Release task's reference.
    state_release(s);

    vTaskDelete(nullptr);
}



static void http_task_entry_after_upload(void* arg)
{
    auto* s = static_cast<HttpNetworkProtocolEspIdfState*>(arg);
    if (!s) {
        vTaskDelete(nullptr);
        return;
    }

    // Cache semaphore handle early (it lives in s).
    SemaphoreHandle_t done_sem = s->done_sem;

    esp_http_client_handle_t client = nullptr;

    take_mutex(s->meta_mutex);
    client = s->client;
    give_mutex(s->meta_mutex);

    esp_err_t err = ESP_FAIL;

    if (client) {
        // Parse headers and make status/length available.
        (void)esp_http_client_fetch_headers(client);

        take_mutex(s->meta_mutex);
        set_status_and_length(*s);
        give_mutex(s->meta_mutex);

        // Read response body and push into stream buffer.
        while (!s->stop_requested) {
            std::uint8_t buf[512];
            const int r = esp_http_client_read(client, reinterpret_cast<char*>(buf), sizeof(buf));

            if (r < 0) { err = ESP_FAIL; break; }
            if (r == 0) { err = ESP_OK;   break; }

            if (!stream_send_all(*s, buf, static_cast<std::size_t>(r))) {
                err = ESP_FAIL;
                break;
            }
        }
    }

    take_mutex(s->meta_mutex);
    s->err = err;
    s->done = true;
    give_mutex(s->meta_mutex);

    if (client) {
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    take_mutex(s->meta_mutex);
    if (s->client == client) {
        s->client = nullptr;
    }
    s->task = nullptr;
    give_mutex(s->meta_mutex);

    if (done_sem) {
        (void)xSemaphoreGive(done_sem);
    }

    // Release task's reference.
    state_release(s);

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
    if (!_s) {
        return;
    }

    close();
    // Release protocol's reference (task may still hold the other reference).
    state_release(_s);
    _s = nullptr;
}

fujinet::io::StatusCode HttpNetworkProtocolEspIdf::open(const fujinet::io::NetworkOpenRequest& req)
{
    if (!_s) {
        return fujinet::io::StatusCode::InternalError;
    }

    // If a previous request is still alive, do not reuse state.
    // IMPORTANT: we must not call close() here because close() is "request stop"
    // while a task owns client lifetime.
    take_mutex(_s->meta_mutex);
    const bool busy = (_s->task != nullptr) || (_s->client != nullptr);
    give_mutex(_s->meta_mutex);

    if (busy) {
        return fujinet::io::StatusCode::DeviceBusy;
    }

    if (!method_supported(req.method)) {
        return fujinet::io::StatusCode::Unsupported;
    }

    // Local failure helper: reset buffers + session-visible metadata.
    // NOTE: This does NOT destroy esp_http_client (callers do that explicitly),
    // and it does NOT assume a task exists (it must not).
    auto fail = [&](fujinet::io::StatusCode sc) -> fujinet::io::StatusCode {
        _s->stop_requested = true; // prevent any future streaming

        // No task should exist in open() failure paths (task only created at end).
        _s->task = nullptr;

        if (_s->stream) {
            (void)xStreamBufferReset(_s->stream);
        }

        take_mutex(_s->meta_mutex);
        _s->headers_block.clear();
        _s->has_http_status = false;
        _s->http_status = 0;
        _s->has_content_length = false;
        _s->content_length = 0;
        _s->done = true;
        _s->err = ESP_FAIL;
        give_mutex(_s->meta_mutex);

        return sc;
    };

    // Fresh session state (exactly once).
    _s->reset_session_state();
    _s->method = req.method;
    _s->response_header_names_lower = req.responseHeaderNamesLower;

    // Reset stream buffer for this session.
    if (_s->stream) {
        (void)xStreamBufferReset(_s->stream);
    }

    // Reset metadata for this session.
    take_mutex(_s->meta_mutex);
    _s->headers_block.clear();
    _s->has_http_status = false;
    _s->has_content_length = false;
    _s->done = false;
    _s->err = ESP_OK;
    give_mutex(_s->meta_mutex);

    // Check for test CA flag in URL (https://host:port/path?testca=1)
    // This uses the embedded FujiNet Test CA for verifying self-signed certs
    // generated with integration-tests/certs/generate_certs.sh
    bool use_test_ca = false;
    bool insecure = false;
    std::string url = req.url;
    
    // Check for testca flag
    size_t queryPos = url.find("?testca=1");
    if (queryPos != std::string::npos) {
        use_test_ca = true;
        url = url.substr(0, queryPos);  // Remove ?testca=1 from URL
        FN_LOGI(TAG, "HTTPS: Using FujiNet Test CA for certificate verification");
    }
    
    // Check for insecure flag (deprecated, but kept for compatibility)
    queryPos = url.find("?insecure=1");
    if (queryPos != std::string::npos) {
        insecure = true;
        url = url.substr(0, queryPos);  // Remove ?insecure=1 from URL
        FN_LOGW(TAG, "HTTPS: Certificate verification DISABLED (insecure mode)");
    }
    
    esp_http_client_config_t cfg{};
    cfg.url = url.c_str();
    cfg.event_handler = &event_handler;
    cfg.user_data = _s;

    // RX buffer (response parsing / body chunks)
    cfg.buffer_size = 2048;

    // TX buffer (request headers). ESP-IDF logs "HTTP_HEADER: Buffer length is small..."
    // if this is too small for request headers.
    cfg.buffer_size_tx = 4096;

    cfg.timeout_ms = 15000;

    // TLS certificate verification configuration
    if (use_test_ca) {
        // Use embedded FujiNet Test CA for self-signed cert verification
        cfg.cert_pem = fujinet::net::test_ca_cert_pem;
    } else if (insecure) {
        // Insecure mode: use cert bundle but skip CN verification
        // Note: This only skips CN check, NOT full chain verification
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.skip_cert_common_name_check = true;
    } else {
        // Normal mode: use ESP-IDF's built-in certificate bundle (Mozilla root CAs)
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    const bool follow = (req.flags & 0x02) != 0;
    cfg.disable_auto_redirect = follow ? false : true;

    _s->client = esp_http_client_init(&cfg);
    if (!_s->client) {
        return fail(fujinet::io::StatusCode::InternalError);
    }

    // Set method
    switch (req.method) {
        case 1: esp_http_client_set_method(_s->client, HTTP_METHOD_GET); break;
        case 2: esp_http_client_set_method(_s->client, HTTP_METHOD_POST); break;
        case 3: esp_http_client_set_method(_s->client, HTTP_METHOD_PUT); break;
        case 4: esp_http_client_set_method(_s->client, HTTP_METHOD_DELETE); break;
        case 5: esp_http_client_set_method(_s->client, HTTP_METHOD_HEAD); break;
        default:
            // Should not happen due to method_supported(), but keep defensive.
            esp_http_client_cleanup(_s->client);
            _s->client = nullptr;
            return fail(fujinet::io::StatusCode::Unsupported);
    }

    // Apply request headers
    for (const auto& kv : req.headers) {
        if (kv.first.empty()) {
            continue;
        }

        const esp_err_t err = esp_http_client_set_header(_s->client, kv.first.c_str(), kv.second.c_str());
        if (err != ESP_OK) {
            FN_LOGE(TAG, "open: esp_http_client_set_header failed err=%d key=%s", (int)err, kv.first.c_str());
            esp_http_client_cleanup(_s->client);
            _s->client = nullptr;
            return fail(fujinet::io::StatusCode::InvalidRequest);
        }
    }

    // Upload/body streaming setup (POST/PUT only)
    const bool is_post_or_put = (req.method == 2 || req.method == 3);
    _s->body_unknown_len = is_post_or_put && (req.bodyLenHint == 0) && ((req.flags & 0x04) != 0);
    _s->has_request_body = is_post_or_put && (req.bodyLenHint > 0 || _s->body_unknown_len);
    _s->expected_body_len = (_s->has_request_body && !_s->body_unknown_len) ? req.bodyLenHint : 0;
    _s->sent_body_len = 0;
    _s->upload_open = false;

    if (_s->has_request_body) {
        const int open_len = _s->body_unknown_len ? -1 : static_cast<int>(_s->expected_body_len);
        const esp_err_t e = esp_http_client_open(_s->client, open_len);
        if (e != ESP_OK) {
            FN_LOGE(TAG, "open: esp_http_client_open failed err=%d", (int)e);
            esp_http_client_cleanup(_s->client);
            _s->client = nullptr;
            return fail(fujinet::io::StatusCode::IOError);
        }

        _s->upload_open = true;
        // No task started yet. Response will be pulled after upload completes.
        return fujinet::io::StatusCode::Ok;
    }

    // No request body: launch task to perform request asynchronously.
    _s->task = nullptr;
    if (_s->done_sem) {
        (void)xSemaphoreTake(_s->done_sem, 0); // drain
    }

    // Task owns a reference while it runs.
    state_acquire(_s);

    BaseType_t ok = xTaskCreate(&http_task_entry, "fn_http", 8192, _s, 3, &_s->task);
    if (ok != pdPASS || !_s->task) {
        // Release task ref we just acquired.
        state_release(_s);
        // No task exists => safe to cleanup here.
        esp_http_client_cleanup(_s->client);
        _s->client = nullptr;
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
    if (!_s->body_unknown_len) {
        const std::uint64_t end = static_cast<std::uint64_t>(offset) + static_cast<std::uint64_t>(dataLen);
        if (end > static_cast<std::uint64_t>(_s->expected_body_len)) {
            FN_LOGW(TAG, "write_body: overflow end=%llu expected=%u",
                (unsigned long long)end, (unsigned)_s->expected_body_len);
            return fujinet::io::StatusCode::InvalidRequest;
        }
    }
    if (dataLen > 0 && !data) {
        FN_LOGE(TAG, "write_body: null data with dataLen=%u", (unsigned)dataLen);
        return fujinet::io::StatusCode::InvalidRequest;
    }

    // IMPORTANT:
    // NetworkDevice's contract expects:
    //   - Ok  => written == dataLen (whole chunk accepted)
    //   - DeviceBusy => written == 0 (no progress; host retries same offset)
    //
    // esp_http_client_write() may accept partial writes, so we MUST NOT return
    // DeviceBusy after partial progress, or we'd wedge the session with a gap/dup.
    const int64_t t_enter = esp_timer_get_time();

    // Stream write to esp_http_client (may accept partial); we loop internally until
    // we send the whole piece or we fail/abort.
    std::size_t total = 0;
    int zero_writes = 0;

    auto write_all = [&](const std::uint8_t* p, std::size_t n) -> bool {
        std::size_t sent_total = 0;
        while (sent_total < n) {
            if (_s->stop_requested) {
                return false;
            }

            const int w = esp_http_client_write(
                _s->client,
                reinterpret_cast<const char*>(p + sent_total),
                static_cast<int>(n - sent_total)
            );

            if (w < 0) {
                return false;
            }
            if (w == 0) {
                ++zero_writes;
                vTaskDelay(pdMS_TO_TICKS(10));

                const int64_t now = esp_timer_get_time();
                const int64_t elapsed_ms = (now - t_enter) / 1000;
                if (elapsed_ms > 2000 || zero_writes > 200) {
                    _s->stop_requested = true;
                    (void)esp_http_client_close(_s->client);
                    return false;
                }
                continue;
            }

            sent_total += static_cast<std::size_t>(w);
        }
        return true;
    };

    if (_s->body_unknown_len) {
        // Unknown-length body: send chunked framing. Commit is signaled by a zero-length Write().
        if (dataLen == 0) {
            static constexpr std::uint8_t final_chunk[] = {'0','\r','\n','\r','\n'};
            if (!write_all(final_chunk, sizeof(final_chunk))) {
                return fujinet::io::StatusCode::IOError;
            }
        } else {
            char hdr[16];
            const int n = std::snprintf(hdr, sizeof(hdr), "%x\r\n", static_cast<unsigned>(dataLen));
            if (n <= 0 || static_cast<std::size_t>(n) >= sizeof(hdr)) {
                return fujinet::io::StatusCode::InternalError;
            }
            if (!write_all(reinterpret_cast<const std::uint8_t*>(hdr), static_cast<std::size_t>(n))) {
                return fujinet::io::StatusCode::IOError;
            }
            if (!write_all(data, dataLen)) {
                return fujinet::io::StatusCode::IOError;
            }
            static constexpr std::uint8_t crlf[] = {'\r','\n'};
            if (!write_all(crlf, sizeof(crlf))) {
                return fujinet::io::StatusCode::IOError;
            }
        }
        total = dataLen;
    } else {
        // Known-length upload: write raw bytes.
        while (total < dataLen) {
            if (_s->stop_requested) {
                FN_LOGW(TAG, "write_body: stop_requested");
                return fujinet::io::StatusCode::IOError;
            }

            const int w = esp_http_client_write(
                _s->client,
                reinterpret_cast<const char*>(data + total),
                static_cast<int>(dataLen - total)
            );

            if (w < 0) {
                FN_LOGE(TAG, "write_body: write error ret=%d", w);
                return fujinet::io::StatusCode::IOError;
            }
            if (w == 0) {
                // Backpressure. We keep trying here to preserve "whole chunk" semantics.
                ++zero_writes;
                vTaskDelay(pdMS_TO_TICKS(10));

                // If we've been blocked for too long, abort this session instead of wedging.
                const int64_t now = esp_timer_get_time();
                const int64_t elapsed_ms = (now - t_enter) / 1000;
                if (elapsed_ms > 2000 || zero_writes > 200) {
                    FN_LOGW(TAG, "write_body: backpressure timeout (ms=%lld, zero_writes=%d); aborting",
                            (long long)elapsed_ms, zero_writes);
                    _s->stop_requested = true;
                    (void)esp_http_client_close(_s->client);
                    return fujinet::io::StatusCode::IOError;
                }
                continue;
            }
            total += static_cast<std::size_t>(w);
        }
    }

    _s->sent_body_len += static_cast<std::uint32_t>(total);
    written = static_cast<std::uint16_t>(total);

    // Body complete: start the "read response" task now.
    // Guard: only start if upload_open is still true (prevents multiple task creation
    // if host sends multiple zero-length writes for unknown-length bodies).
    if (_s->upload_open &&
        ((!_s->body_unknown_len && _s->sent_body_len == _s->expected_body_len) ||
         (_s->body_unknown_len && dataLen == 0))) {
        FN_LOGI(TAG, "write_body: body complete, starting response task");
        // We manually read the response in http_task_entry_after_upload(),
        // so suppress event_handler's HTTP_EVENT_ON_DATA forwarding to avoid duplicates.
        _s->suppress_on_data = true;
        _s->upload_open = false;  // Prevent re-entry

        _s->task = nullptr;
        if (_s->done_sem) { (void)xSemaphoreTake(_s->done_sem, 0); }

        // Task owns a reference while it runs.
        state_acquire(_s);

        BaseType_t ok = xTaskCreate(&http_task_entry_after_upload, "fn_http2", 8192, _s, 3, &_s->task);
        if (ok != pdPASS || !_s->task) {
            state_release(_s);
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
                    // Signal consumed; don't give it back. We just wanted to reduce
                    // latency to EOF. The cleanup path will no longer rely on done_sem
                    // being available after this point.
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
    // no-op, task owns cleanup
}


void HttpNetworkProtocolEspIdf::close()
{
    if (!_s) return;

    _s->stop_requested = true;

    esp_http_client_handle_t client = nullptr;
    TaskHandle_t task = nullptr;

    take_mutex(_s->meta_mutex);
    client = _s->client;
    task = _s->task;
    give_mutex(_s->meta_mutex);

    // If task is running, wait for it to complete.
    // DO NOT call esp_http_client_close() here - it can cause heap corruption
    // if the task is in the middle of TLS setup.
    if (task) {
        FN_LOGW(TAG, "close: task running; waiting for task to complete");
        
        // Wait for task to finish (with timeout)
        // The task will clean up the client itself
        if (_s->done_sem) {
            // Wait up to 2 seconds for the task to complete
            (void)xSemaphoreTake(_s->done_sem, pdMS_TO_TICKS(2000));
        }
        
        // Small delay to let task clean up
        vTaskDelay(pdMS_TO_TICKS(10));
        
        // Task has cleaned up - re-read client pointer (should be nullptr now)
        take_mutex(_s->meta_mutex);
        _s->task = nullptr;
        client = _s->client;  // Re-read after task cleanup
        give_mutex(_s->meta_mutex);
        
        FN_LOGD(TAG, "close: task completed, client=%p", client);
    }

    // Only clean up if client is still valid (task didn't clean it up)
    if (client) {
        FN_LOGD(TAG, "close: cleaning up client");
        (void)esp_http_client_close(client);
        esp_http_client_cleanup(client);

        take_mutex(_s->meta_mutex);
        if (_s->client == client) {
            _s->client = nullptr;
        }
        give_mutex(_s->meta_mutex);
    }

    if (_s->stream) {
        (void)xStreamBufferReset(_s->stream);
    }

    take_mutex(_s->meta_mutex);
    _s->headers_block.clear();
    _s->has_http_status = false;
    _s->http_status = 0;
    _s->has_content_length = false;
    _s->content_length = 0;
    _s->done = true;
    _s->err = ESP_OK;
    _s->task = nullptr;
    give_mutex(_s->meta_mutex);

    FN_LOGD(TAG, "close: cleaned up (no task)");
}


} // namespace fujinet::platform::esp32
