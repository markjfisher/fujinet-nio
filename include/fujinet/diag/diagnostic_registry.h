#pragma once

#include "fujinet/diag/diagnostic_provider.h"

#include <vector>

namespace fujinet::diag {

class DiagnosticRegistry {
public:
    void add_provider(IDiagnosticProvider& p) {
        _providers.push_back(&p);
    }

    void list_all_commands(std::vector<DiagCommandSpec>& out) const {
        for (const auto* p : _providers) {
            p->list_commands(out);
        }
    }

    // Dispatch by asking providers in registration order.
    // Convention: providers return NotFound when they don't handle the command.
    DiagResult dispatch(const DiagArgsView& args) {
        for (auto* p : _providers) {
            DiagResult r = p->execute(args);
            if (r.status != DiagStatus::NotFound) {
                return r;
            }
        }
        return DiagResult::not_found("Unknown command");
    }

private:
    std::vector<IDiagnosticProvider*> _providers;
};

} // namespace fujinet::diag
