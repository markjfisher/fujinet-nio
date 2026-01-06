#pragma once

#include "fujinet/diag/diagnostic_types.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace fujinet::diag {

// Providers publish a set of commands and execute them.
// The registry owns no provider state; providers own their own state (core services/devices).
class IDiagnosticProvider {
public:
    virtual ~IDiagnosticProvider() = default;

    // Stable provider identifier, e.g. "core", "net", "fs", "clock".
    virtual std::string_view provider_id() const noexcept = 0;

    // A provider returns the commands it implements.
    // This must be stable after registration (avoid dynamic changes).
    virtual void list_commands(std::vector<DiagCommandSpec>& out) const = 0;

    // Execute a command. `args.argv[0]` is the command name.
    // Provider should return NotFound if it doesn't recognize the command.
    virtual DiagResult execute(const DiagArgsView& args) = 0;
};

} // namespace fujinet::diag
