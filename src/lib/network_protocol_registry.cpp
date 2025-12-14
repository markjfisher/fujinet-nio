#include "fujinet/io/devices/network_protocol_registry.h"

#include "fujinet/io/devices/network_protocol.h"

namespace fujinet::io {

bool ProtocolRegistry::register_scheme(std::string schemeLower, Factory factory)
{
    if (schemeLower.empty() || !factory) {
        return false;
    }

    auto it = _factories.find(schemeLower);
    if (it != _factories.end()) {
        // already registered
        return false;
    }

    _factories.emplace(std::move(schemeLower), std::move(factory));
    return true;
}

std::unique_ptr<INetworkProtocol> ProtocolRegistry::create(std::string_view schemeLower) const
{
    auto it = _factories.find(std::string(schemeLower));
    if (it == _factories.end()) {
        return nullptr;
    }
    return (it->second)();
}

} // namespace fujinet::io


