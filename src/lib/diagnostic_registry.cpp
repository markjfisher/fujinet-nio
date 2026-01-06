#include "fujinet/diag/diagnostic_registry.h"

namespace fujinet::diag {

void DiagnosticRegistry::add_provider(IDiagnosticProvider& p)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _providers.push_back(&p);
}

void DiagnosticRegistry::list_all_commands(std::vector<DiagCommandSpec>& out) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto* p : _providers) {
        p->list_commands(out);
    }
}

DiagResult DiagnosticRegistry::dispatch(const DiagArgsView& args)
{
    std::lock_guard<std::mutex> lock(_mutex);
    for (auto* p : _providers) {
        DiagResult r = p->execute(args);
        if (r.status != DiagStatus::NotFound) {
            return r;
        }
    }
    return DiagResult::not_found("Unknown command");
}

} // namespace fujinet::diag


