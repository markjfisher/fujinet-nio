#include "fujinet/platform/esp32/tls_network_protocol_espidf.h"

#include <algorithm>
#include <cstring>

#include "fujinet/core/logging.h"

extern "C" {
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
}

namespace fujinet::platform::esp32 {

static constexpr const char* TAG = "platform";
static constexpr std::size_t RX_BUFFER_SIZE = 8192;
static constexpr int CONNECT_TIMEOUT_MS = 10000;
static constexpr int IO_TIMEOUT_MS = 100;

TlsNetworkProtocolEspIdf::TlsNetworkProtocolEspIdf()
    : _rxBuffer(RX_BUFFER_SIZE)
{
}

TlsNetworkProtocolEspIdf::~TlsNetworkProtocolEspIdf()
{
    close();
}

void TlsNetworkProtocolEspIdf::reset_state()
{
    _state = State::Idle;
    _rxAvailable = 0;
    _readCursor = 0;
    _writeCursor = 0;
    _lastError = 0;
    _peerClosed = false;
}

void TlsNetworkProtocolEspIdf::handle_error(int err)
{
    _lastError = err;
    _state = State::Error;
    FN_LOGE(TAG, "TLS error: %d", err);
}

bool TlsNetworkProtocolEspIdf::parse_tls_url(const std::string& url,
                                             std::string& outHost,
                                             std::uint16_t& outPort)
{
    // Expected format: tls://host:port or tls://host (default port 443)
    const std::string prefix = "tls://";
    if (url.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }

    std::string hostPort = url.substr(prefix.size());
    if (hostPort.empty()) {
        return false;
    }

    // Find port separator
    auto colonPos = hostPort.rfind(':');
    if (colonPos != std::string::npos) {
        outHost = hostPort.substr(0, colonPos);
        std::string portStr = hostPort.substr(colonPos + 1);
        
        // Parse port number
        try {
            unsigned long port = std::stoul(portStr);
            if (port == 0 || port > 65535) {
                return false;
            }
            outPort = static_cast<std::uint16_t>(port);
        } catch (...) {
            return false;
        }
    } else {
        // Default TLS port
        outHost = hostPort;
        outPort = 443;
    }

    return !outHost.empty();
}

fujinet::io::StatusCode TlsNetworkProtocolEspIdf::open(const fujinet::io::NetworkOpenRequest& req)
{
    close();

    if (!parse_tls_url(req.url, _host, _port)) {
        FN_LOGE(TAG, "TLS: Invalid URL format: %s", req.url.c_str());
        return fujinet::io::StatusCode::InvalidRequest;
    }

    // Check for insecure flag in URL (tls://host:port?insecure=1)
    bool insecure = false;
    size_t queryPos = _host.find('?');
    if (queryPos != std::string::npos) {
        std::string query = _host.substr(queryPos + 1);
        _host = _host.substr(0, queryPos);
        if (query.find("insecure=1") != std::string::npos) {
            insecure = true;
            FN_LOGW(TAG, "TLS: Certificate verification DISABLED (insecure mode)");
        }
    }

    FN_LOGI(TAG, "TLS: Connecting to %s:%u%s", _host.c_str(), _port, 
            insecure ? " (insecure)" : "");

    // Configure TLS
    esp_tls_cfg_t tls_cfg{};
    if (!insecure) {
        tls_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }
    // When insecure=true and crt_bundle_attach is null, cert verification is skipped
    tls_cfg.timeout_ms = CONNECT_TIMEOUT_MS;

    // Create TLS connection
    _tls = esp_tls_init();
    if (!_tls) {
        FN_LOGE(TAG, "TLS: Failed to init esp_tls");
        return fujinet::io::StatusCode::InternalError;
    }

    // Connect synchronously (TODO: consider async for non-blocking behavior)
    int ret = esp_tls_conn_new_sync(_host.c_str(), _host.size(),
                                    _port, &tls_cfg, _tls);

    if (ret != 1) {
        int espErr = 0;
        int tlsFlags = 0;
        esp_tls_error_handle_t errHandle = nullptr;
        esp_tls_get_error_handle(_tls, &errHandle);
        if (errHandle) {
            esp_tls_get_and_clear_last_error(errHandle, &espErr, &tlsFlags);
        }
        
        FN_LOGE(TAG, "TLS: Connection failed to %s:%u, err=%d", 
                _host.c_str(), _port, espErr);
        
        esp_tls_conn_destroy(_tls);
        _tls = nullptr;
        return fujinet::io::StatusCode::IOError;
    }

    FN_LOGI(TAG, "TLS: Connected to %s:%u", _host.c_str(), _port);
    
    reset_state();
    _state = State::Connected;

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TlsNetworkProtocolEspIdf::write_body(std::uint32_t offset,
                                                             const std::uint8_t* data,
                                                             std::size_t len,
                                                             std::uint16_t& written)
{
    written = 0;

    if (_state != State::Connected) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    if (!data || len == 0) {
        return fujinet::io::StatusCode::Ok;
    }

    // Check offset matches expected write cursor
    if (offset != _writeCursor) {
        FN_LOGE(TAG, "TLS: Write offset mismatch: expected %u, got %u", 
                _writeCursor, offset);
        return fujinet::io::StatusCode::InvalidRequest;
    }

    // Write data
    int ret = esp_tls_conn_write(_tls, data, len);
    
    if (ret < 0) {
        int espErr = 0;
        int tlsFlags = 0;
        esp_tls_error_handle_t errHandle = nullptr;
        esp_tls_get_error_handle(_tls, &errHandle);
        if (errHandle) {
            esp_tls_get_and_clear_last_error(errHandle, &espErr, &tlsFlags);
        }
        
        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            // Non-blocking: would block, try again later
            return fujinet::io::StatusCode::NotReady;
        }
        
        handle_error(espErr);
        return fujinet::io::StatusCode::IOError;
    }

    written = static_cast<std::uint16_t>(ret);
    _writeCursor += written;

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TlsNetworkProtocolEspIdf::read_body(std::uint32_t offset,
                                                            std::uint8_t* out,
                                                            std::size_t outLen,
                                                            std::uint16_t& read,
                                                            bool& eof)
{
    read = 0;
    eof = false;

    if (_state == State::Idle) {
        return fujinet::io::StatusCode::InvalidRequest;
    }

    if (_state == State::Error) {
        return fujinet::io::StatusCode::IOError;
    }

    if (!out || outLen == 0) {
        return fujinet::io::StatusCode::Ok;
    }

    // Check offset matches expected read cursor
    if (offset != _readCursor) {
        FN_LOGE(TAG, "TLS: Read offset mismatch: expected %u, got %u", 
                _readCursor, offset);
        return fujinet::io::StatusCode::InvalidRequest;
    }

    // If peer closed and no buffered data, return EOF
    if (_peerClosed && _rxAvailable == 0) {
        eof = true;
        return fujinet::io::StatusCode::Ok;
    }

    // Try to read more data if buffer is empty
    if (_rxAvailable == 0 && _state == State::Connected) {
        int ret = esp_tls_conn_read(_tls, _rxBuffer.data(), _rxBuffer.size());
        
        if (ret < 0) {
            int espErr = 0;
            int tlsFlags = 0;
            esp_tls_error_handle_t errHandle = nullptr;
            esp_tls_get_error_handle(_tls, &errHandle);
            if (errHandle) {
                esp_tls_get_and_clear_last_error(errHandle, &espErr, &tlsFlags);
            }
            
            if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
                // Non-blocking: would block, try again later
                return fujinet::io::StatusCode::NotReady;
            }
            
            handle_error(espErr);
            return fujinet::io::StatusCode::IOError;
        }
        
        if (ret == 0) {
            // Connection closed by peer
            _peerClosed = true;
            _state = State::PeerClosed;
            eof = true;
            return fujinet::io::StatusCode::Ok;
        }

        _rxAvailable = static_cast<std::size_t>(ret);
    }

    // Copy buffered data to output
    if (_rxAvailable > 0) {
        std::size_t toCopy = std::min(_rxAvailable, outLen);
        std::memcpy(out, _rxBuffer.data(), toCopy);
        
        // Move remaining data to front of buffer
        if (toCopy < _rxAvailable) {
            std::memmove(_rxBuffer.data(), _rxBuffer.data() + toCopy, 
                        _rxAvailable - toCopy);
        }
        
        _rxAvailable -= toCopy;
        read = static_cast<std::uint16_t>(toCopy);
        _readCursor += read;
    }

    // Check for EOF condition
    if (_peerClosed && _rxAvailable == 0) {
        eof = true;
    }

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TlsNetworkProtocolEspIdf::info(fujinet::io::NetworkInfo& out)
{
    // TLS connections don't have HTTP-specific info
    out.hasHttpStatus = false;
    out.httpStatus = 0;
    out.hasContentLength = false;
    out.contentLength = 0;
    out.headersBlock.clear();
    
    return fujinet::io::StatusCode::Ok;
}

void TlsNetworkProtocolEspIdf::poll()
{
    // Nothing to poll for now - reads are handled synchronously
    // Future: could implement async connection establishment here
}

void TlsNetworkProtocolEspIdf::close()
{
    if (_tls) {
        esp_tls_conn_destroy(_tls);
        _tls = nullptr;
    }
    
    reset_state();
    _host.clear();
    _port = 0;
}

} // namespace fujinet::platform::esp32
