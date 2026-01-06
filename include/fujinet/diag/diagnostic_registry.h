#pragma once

#include "fujinet/diag/diagnostic_provider.h"

#include <mutex>
#include <vector>

namespace fujinet::diag {

class DiagnosticRegistry {
public:
    DiagnosticRegistry() = default;

    void add_provider(IDiagnosticProvider& p);

    void list_all_commands(std::vector<DiagCommandSpec>& out) const;

    // Dispatch by asking providers in registration order.
    // Convention: providers return NotFound when they don't handle the command.
    DiagResult dispatch(const DiagArgsView& args);

private:
    mutable std::mutex _mutex;
    std::vector<IDiagnosticProvider*> _providers;
};

} // namespace fujinet::diag
