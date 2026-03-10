#include "doctest.h"

#include "fujinet/fs/path_resolvers/path_resolver.h"

namespace {

using fujinet::fs::PathContext;
using fujinet::fs::PathResolver;
using fujinet::fs::ResolvedTarget;

TEST_CASE("PathResolver resolves TNFS URI schemes")
{
    PathResolver resolver;
    ResolvedTarget out;
    const PathContext ctx{"host", "/"};

    CHECK(resolver.resolve("tnfs://localhost:16384/", ctx, out));
    CHECK(out.fs_name == "tnfs");
    CHECK(out.fs_path == "tnfs://localhost:16384/");

    CHECK(resolver.resolve("tnfs+tcp://127.0.0.1:16384/subdir", ctx, out));
    CHECK(out.fs_name == "tnfs");
    CHECK(out.fs_path == "tnfs+tcp://127.0.0.1:16384/subdir");
}

TEST_CASE("PathResolver resolves TNFS URI schemes case-insensitive")
{
    PathResolver resolver;
    ResolvedTarget out;
    const PathContext ctx{"host", "/"};

    // Uppercase TNFS:// should work
    CHECK(resolver.resolve("TNFS://192.168.1.100:16384/path", ctx, out));
    CHECK(out.fs_name == "tnfs");
    CHECK(out.fs_path == "TNFS://192.168.1.100:16384/path");

    // Mixed case Tnfs:// should work
    CHECK(resolver.resolve("Tnfs://192.168.1.100:16384/path", ctx, out));
    CHECK(out.fs_name == "tnfs");
    CHECK(out.fs_path == "Tnfs://192.168.1.100:16384/path");

    // Uppercase TNFS+TCP:// should work
    CHECK(resolver.resolve("TNFS+TCP://192.168.1.100:16384/path", ctx, out));
    CHECK(out.fs_name == "tnfs");
}

TEST_CASE("PathResolver resolves fs-prefixed paths")
{
    PathResolver resolver;
    ResolvedTarget out;
    const PathContext ctx{"host", "/cwd"};

    CHECK(resolver.resolve("sd0:games/../demo", ctx, out));
    CHECK(out.fs_name == "sd0");
    CHECK(out.fs_path == "/demo");

    CHECK(resolver.resolve("tnfs://localhost:16384/file.txt", ctx, out));
    CHECK(out.fs_name == "tnfs");
    CHECK(out.fs_path == "tnfs://localhost:16384/file.txt");
    CHECK(out.display_path == "tnfs:tnfs://localhost:16384/file.txt");
}

TEST_CASE("PathResolver resolves relative paths from cwd")
{
    PathResolver resolver;
    ResolvedTarget out;

    CHECK(resolver.resolve("subdir/../file.txt", PathContext{"host", "/a/b"}, out));
    CHECK(out.fs_name == "host");
    CHECK(out.fs_path == "/a/b/file.txt");

    CHECK(resolver.resolve("next", PathContext{"tnfs", "//192.168.1.10:16384/root"}, out));
    CHECK(out.fs_name == "tnfs");
    CHECK(out.fs_path == "//192.168.1.10:16384/root/next");
}

TEST_CASE("PathResolver resolveOrCwd falls back to cwd")
{
    PathResolver resolver;
    ResolvedTarget out;

    const std::vector<std::string_view> no_path_argv{"ls"};
    CHECK(resolver.resolveOrCwd(no_path_argv, PathContext{"host", "/x"}, out));
    CHECK(out.fs_name == "host");
    CHECK(out.fs_path == "/x");

    CHECK_FALSE(resolver.resolveOrCwd(no_path_argv, PathContext{"", ""}, out));
}

} // namespace
