#include "doctest.h"

#include "fujinet/fs/http_filesystem.h"
#include "fujinet/io/devices/network_protocol.h"
#include "fujinet/io/core/io_message.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace fujinet::fs;

namespace {

class MockHttpProtocol final : public fujinet::io::INetworkProtocol {
public:
    explicit MockHttpProtocol(std::string scheme)
        : _scheme(std::move(scheme))
    {
    }

    fujinet::io::StatusCode open(const fujinet::io::NetworkOpenRequest& req) override
    {
        _openReq = req;

        if ((_scheme == "http" && req.url == "http://example.com/disk.atr") ||
            (_scheme == "https" && req.url == "https://secure.example.com/demo.xex")) {
            _httpStatus = 200;
            _body = (_scheme == "http")
                ? std::vector<std::uint8_t>{'D', 'I', 'S', 'K'}
                : std::vector<std::uint8_t>{'S', 'E', 'C', 'U', 'R', 'E'};
            _contentLength = _body.size();
            return fujinet::io::StatusCode::Ok;
        }

        _httpStatus = 404;
        _body.clear();
        _contentLength = 0;
        return fujinet::io::StatusCode::Ok;
    }

    fujinet::io::StatusCode write_body(std::uint32_t,
                                       const std::uint8_t*,
                                       std::size_t,
                                       std::uint16_t& written) override
    {
        written = 0;
        return fujinet::io::StatusCode::Unsupported;
    }

    fujinet::io::StatusCode read_body(std::uint32_t offset,
                                      std::uint8_t* out,
                                      std::size_t outLen,
                                      std::uint16_t& read,
                                      bool& eof) override
    {
        read = 0;
        eof = false;

        if (_openReq.method == 5) {
            eof = true;
            return fujinet::io::StatusCode::Ok;
        }

        if (offset > _body.size()) {
            return fujinet::io::StatusCode::InvalidRequest;
        }

        const std::size_t n = std::min(outLen, _body.size() - offset);
        if (n > 0 && out) {
            std::memcpy(out, _body.data() + offset, n);
            read = static_cast<std::uint16_t>(n);
        }
        eof = (offset + n) >= _body.size();
        return fujinet::io::StatusCode::Ok;
    }

    fujinet::io::StatusCode info(fujinet::io::NetworkInfo& out) override
    {
        out = fujinet::io::NetworkInfo{};
        out.hasHttpStatus = true;
        out.httpStatus = _httpStatus;
        out.hasContentLength = true;
        out.contentLength = static_cast<std::uint64_t>(_contentLength);
        return fujinet::io::StatusCode::Ok;
    }

    void poll() override {}

    void close() override
    {
        _openReq = fujinet::io::NetworkOpenRequest{};
        _body.clear();
        _contentLength = 0;
        _httpStatus = 0;
    }

private:
    std::string _scheme;
    fujinet::io::NetworkOpenRequest _openReq{};
    std::vector<std::uint8_t> _body;
    std::size_t _contentLength{0};
    std::uint16_t _httpStatus{0};
};

HttpProtocolFactory make_mock_http_factory()
{
    return [](std::string_view schemeLower) -> std::unique_ptr<fujinet::io::INetworkProtocol> {
        if (schemeLower == "http" || schemeLower == "https") {
            return std::make_unique<MockHttpProtocol>(std::string(schemeLower));
        }
        return nullptr;
    };
}

} // namespace

TEST_CASE("HttpFileSystem: create and basic properties")
{
    auto fs = make_http_filesystem(make_mock_http_factory());

    REQUIRE(fs != nullptr);
    CHECK(fs->name() == "http");
    CHECK(fs->kind() == FileSystemKind::NetworkHttp);
}

TEST_CASE("HttpFileSystem: stat and exists succeed for HTTP and HTTPS URLs")
{
    auto fs = make_http_filesystem(make_mock_http_factory());
    REQUIRE(fs != nullptr);

    FileInfo info{};
    CHECK(fs->stat("http://example.com/disk.atr", info));
    CHECK(info.path == "http://example.com/disk.atr");
    CHECK(info.sizeBytes == 4);
    CHECK_FALSE(info.isDirectory);

    CHECK(fs->exists("https://secure.example.com/demo.xex"));
    CHECK_FALSE(fs->exists("http://example.com/missing.atr"));
}

TEST_CASE("HttpFileSystem: open reads fetched body and is read-only")
{
    auto fs = make_http_filesystem(make_mock_http_factory());
    REQUIRE(fs != nullptr);

    auto file = fs->open("https://secure.example.com/demo.xex", "rb");
    REQUIRE(file != nullptr);

    char buffer[8]{};
    const std::size_t n = file->read(buffer, sizeof(buffer));
    CHECK(n == 6);
    CHECK(std::string(buffer, buffer + n) == "SECURE");
    CHECK(file->tell() == 6);
    CHECK(file->seek(2));
    CHECK(file->tell() == 2);

    CHECK(fs->open("https://secure.example.com/demo.xex", "wb") == nullptr);
}

TEST_CASE("HttpFileSystem: directory and mutation operations are unsupported")
{
    auto fs = make_http_filesystem(make_mock_http_factory());
    REQUIRE(fs != nullptr);

    std::vector<FileInfo> entries;
    CHECK_FALSE(fs->listDirectory("http://example.com/", entries));
    CHECK(entries.empty());
    CHECK_FALSE(fs->createDirectory("http://example.com/newdir"));
    CHECK_FALSE(fs->removeFile("http://example.com/disk.atr"));
    CHECK_FALSE(fs->removeDirectory("http://example.com/newdir"));
    CHECK_FALSE(fs->rename("http://example.com/a", "http://example.com/b"));
    CHECK_FALSE(fs->isDirectory("http://example.com/"));
}
