
#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/tnfs/tnfs_protocol.h"
#include "fujinet/fs/uri_parser.h"
#include "fujinet/core/logging.h"

#include <chrono>
#include <cstring>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <utility>

namespace fujinet::fs {

static constexpr const char* TAG = "tnfs_fs";

class TnfsFile final : public IFile {
public:
    TnfsFile(std::shared_ptr<tnfs::ITnfsClient> client, int fileHandle)
        : _client(std::move(client))
        , _fileHandle(fileHandle)
        , _position(0)
    {
        FN_LOGD(TAG, "File handle %d created", fileHandle);
    }

    ~TnfsFile() override {
        if (_fileHandle != -1) {
            _client->close(_fileHandle);
            FN_LOGD(TAG, "File handle %d closed", _fileHandle);
        }
    }

    std::size_t read(void* dst, std::size_t maxBytes) override {
        std::size_t bytesRead = _client->read(_fileHandle, dst, maxBytes);
        _position += bytesRead;
        return bytesRead;
    }

    std::size_t write(const void* src, std::size_t bytes) override {
        std::size_t bytesWritten = _client->write(_fileHandle, src, bytes);
        _position += bytesWritten;
        return bytesWritten;
    }

    bool seek(std::uint64_t offset) override {
        bool success = _client->seek(_fileHandle, static_cast<uint32_t>(offset));
        if (success) {
            _position = offset;
        }
        return success;
    }

    std::uint64_t tell() const override {
        return _position;
    }

    bool flush() override {
        // TNFS doesn't have a flush command, so this is a no-op
        return true;
    }

private:
    std::shared_ptr<tnfs::ITnfsClient> _client;
    int _fileHandle;
    std::uint64_t _position;
};

class TnfsFileSystem final : public IFileSystem {
public:
    explicit TnfsFileSystem(TnfsClientFactory clientFactory)
        : _clientFactory(std::move(clientFactory))
    {
        FN_LOGI(TAG, "TNFS filesystem created (dynamic endpoints)");
    }

    explicit TnfsFileSystem(std::shared_ptr<tnfs::ITnfsClient> fixedClient)
        : _fixedClient(std::move(fixedClient))
    {
        FN_LOGI(TAG, "TNFS filesystem created (single client)");
    }

    ~TnfsFileSystem() override = default;

    FileSystemKind kind() const override {
        return FileSystemKind::NetworkTnfs;
    }

    std::string name() const override {
        return "tnfs";
    }

    bool exists(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->exists(resolved.path);
    }

    bool isDirectory(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->isDirectory(resolved.path);
    }

    bool createDirectory(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->createDirectory(resolved.path);
    }

    bool removeFile(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->removeFile(resolved.path);
    }

    bool removeDirectory(const std::string& path) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }
        return resolved.client->removeDirectory(resolved.path);
    }

    bool rename(const std::string& from, const std::string& to) override {
        ResolvedPath src{};
        ResolvedPath dst{};
        if (!resolve_path(from, src) || !resolve_path(to, dst)) {
            return false;
        }

        if (src.endpoint.host != dst.endpoint.host ||
            src.endpoint.port != dst.endpoint.port ||
            src.endpoint.mountPath != dst.endpoint.mountPath ||
            src.endpoint.user != dst.endpoint.user ||
            src.endpoint.password != dst.endpoint.password) {
            FN_LOGE(TAG, "Rename across TNFS endpoints is not supported");
            return false;
        }

        return src.client->rename(src.path, dst.path);
    }

    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return nullptr;
        }
        uint16_t openMode = 0;
        // Default perms for newly created files (rw-r--r--).
        uint16_t createPerms = 0644;

        if (std::strchr(mode, 'r') != nullptr) {
            openMode |= tnfs::OPENMODE_READ;
        }
        if (std::strchr(mode, 'w') != nullptr) {
            openMode |= tnfs::OPENMODE_WRITE | tnfs::OPENMODE_WRITE_CREATE | tnfs::OPENMODE_WRITE_TRUNCATE;
        }
        if (std::strchr(mode, 'a') != nullptr) {
            openMode |= tnfs::OPENMODE_WRITE | tnfs::OPENMODE_WRITE_CREATE | tnfs::OPENMODE_WRITE_APPEND;
        }
        if (std::strchr(mode, '+') != nullptr) {
            openMode |= tnfs::OPENMODE_READWRITE;
        }
        if (openMode == 0) {
            // Default to read mode for unrecognized mode strings.
            openMode = tnfs::OPENMODE_READ;
        }

        int fileHandle = resolved.client->open(resolved.path, openMode, createPerms);
        if (fileHandle == -1) {
            FN_LOGE(TAG, "Failed to open file: %s", resolved.path.c_str());
            return nullptr;
        }

        return std::make_unique<TnfsFile>(resolved.client, fileHandle);
    }

    bool stat(const std::string& path, FileInfo& outInfo) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }

        tnfs::TnfsStat st{};
        if (!resolved.client->stat(resolved.path, st)) {
            return false;
        }

        outInfo.path = path;
        outInfo.isDirectory = st.isDir;
        outInfo.sizeBytes = st.filesize;
        outInfo.modifiedTime = std::chrono::system_clock::from_time_t(st.mTime);

        return true;
    }

    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override {
        ResolvedPath resolved{};
        if (!resolve_path(path, resolved)) {
            return false;
        }

        if (!resolved.client->isDirectory(resolved.path)) {
            return false;
        }

        outEntries.clear();
        std::vector<std::string> entries = resolved.client->listDirectory(resolved.path);
        for (const auto& entryName : entries) {
            std::string entryPath = join_path(resolved.path, entryName);
            tnfs::TnfsStat st{};
            if (!resolved.client->stat(entryPath, st)) {
                continue;
            }

            FileInfo info{};
            info.path = entryPath;
            info.isDirectory = st.isDir;
            info.sizeBytes = st.filesize;
            info.modifiedTime = std::chrono::system_clock::from_time_t(st.mTime);
            outEntries.push_back(std::move(info));
        }

        return true;
    }

