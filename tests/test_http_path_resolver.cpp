#include "doctest.h"

#include "fujinet/fs/path_resolvers/path_resolver.h"

namespace {

using fujinet::fs::PathContext;
using fujinet::fs::PathResolver;
using fujinet::fs::ResolvedTarget;

TEST_CASE("PathResolver resolves HTTP and HTTPS URI schemes")
{
    PathResolver resolver;
    ResolvedTarget out;
    const PathContext ctx{"host", "/"};

    CHECK(resolver.resolve("http://example.com/files/disk.atr", ctx, out));
    CHECK(out.fs_name == "http");
    CHECK(out.fs_path == "http://example.com/files/disk.atr");

    CHECK(resolver.resolve("HTTPS://secure.example.com/demo.xex", ctx, out));
    CHECK(out.fs_name == "http");
    CHECK(out.fs_path == "HTTPS://secure.example.com/demo.xex");
}

TEST_CASE("PathResolver resolves relative paths from HTTP cwd")
{
    PathResolver resolver;
    ResolvedTarget out;

    CHECK(resolver.resolve("next/file.atr",
                           PathContext{"http", "http://example.com/root/base"},
                           out));
    CHECK(out.fs_name == "http");
    CHECK(out.fs_path == "http://example.com/root/base/next/file.atr");

    CHECK(resolver.resolve("/absolute/demo.xex",
                           PathContext{"http", "https://secure.example.com/root/base"},
                           out));
    CHECK(out.fs_path == "https://secure.example.com/absolute/demo.xex");
}

} // namespace
