#include "doctest.h"

#include "fujinet/console/console_commands.h"
#include "fujinet/console/console_engine.h"
#include "fujinet/console/console_parse.h"
#include "fujinet/console/fs_shell.h"
#include "fujinet/diag/diagnostic_registry.h"
#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/fs/storage_manager.h"

#include "fake_fs.h"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::tests {
namespace {

class FakeConsoleTransport final : public fujinet::console::IConsoleTransport {
public:
    bool connected{true};
    std::deque<std::uint8_t> in;
    std::string out;

    bool is_connected() const override { return connected; }

    bool read_byte(std::uint8_t& outb, int /*timeout_ms*/) override
    {
        if (in.empty()) return false;
        outb = in.front();
        in.pop_front();
        return true;
    }

    void write(std::string_view s) override { out.append(s.data(), s.size()); }

    void write_line(std::string_view s) override
    {
        out.append(s.data(), s.size());
        out.push_back('\n');
    }

    void push_line(std::string_view s)
    {
        for (char c : s) in.push_back(static_cast<std::uint8_t>(c));
        in.push_back(static_cast<std::uint8_t>('\r'));
    }
};

class DummyDiagProvider final : public fujinet::diag::IDiagnosticProvider {
public:
    explicit DummyDiagProvider(std::string_view id, std::vector<fujinet::diag::DiagCommandSpec> cmds)
        : _id(id), _cmds(std::move(cmds))
    {}

    std::string_view provider_id() const noexcept override { return _id; }

    void list_commands(std::vector<fujinet::diag::DiagCommandSpec>& out) const override
    {
        out.insert(out.end(), _cmds.begin(), _cmds.end());
    }

    fujinet::diag::DiagResult execute(const fujinet::diag::DiagArgsView& args) override
    {
        (void)args;
        return fujinet::diag::DiagResult::not_found("not found");
    }

private:
    std::string _id;
    std::vector<fujinet::diag::DiagCommandSpec> _cmds;
};

static bool contains(std::string_view hay, std::string_view needle)
{
    return hay.find(needle) != std::string_view::npos;
}

static std::vector<std::string_view> sv_argv(const std::vector<std::string>& v)
{
    std::vector<std::string_view> out;
    out.reserve(v.size());
    for (const auto& s : v) out.push_back(s);
    return out;
}

} // namespace

TEST_CASE("console_parse split_ws")
{
    using fujinet::console::split_ws;
    CHECK(split_ws("") == std::vector<std::string_view>{});
    CHECK(split_ws("   ") == std::vector<std::string_view>{});

    auto v = split_ws("  a  b\tc ");
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "a");
    CHECK(v[1] == "b");
    CHECK(v[2] == "c");
}

TEST_CASE("ConsoleCommandRegistry register/dispatch")
{
    using fujinet::console::ConsoleCommandRegistry;
    using fujinet::console::ConsoleCommandSpec;

    ConsoleCommandRegistry reg;
    int count = 0;

    CHECK(reg.register_command(ConsoleCommandSpec{"a", "A", "a"}, [&](const auto&) {
        ++count;
        return true;
    }));
    CHECK(!reg.register_command(ConsoleCommandSpec{"a", "A2", "a"}, [&](const auto&) {
        return true;
    }));

    std::vector<std::string> argv_s = {"a"};
    auto r = reg.dispatch(sv_argv(argv_s));
    REQUIRE(r.has_value());
    CHECK(*r == true);
    CHECK(count == 1);
}

TEST_CASE("ConsoleEngine help prints diagnostic sections")
{
    fujinet::diag::DiagnosticRegistry dreg;

    DummyDiagProvider corep(
        "core",
        {
            fujinet::diag::DiagCommandSpec{.name = "core.stats", .summary = "stats", .usage = "core.stats"},
        });
    DummyDiagProvider diskp(
        "disk",
        {
            fujinet::diag::DiagCommandSpec{.name = "disk.slots", .summary = "slots", .usage = "disk.slots"},
        });
    dreg.add_provider(corep);
    dreg.add_provider(diskp);

    FakeConsoleTransport io;
    fujinet::console::ConsoleEngine eng(dreg, io);

    // First step should emit MOTD + prompt on initial connect.
    CHECK(eng.step(0));
    CHECK(contains(io.out, "fujinet-nio diagnostic console"));
    CHECK(contains(io.out, "> "));

    // Then run help.
    io.push_line("help");
    CHECK(eng.step(0));
    CHECK(contains(io.out, "commands:"));
    CHECK(contains(io.out, "diagnostics:"));
    CHECK(contains(io.out, "[core]"));
    CHECK(contains(io.out, "core.stats"));
    CHECK(contains(io.out, "[disk]"));
    CHECK(contains(io.out, "disk.slots"));
}

TEST_CASE("FsShell rm/rmdir semantics + wildcards + mv to directory")
{
    fujinet::fs::StorageManager storage;
    auto fs_up = std::make_unique<fujinet::tests::MemoryFileSystem>("mem");
    auto* fs = fs_up.get();
    REQUIRE(storage.registerFileSystem(std::move(fs_up)));

    REQUIRE(fs->createDirectory("/a"));
    REQUIRE(fs->createDirectory("/b"));
    REQUIRE(fs->create_file("/a/f.txt", {1, 2, 3}));

    FakeConsoleTransport io;
    fujinet::console::ConsoleCommandRegistry reg;
    fujinet::console::FsShell sh(storage);
    sh.cwd_fs() = "mem";
    sh.cwd_path() = "/a";
    REQUIRE(sh.register_commands(reg, io));

    SUBCASE("rmdir on a file fails and does not delete the file")
    {
        std::vector<std::string> argv_s = {"rmdir", "f.txt"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(fs->exists("/a/f.txt"));
        CHECK(contains(io.out, "error: not a directory"));
    }

    SUBCASE("rm wildcard deletes matching files")
    {
        std::vector<std::string> argv_s = {"rm", "*.txt"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(!fs->exists("/a/f.txt"));
    }

    SUBCASE("rm without -r refuses to delete a directory")
    {
        std::vector<std::string> argv_s = {"rm", "/a"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(fs->exists("/a"));
        CHECK(contains(io.out, "error: is a directory"));
    }

    SUBCASE("rmdir -rf deletes a directory tree")
    {
        REQUIRE(fs->createDirectory("/a/sub"));
        REQUIRE(fs->create_file("/a/sub/x.bin", {9}));

        std::vector<std::string> argv_s = {"rmdir", "-rf", "/a"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(!fs->exists("/a"));
        CHECK(!fs->exists("/a/sub/x.bin"));
    }

    SUBCASE("mv destination directory appends basename")
    {
        REQUIRE(fs->create_file("/a/x.bin", {7, 7}));

        std::vector<std::string> argv_s = {"mv", "x.bin", "/b/"};
        auto r = reg.dispatch(sv_argv(argv_s));
        REQUIRE(r.has_value());
        CHECK(!fs->exists("/a/x.bin"));
        CHECK(fs->exists("/b/x.bin"));
    }
}

} // namespace fujinet::tests