private:
    struct SessionKey {
        std::string host;
        std::uint16_t port{tnfs::DEFAULT_PORT};
        std::string mountPath;
        std::string user;
        std::string password;
        bool useTcp{false};

        bool operator<(const SessionKey& other) const
        {
            return std::tie(host, port, mountPath, user, password, useTcp) <
                   std::tie(other.host, other.port, other.mountPath, other.user, other.password, other.useTcp);
        }
    };

    struct Session {
        TnfsEndpoint endpoint;
        std::shared_ptr<tnfs::ITnfsClient> client;
    };

    struct ResolvedPath {
        TnfsEndpoint endpoint;
        std::string path;
        std::shared_ptr<tnfs::ITnfsClient> client;
    };

    static std::string ensure_abs_path(const std::string& path)
    {
        if (path.empty()) {
            return "/";
        }
        if (path.front() == '/') {
            return path;
        }
        return "/" + path;
    }

    static std::string join_path(const std::string& base, const std::string& name)
    {
        if (base.empty() || base == "/") {
            return "/" + name;
        }
        if (base.back() == '/') {
            return base + name;
        }
        return base + "/" + name;
    }

    static bool parse_host_port(const std::string& authority, std::string& outHost, std::uint16_t& outPort)
    {
        if (authority.empty()) {
            return false;
        }

        outHost = authority;
        outPort = tnfs::DEFAULT_PORT;

        // Handle [ipv6]:port syntax.
        if (authority.front() == '[') {
            std::size_t bracket = authority.find(']');
            if (bracket == std::string::npos) {
                return false;
            }
            outHost = authority.substr(1, bracket - 1);
            if (bracket + 1 < authority.size()) {
                if (authority[bracket + 1] != ':') {
                    return false;
                }
                std::string portStr = authority.substr(bracket + 2);
                if (portStr.empty()) {
                    return false;
                }
                unsigned long parsed = 0;
                try {
                    parsed = std::stoul(portStr);
                } catch (...) {
                    return false;
                }
                if (parsed == 0 || parsed > 65535UL) {
                    return false;
                }
                outPort = static_cast<std::uint16_t>(parsed);
            }
            return true;
        }

        std::size_t colon = authority.rfind(':');
        if (colon == std::string::npos) {
            return true;
        }

        // If there are multiple colons it's likely bare IPv6 without brackets, keep as host.
        if (authority.find(':') != colon) {
            return true;
        }

        std::string host = authority.substr(0, colon);
        std::string portStr = authority.substr(colon + 1);
        if (host.empty() || portStr.empty()) {
            return false;
        }

        unsigned long parsed = 0;
        try {
            parsed = std::stoul(portStr);
        } catch (...) {
            return false;
        }
        if (parsed == 0 || parsed > 65535UL) {
            return false;
        }

        outHost = host;
        outPort = static_cast<std::uint16_t>(parsed);
        return true;
    }

    bool parse_endpoint_and_path(const std::string& rawPath, TnfsEndpoint& endpoint, std::string& outPath) const
    {
        endpoint = TnfsEndpoint{};

        // Preferred format: tnfs://host[:port]/path
        if (rawPath.rfind("tnfs://", 0) == 0) {
            UriParts parts = parse_uri(rawPath);
            if (parts.scheme != "tnfs") {
                return false;
            }

            if (!parse_host_port(parts.authority, endpoint.host, endpoint.port)) {
                FN_LOGE(TAG, "Invalid TNFS authority in URI: %s", rawPath.c_str());
                return false;
            }

            outPath = ensure_abs_path(parts.path);
            return true;
        }

        // TCP TNFS form: tnfs+tcp://host[:port]/path (aliases: tnfstcp://, tnfs-tcp://)
        if (rawPath.rfind("tnfs+tcp://", 0) == 0 ||
            rawPath.rfind("tnfstcp://", 0) == 0 ||
            rawPath.rfind("tnfs-tcp://", 0) == 0) {
            UriParts parts = parse_uri(rawPath);
            if (parts.scheme != "tnfs+tcp" &&
                parts.scheme != "tnfstcp" &&
                parts.scheme != "tnfs-tcp") {
                return false;
            }

            if (!parse_host_port(parts.authority, endpoint.host, endpoint.port)) {
                FN_LOGE(TAG, "Invalid TNFS authority in URI: %s", rawPath.c_str());
                return false;
            }

            endpoint.useTcp = true;
            outPath = ensure_abs_path(parts.path);
            return true;
        }

        // Backward-compatible form: //host[:port]/path
        if (rawPath.rfind("//", 0) == 0) {
            std::size_t slash = rawPath.find('/', 2);
            std::string authority = (slash == std::string::npos)
                ? rawPath.substr(2)
                : rawPath.substr(2, slash - 2);

            if (!parse_host_port(authority, endpoint.host, endpoint.port)) {
                FN_LOGE(TAG, "Invalid TNFS authority in path: %s", rawPath.c_str());
                return false;
            }

            outPath = (slash == std::string::npos) ? "/" : rawPath.substr(slash);
            outPath = ensure_abs_path(outPath);
            return true;
        }

        // No endpoint info in path; use default route if configured.
        if (_fixedClient) {
            endpoint.host = "__fixed__";
            endpoint.port = tnfs::DEFAULT_PORT;
            endpoint.mountPath = "/";
            outPath = ensure_abs_path(rawPath);
            return true;
        }

        if (_defaultEndpoint.host.empty()) {
            FN_LOGE(TAG, "TNFS path is missing endpoint: %s", rawPath.c_str());
            return false;
        }

        endpoint = _defaultEndpoint;
        outPath = ensure_abs_path(rawPath);
        return true;
    }

    std::shared_ptr<tnfs::ITnfsClient> get_or_create_session(const TnfsEndpoint& endpoint)
    {
        SessionKey key{endpoint.host, endpoint.port, endpoint.mountPath, endpoint.user, endpoint.password, endpoint.useTcp};
        auto existing = _sessions.find(key);
        if (existing != _sessions.end()) {
            return existing->second.client;
        }

        if (_fixedClient) {
            if (_sessions.empty()) {
                if (!_fixedClient->mount(endpoint.mountPath, endpoint.user, endpoint.password)) {
                    FN_LOGE(TAG, "Failed to mount fixed TNFS client");
                    return {};
                }
                Session session{};
                session.endpoint = endpoint;
                session.client = _fixedClient;
                _sessions.emplace(std::move(key), std::move(session));
                return _fixedClient;
            }
            FN_LOGE(TAG, "Fixed TNFS client cannot switch endpoints");
            return {};
        }

        if (!_clientFactory) {
            FN_LOGE(TAG, "No TNFS client factory configured");
            return {};
        }

        std::unique_ptr<tnfs::ITnfsClient> created = _clientFactory(endpoint);
        if (!created) {
            FN_LOGE(TAG, "Failed to create TNFS client for %s:%u",
                endpoint.host.c_str(),
                static_cast<unsigned>(endpoint.port));
            return {};
        }

        std::shared_ptr<tnfs::ITnfsClient> client(std::move(created));
        if (!client->mount(endpoint.mountPath, endpoint.user, endpoint.password)) {
            FN_LOGE(TAG, "Failed to mount TNFS session for %s:%u",
                endpoint.host.c_str(),
                static_cast<unsigned>(endpoint.port));
            return {};
        }

        Session session{};
        session.endpoint = endpoint;
        session.client = client;
        _sessions.emplace(std::move(key), std::move(session));
        FN_LOGI(TAG, "Mounted TNFS session %s:%u",
            endpoint.host.c_str(),
            static_cast<unsigned>(endpoint.port));
        return client;
    }

    bool resolve_path(const std::string& rawPath, ResolvedPath& out)
    {
        if (!parse_endpoint_and_path(rawPath, out.endpoint, out.path)) {
            return false;
        }
        out.client = get_or_create_session(out.endpoint);
        return static_cast<bool>(out.client);
    }

    TnfsClientFactory _clientFactory;
    std::shared_ptr<tnfs::ITnfsClient> _fixedClient;
    TnfsEndpoint _defaultEndpoint;
    std::map<SessionKey, Session> _sessions;
};

std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::shared_ptr<tnfs::ITnfsClient> client) {
    if (!client) {
        return nullptr;
    }
    return std::make_unique<TnfsFileSystem>(std::move(client));
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem(std::unique_ptr<tnfs::ITnfsClient> client) {
    return make_tnfs_filesystem(std::shared_ptr<tnfs::ITnfsClient>(std::move(client)));
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem(TnfsClientFactory clientFactory)
{
    if (!clientFactory) {
        return nullptr;
    }
    return std::make_unique<TnfsFileSystem>(std::move(clientFactory));
}

std::unique_ptr<IFileSystem> make_tnfs_filesystem() {
    FN_LOGE(TAG, "make_tnfs_filesystem() without factory is not supported");
    return nullptr;
}

} // namespace fujinet::fs
