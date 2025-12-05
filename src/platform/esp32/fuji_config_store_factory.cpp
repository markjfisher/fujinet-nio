#include "fujinet/platform/fuji_config_store_factory.h"

#include "fujinet/config/fuji_config_yaml_store.h"

namespace fujinet::platform {

std::unique_ptr<fujinet::config::FujiConfigStore>
create_fuji_config_store(const std::string& rootHint)
{
    // TODO: decide SD vs flash; for now, assume SD at /sdcard/fujinet
    std::string root = rootHint.empty() ? "/fujifs" : rootHint;
    std::string path = root + "/fujinet.yaml";
    return std::make_unique<fujinet::config::YamlFujiConfigStore>(std::move(path));
}

} // namespace fujinet::platform
