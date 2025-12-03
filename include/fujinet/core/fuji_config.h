#pragma once

#include <string>
#include <vector>

namespace fujinet::core {

struct HostSlot {
    std::string name;
    std::string url;
    bool        enabled{true};
};

struct FujiConfig {
    std::vector<HostSlot> hosts;
    // wifi, printer, etc later
};

class FujiConfigStore {
public:
    virtual ~FujiConfigStore() = default;

    virtual FujiConfig load() = 0;
    virtual void       save(const FujiConfig& cfg) = 0;
};

} // namespace fujinet::core
