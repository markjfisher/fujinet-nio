#include "fujinet/fs/http_filesystem.h"

#include "fujinet/core/logging.h"
#include "fujinet/fs/uri_parser.h"
#include "fujinet/io/devices/network_protocol.h"

#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace fujinet::fs {

static constexpr const char* TAG = "http_fs";
static constexpr auto kHttpWaitTimeout = std::chrono::seconds(15);
static constexpr auto kHttpPollInterval = std::chrono::milliseconds(5);

namespace {

std::string to_lower_ascii(std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return out;
}

bool is_http_scheme(std::string_view scheme)
{
    const std::string lower = to_lower_ascii(scheme);
    return lower == "http" || lower == "https";
}

bool is_success_http_status(std::uint16_t status)
{
    return status >= 200U && status < 300U;
}

bool is_read_only_mode(const char* mode)
{
    if (!mode || *mode == '\0') {
        return true;
    }

    if (std::strchr(mode, 'w') != nullptr ||
        std::strchr(mode, 'a') != nullptr) {
        return false;
    }

    return std::strchr(mode, 'r') != nullptr || std::strchr(mode, 'b') != nullptr || std::strchr(mode, '+') != nullptr;
}

struct ParsedHttpUri {
    std::string schemeLower;
    std::string uri;
};

bool parse_http_uri(const std::string& path, ParsedHttpUri& out)
{
    const UriParts parts = parse_uri(path);
    if (!is_http_scheme(parts.scheme) || parts.authority.empty()) {
        return false;
    }

    out.schemeLower = to_lower_ascii(parts.scheme);
    out.uri = path;
    return true;
}

struct HttpTransferResult {
    io::NetworkInfo info{};
    std::vector<std::uint8_t> body;
};

} // namespace

class HttpFile final : public IFile {
public:
    explicit HttpFile(std::vector<std::uint8_t> data)
        : _data(std::move(data))
    {
    }

    std::size_t read(void* dst, std::size_t maxBytes) override
    {
        if (!dst || maxBytes == 0 || _position >= _data.size()) {
            return 0;
        }

        const std::size_t remaining = _data.size() - _position;
        const std::size_t n = std::min(maxBytes, remaining);
        std::memcpy(dst, _data.data() + _position, n);
        _position += n;
        return n;
    }

    std::size_t write(const void* /*src*/, std::size_t /*bytes*/) override
    {
        return 0;
    }

    bool seek(std::uint64_t offset) override
    {
        if (offset > _data.size()) {
            return false;
        }
        _position = static_cast<std::size_t>(offset);
        return true;
    }

    std::uint64_t tell() const override
    {
        return _position;
    }

    bool flush() override
    {
        return true;
    }

private:
    std::vector<std::uint8_t> _data;
    std::size_t _position{0};
};

class HttpFileSystem final : public IFileSystem {
public:
    explicit HttpFileSystem(HttpProtocolFactory protocolFactory)
        : _protocolFactory(std::move(protocolFactory))
    {
        FN_LOGI(TAG, "HTTP filesystem created (dynamic URLs)");
    }

    FileSystemKind kind() const override
    {
        return FileSystemKind::NetworkHttp;
    }

    std::string name() const override
    {
        return "http";
    }

    bool supportsScheme(std::string_view scheme) const
    {
        return is_http_scheme(scheme);
    }

    bool exists(const std::string& path) override
    {
        FileInfo info{};
        return stat(path, info);
    }

    bool isDirectory(const std::string& /*path*/) override
    {
        return false;
    }

    bool createDirectory(const std::string& /*path*/) override
    {
        return false;
    }

    bool removeFile(const std::string& /*path*/) override
    {
        return false;
    }

    bool removeDirectory(const std::string& /*path*/) override
    {
        return false;
    }

