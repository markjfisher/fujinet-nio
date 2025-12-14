#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace fujinet::io {

class INetworkProtocol;

class ProtocolRegistry {
public:
    using Factory = std::function<std::unique_ptr<INetworkProtocol>()>;

    bool register_scheme(std::string schemeLower, Factory factory);

    // Returns nullptr if the scheme is not registered.
    std::unique_ptr<INetworkProtocol> create(std::string_view schemeLower) const;

private:
    std::unordered_map<std::string, Factory> _factories;
};

} // namespace fujinet::io


