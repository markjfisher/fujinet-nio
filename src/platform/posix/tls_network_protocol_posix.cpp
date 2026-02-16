#include "fujinet/platform/posix/tls_network_protocol_posix.h"

#include <algorithm>
#include <cstring>
#include <cerrno>

#include "fujinet/core/logging.h"

// OpenSSL headers
#include <openssl/ssl.h>
#include <openssl/err.h>

// POSIX headers
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

namespace fujinet::platform::posix {

static constexpr const char* TAG = "platform";
static constexpr std::size_t RX_BUFFER_SIZE = 8192;
static constexpr int CONNECT_TIMEOUT_SEC = 10;

TlsNetworkProtocolPosix::TlsNetworkProtocolPosix()
    : _rxBuffer(RX_BUFFER_SIZE)
{
    ensure_ssl_init();
}

TlsNetworkProtocolPosix::~TlsNetworkProtocolPosix()
{
    close();
}

void TlsNetworkProtocolPosix::ensure_ssl_init()
{
    static const bool inited = []{
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        return true;
    }();
    (void)inited;
}

void TlsNetworkProtocolPosix::reset_state()
{
    _state = State::Idle;
    _rxAvailable = 0;
    _readCursor = 0;
    _writeCursor = 0;
    _lastError = 0;
    _peerClosed = false;
}

void TlsNetworkProtocolPosix::handle_error(const char* context, int sslError)
{
    _lastError = sslError;
    _state = State::Error;
    
    char errBuf[256];
    ERR_error_string_n(sslError, errBuf, sizeof(errBuf));
    FN_LOGE(TAG, "TLS error (%s): %s", context, errBuf);
}

bool TlsNetworkProtocolPosix::parse_tls_url(const std::string& url,
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

fujinet::io::StatusCode TlsNetworkProtocolPosix::open(const fujinet::io::NetworkOpenRequest& req)
{
    close();

    if (!parse_tls_url(req.url, _host, _port)) {
        FN_LOGE(TAG, "TLS: Invalid URL format: %s", req.url.c_str());
        return fujinet::io::StatusCode::InvalidRequest;
    }

    FN_LOGI(TAG, "TLS: Connecting to %s:%u", _host.c_str(), _port);

    // Resolve host
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    int gaiRet = getaddrinfo(_host.c_str(), std::to_string(_port).c_str(), &hints, &result);
    if (gaiRet != 0) {
        FN_LOGE(TAG, "TLS: DNS resolution failed for %s: %s", _host.c_str(), gai_strerror(gaiRet));
        return fujinet::io::StatusCode::IOError;
    }

    // Create socket
    _socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (_socket < 0) {
        FN_LOGE(TAG, "TLS: Failed to create socket: %s", strerror(errno));
        freeaddrinfo(result);
        return fujinet::io::StatusCode::InternalError;
    }

    // Connect
    int connRet = connect(_socket, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (connRet < 0) {
        FN_LOGE(TAG, "TLS: Connection failed to %s:%u: %s", _host.c_str(), _port, strerror(errno));
        ::close(_socket);
        _socket = -1;
        return fujinet::io::StatusCode::IOError;
    }

    // Create SSL context
    const SSL_METHOD* method = TLS_client_method();
    _ctx = SSL_CTX_new(method);
    if (!_ctx) {
        FN_LOGE(TAG, "TLS: Failed to create SSL context");
        ::close(_socket);
        _socket = -1;
        return fujinet::io::StatusCode::InternalError;
    }

    // Configure certificate verification
    SSL_CTX_set_verify(_ctx, SSL_VERIFY_PEER, nullptr);
    SSL_CTX_set_default_verify_paths(_ctx);  // Use system CA certificates

    // Create SSL connection
    _ssl = SSL_new(_ctx);
    if (!_ssl) {
        FN_LOGE(TAG, "TLS: Failed to create SSL structure");
        SSL_CTX_free(_ctx);
        _ctx = nullptr;
        ::close(_socket);
        _socket = -1;
        return fujinet::io::StatusCode::InternalError;
    }

    // Set hostname for SNI
    SSL_set_tlsext_host_name(_ssl, _host.c_str());

    // Attach socket to SSL
    SSL_set_fd(_ssl, _socket);

    // Perform TLS handshake
    int sslRet = SSL_connect(_ssl);
    if (sslRet != 1) {
        int sslError = SSL_get_error(_ssl, sslRet);
        handle_error("connect", sslError);
        SSL_free(_ssl);
        _ssl = nullptr;
        SSL_CTX_free(_ctx);
        _ctx = nullptr;
        ::close(_socket);
        _socket = -1;
        return fujinet::io::StatusCode::IOError;
    }

    FN_LOGI(TAG, "TLS: Connected to %s:%u (cipher: %s)", 
            _host.c_str(), _port, SSL_get_cipher_name(_ssl));

    reset_state();
    _state = State::Connected;

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TlsNetworkProtocolPosix::write_body(std::uint32_t offset,
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
    int ret = SSL_write(_ssl, data, static_cast<int>(len));
    
    if (ret <= 0) {
        int sslError = SSL_get_error(_ssl, ret);
        
        if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
            // Non-blocking: would block, try again later
            return fujinet::io::StatusCode::NotReady;
        }
        
        handle_error("write", sslError);
        return fujinet::io::StatusCode::IOError;
    }

    written = static_cast<std::uint16_t>(ret);
    _writeCursor += written;

    return fujinet::io::StatusCode::Ok;
}

fujinet::io::StatusCode TlsNetworkProtocolPosix::read_body(std::uint32_t offset,
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
        int ret = SSL_read(_ssl, _rxBuffer.data(), static_cast<int>(_rxBuffer.size()));
        
        if (ret <= 0) {
            int sslError = SSL_get_error(_ssl, ret);
            
            if (sslError == SSL_ERROR_WANT_READ || sslError == SSL_ERROR_WANT_WRITE) {
                // Non-blocking: would block, try again later
                return fujinet::io::StatusCode::NotReady;
            }
            
            if (sslError == SSL_ERROR_ZERO_RETURN) {
                // Connection closed by peer (clean shutdown)
                _peerClosed = true;
                _state = State::PeerClosed;
                eof = true;
                return fujinet::io::StatusCode::Ok;
            }
            
            handle_error("read", sslError);
            return fujinet::io::StatusCode::IOError;
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

fujinet::io::StatusCode TlsNetworkProtocolPosix::info(fujinet::io::NetworkInfo& out)
{
    // TLS connections don't have HTTP-specific info
    out.hasHttpStatus = false;
    out.httpStatus = 0;
    out.hasContentLength = false;
    out.contentLength = 0;
    out.headersBlock.clear();
    
    return fujinet::io::StatusCode::Ok;
}

void TlsNetworkProtocolPosix::poll()
{
    // Nothing to poll for now - reads are handled synchronously
    // Future: could implement async connection establishment here
}

void TlsNetworkProtocolPosix::close()
{
    if (_ssl) {
        SSL_shutdown(_ssl);
        SSL_free(_ssl);
        _ssl = nullptr;
    }
    
    if (_ctx) {
        SSL_CTX_free(_ctx);
        _ctx = nullptr;
    }
    
    if (_socket >= 0) {
        ::close(_socket);
        _socket = -1;
    }
    
    reset_state();
    _host.clear();
    _port = 0;
}

} // namespace fujinet::platform::posix
