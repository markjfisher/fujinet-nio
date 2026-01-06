#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace fujinet::diag {

enum class DiagStatus : std::uint8_t {
    Ok = 0,
    Error,
    NotFound,
    InvalidArgs,
    NotReady,
    Busy,
};

struct DiagResult {
    DiagStatus status{DiagStatus::Ok};

    // Human-readable output. Keep it line-oriented.
    std::string text;

    // Optional structured output for future tooling.
    // Keep it simple: key/value strings.
    std::vector<std::pair<std::string, std::string>> kv;

    static DiagResult ok(std::string t = {}) {
        DiagResult r;
        r.status = DiagStatus::Ok;
        r.text = std::move(t);
        return r;
    }

    static DiagResult error(std::string t) {
        DiagResult r;
        r.status = DiagStatus::Error;
        r.text = std::move(t);
        return r;
    }

    static DiagResult invalid_args(std::string t) {
        DiagResult r;
        r.status = DiagStatus::InvalidArgs;
        r.text = std::move(t);
        return r;
    }

    static DiagResult not_found(std::string t) {
        DiagResult r;
        r.status = DiagStatus::NotFound;
        r.text = std::move(t);
        return r;
    }

    static DiagResult not_ready(std::string t) {
        DiagResult r;
        r.status = DiagStatus::NotReady;
        r.text = std::move(t);
        return r;
    }

    static DiagResult busy(std::string t) {
        DiagResult r;
        r.status = DiagStatus::Busy;
        r.text = std::move(t);
        return r;
    }
};

struct DiagCommandSpec {
    // Fully-qualified command name, e.g. "net.sessions", "fs.list", "core.stats"
    std::string name;

    // Short help line
    std::string summary;

    // Usage string, e.g. "net.sessions" or "net.info <handle>"
    std::string usage;
};

// A lightweight, immutable view of argv
struct DiagArgsView {
    std::string_view line;
    std::vector<std::string_view> argv;  // argv[0] is command, rest are args
};

} // namespace fujinet::diag
