#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/core.h"

#include <string>

namespace fujinet {
const char* version();
} // namespace fujinet

namespace fujinet::diag {

namespace {

class CoreDiagnosticProvider final : public IDiagnosticProvider {
public:
    explicit CoreDiagnosticProvider(fujinet::core::FujinetCore& core)
        : _core(core)
    {}

    std::string_view provider_id() const noexcept override { return "core"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "core.info",
            .summary = "build/version information",
            .usage = "core.info",
        });
        out.push_back(DiagCommandSpec{
            .name = "core.stats",
            .summary = "core runtime statistics",
            .usage = "core.stats",
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }

        const std::string_view cmd = args.argv[0];
        if (cmd == "core.info") {
            return cmd_info();
        }
        if (cmd == "core.stats") {
            return cmd_stats();
        }

        return DiagResult::not_found("unknown core command");
    }

private:
    DiagResult cmd_info()
    {
        const auto profile = build::current_build_profile();

        DiagResult r = DiagResult::ok();
        r.kv.emplace_back("version", fujinet::version());
        r.kv.emplace_back("build_profile", std::string(profile.name));

        r.text.reserve(128);
        r.text += "version: ";
        r.text += fujinet::version();
        r.text += "\nprofile: ";
        r.text.append(profile.name.data(), profile.name.size());
        r.text += "\r\n";

        return r;
    }

    DiagResult cmd_stats()
    {
        const std::uint64_t ticks = _core.tick_count();
        const std::size_t devs = _core.deviceManager().device_count();

        DiagResult r = DiagResult::ok();
        r.kv.emplace_back("tick_count", std::to_string(ticks));
        r.kv.emplace_back("devices_registered", std::to_string(devs));

        r.text.reserve(128);
        r.text += "tick_count: ";
        r.text += std::to_string(ticks);
        r.text += "\ndevices_registered: ";
        r.text += std::to_string(devs);
        r.text += "\r\n";

        return r;
    }

    fujinet::core::FujinetCore& _core;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_core_diagnostic_provider(fujinet::core::FujinetCore& core)
{
    return std::make_unique<CoreDiagnosticProvider>(core);
}

} // namespace fujinet::diag