    bool rename(const std::string& /*from*/, const std::string& /*to*/) override
    {
        return false;
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override
    {
        if (!is_read_only_mode(mode)) {
            FN_LOGW(TAG, "HTTP filesystem is read-only: mode='%s'", mode ? mode : "");
            return nullptr;
        }

        HttpTransferResult result{};
        if (!perform_get(path, result)) {
            return nullptr;
        }

        return std::make_unique<HttpFile>(std::move(result.body));
    }

    bool stat(const std::string& path, FileInfo& outInfo) override
    {
        HttpTransferResult result{};
        bool ok = perform_head(path, result);

        if (!ok || !result.info.hasContentLength) {
            result = HttpTransferResult{};
            ok = perform_get(path, result);
        }

        if (!ok) {
            return false;
        }

        outInfo = FileInfo{};
        outInfo.path = path;
        outInfo.isDirectory = false;
        outInfo.sizeBytes = result.info.hasContentLength
            ? result.info.contentLength
            : static_cast<std::uint64_t>(result.body.size());
        return true;
    }

    bool listDirectory(const std::string& /*path*/, std::vector<FileInfo>& outEntries) override
    {
        outEntries.clear();
        return false;
    }

private:
    bool perform_head(const std::string& path, HttpTransferResult& out)
    {
        return perform_request(path, 5, false, out); // HEAD
    }

    bool perform_get(const std::string& path, HttpTransferResult& out)
    {
        return perform_request(path, 1, true, out); // GET
    }

    bool perform_request(const std::string& path,
                         std::uint8_t method,
                         bool readBody,
                         HttpTransferResult& out)
    {
        ParsedHttpUri parsed{};
        if (!parse_http_uri(path, parsed)) {
            FN_LOGE(TAG, "Invalid HTTP URL: %s", path.c_str());
            return false;
        }

        if (!supportsScheme(parsed.schemeLower)) {
            FN_LOGE(TAG, "Unsupported HTTP scheme '%s'", parsed.schemeLower.c_str());
            return false;
        }

        auto protocol = _protocolFactory(parsed.schemeLower);
        if (!protocol) {
            FN_LOGE(TAG, "No HTTP protocol registered for scheme '%s'", parsed.schemeLower.c_str());
            return false;
        }

        io::NetworkOpenRequest req{};
        req.method = method;
        req.url = parsed.uri;

        const io::StatusCode openStatus = protocol->open(req);
        if (openStatus != io::StatusCode::Ok) {
            FN_LOGE(TAG, "HTTP open failed for '%s'", parsed.uri.c_str());
            return false;
        }

        bool ok = readBody
            ? read_response_body(*protocol, out.body)
            : wait_for_info(*protocol, out.info);

        if (!ok) {
            protocol->close();
            return false;
        }

        if (!wait_for_info(*protocol, out.info)) {
            protocol->close();
            return false;
        }

        protocol->close();

        if (!out.info.hasHttpStatus || !is_success_http_status(out.info.httpStatus)) {
            FN_LOGW(TAG,
                    "HTTP request rejected: url='%s' status=%u",
                    parsed.uri.c_str(),
                    static_cast<unsigned>(out.info.httpStatus));
            return false;
        }

        if (readBody && !out.info.hasContentLength) {
            out.info.hasContentLength = true;
            out.info.contentLength = static_cast<std::uint64_t>(out.body.size());
        }

        return true;
    }

    static bool wait_for_info(io::INetworkProtocol& protocol, io::NetworkInfo& outInfo)
    {
        const auto deadline = std::chrono::steady_clock::now() + kHttpWaitTimeout;
        while (true) {
            protocol.poll();

            io::NetworkInfo info{};
            const io::StatusCode status = protocol.info(info);
            if (status == io::StatusCode::Ok) {
                outInfo = std::move(info);
                return true;
            }
            if (status != io::StatusCode::NotReady) {
                return false;
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                FN_LOGE(TAG, "Timed out waiting for HTTP metadata");
                return false;
            }

            std::this_thread::sleep_for(kHttpPollInterval);
        }
    }

    static bool read_response_body(io::INetworkProtocol& protocol, std::vector<std::uint8_t>& outBody)
    {
        outBody.clear();

        std::array<std::uint8_t, 1024> buffer{};
        std::uint32_t offset = 0;
        const auto deadline = std::chrono::steady_clock::now() + kHttpWaitTimeout;

        while (true) {
            protocol.poll();

            std::uint16_t read = 0;
            bool eof = false;
            const io::StatusCode status = protocol.read_body(offset,
                                                             buffer.data(),
                                                             buffer.size(),
                                                             read,
                                                             eof);
            if (status == io::StatusCode::Ok) {
                if (read > 0) {
                    outBody.insert(outBody.end(), buffer.begin(), buffer.begin() + read);
                    if (offset > std::numeric_limits<std::uint32_t>::max() - read) {
                        FN_LOGE(TAG, "HTTP body exceeds supported size");
                        return false;
                    }
                    offset += read;
                }

                if (eof) {
                    return true;
                }
                continue;
            }

            if (status != io::StatusCode::NotReady) {
                return false;
            }

            if (std::chrono::steady_clock::now() >= deadline) {
                FN_LOGE(TAG, "Timed out reading HTTP body");
                return false;
            }

            std::this_thread::sleep_for(kHttpPollInterval);
        }
    }

    HttpProtocolFactory _protocolFactory;
};

std::unique_ptr<IFileSystem> make_http_filesystem(HttpProtocolFactory protocolFactory)
{
    if (!protocolFactory) {
        FN_LOGE(TAG, "make_http_filesystem() requires a protocol factory");
        return nullptr;
    }

    return std::make_unique<HttpFileSystem>(std::move(protocolFactory));
}

std::unique_ptr<IFileSystem> make_http_filesystem()
{
    FN_LOGE(TAG, "make_http_filesystem() without factory is not supported");
    return nullptr;
}

} // namespace fujinet::fs
