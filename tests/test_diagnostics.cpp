#include "doctest.h"

#include "fujinet/diag/diagnostic_provider.h"
#include "fujinet/diag/diagnostic_registry.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::tests {
namespace {

class RecordingProvider final : public fujinet::diag::IDiagnosticProvider {
public:
    RecordingProvider(std::string_view id, std::vector<fujinet::diag::DiagCommandSpec> cmds)
        : _id(id), _cmds(std::move(cmds))
    {}

    std::string_view provider_id() const noexcept override { return _id; }

    void list_commands(std::vector<fujinet::diag::DiagCommandSpec>& out) const override
    {
        out.insert(out.end(), _cmds.begin(), _cmds.end());
    }

    fujinet::diag::DiagResult execute(const fujinet::diag::DiagArgsView& args) override
    {
        ++execute_calls;
        last_line = std::string(args.line);
        last_argv.clear();
        for (auto a : args.argv) last_argv.push_back(std::string(a));

        if (_handle) {
            return _handle(args);
        }
        return fujinet::diag::DiagResult::not_found("not found");
    }

    void set_handler(std::function<fujinet::diag::DiagResult(const fujinet::diag::DiagArgsView&)> h)
    {
        _handle = std::move(h);
    }

    int execute_calls{0};
    std::string last_line;
    std::vector<std::string> last_argv;

private:
    std::string _id;
    std::vector<fujinet::diag::DiagCommandSpec> _cmds;
    std::function<fujinet::diag::DiagResult(const fujinet::diag::DiagArgsView&)> _handle;
};

static fujinet::diag::DiagArgsView make_args(std::string_view line, std::vector<std::string_view> argv)
{
    fujinet::diag::DiagArgsView a;
    a.line = line;
    a.argv = std::move(argv);
    return a;
}

static bool has_cmd(const std::vector<fujinet::diag::DiagCommandSpec>& cmds, std::string_view name)
{
    for (const auto& c : cmds) {
        if (c.name == name) return true;
    }
    return false;
}

} // namespace

TEST_CASE("DiagnosticRegistry list_all_commands aggregates providers")
{
    fujinet::diag::DiagnosticRegistry reg;

    RecordingProvider p1(
        "core",
        {
            fujinet::diag::DiagCommandSpec{.name = "core.stats", .summary = "stats", .usage = "core.stats"},
        });
    RecordingProvider p2(
        "disk",
        {
            fujinet::diag::DiagCommandSpec{.name = "disk.slots", .summary = "slots", .usage = "disk.slots"},
        });
    reg.add_provider(p1);
    reg.add_provider(p2);

    std::vector<fujinet::diag::DiagCommandSpec> cmds;
    reg.list_all_commands(cmds);
    CHECK(has_cmd(cmds, "core.stats"));
    CHECK(has_cmd(cmds, "disk.slots"));
}

TEST_CASE("DiagnosticRegistry dispatch uses first non-NotFound provider and preserves result")
{
    fujinet::diag::DiagnosticRegistry reg;

    RecordingProvider p1("p1", {});
    RecordingProvider p2("p2", {});
    reg.add_provider(p1);
    reg.add_provider(p2);

    // p1 declines everything.
    p1.set_handler([](const auto&) { return fujinet::diag::DiagResult::not_found("no"); });

    // p2 handles exactly "disk.slots" and returns a distinctive status/text.
    p2.set_handler([](const fujinet::diag::DiagArgsView& args) {
        if (!args.argv.empty() && args.argv[0] == "disk.slots") {
            fujinet::diag::DiagResult r;
            r.status = fujinet::diag::DiagStatus::Ok;
            r.text = "ok: slots";
            return r;
        }
        return fujinet::diag::DiagResult::not_found("no");
    });

    auto args = make_args("disk.slots", {"disk.slots"});
    auto r = reg.dispatch(args);
    CHECK(r.status == fujinet::diag::DiagStatus::Ok);
    CHECK(r.text == "ok: slots");
    CHECK(p1.execute_calls == 1);
    CHECK(p2.execute_calls == 1);
}

TEST_CASE("DiagnosticRegistry dispatch returns NotFound when no provider handles")
{
    fujinet::diag::DiagnosticRegistry reg;

    RecordingProvider p1("p1", {});
    RecordingProvider p2("p2", {});
    reg.add_provider(p1);
    reg.add_provider(p2);

    p1.set_handler([](const auto&) { return fujinet::diag::DiagResult::not_found("no"); });
    p2.set_handler([](const auto&) { return fujinet::diag::DiagResult::not_found("no"); });

    auto args = make_args("nope.cmd", {"nope.cmd"});
    auto r = reg.dispatch(args);
    CHECK(r.status == fujinet::diag::DiagStatus::NotFound);
}

TEST_CASE("DiagnosticRegistry dispatch stops at first provider that handles (non-NotFound)")
{
    fujinet::diag::DiagnosticRegistry reg;

    RecordingProvider p1("p1", {});
    RecordingProvider p2("p2", {});
    reg.add_provider(p1);
    reg.add_provider(p2);

    p1.set_handler([](const auto&) { return fujinet::diag::DiagResult::busy("busy here"); });
    p2.set_handler([](const auto&) { return fujinet::diag::DiagResult::ok("should not run"); });

    auto args = make_args("anything", {"anything"});
    auto r = reg.dispatch(args);
    CHECK(r.status == fujinet::diag::DiagStatus::Busy);
    CHECK(r.text == "busy here");
    CHECK(p1.execute_calls == 1);
    CHECK(p2.execute_calls == 0);
}

} // namespace fujinet::tests


