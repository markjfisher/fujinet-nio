#include "fujinet/platform/fuji_config_store_factory.h"

#include <memory>

namespace fujinet::platform {

using fujinet::core::FujiConfig;
using fujinet::core::FujiConfigStore;

class PosixFujiConfigStore : public FujiConfigStore {
public:
    explicit PosixFujiConfigStore(std::string root)
        : _root(std::move(root)) {}

    FujiConfig load() override {
        FujiConfig cfg;
        // TODO: read ./fujinet_config.json or similar
        return cfg;
    }

    void save(const FujiConfig& cfg) override {
        (void)cfg;
        // TODO: write to ./fujinet_config.json
    }

private:
    std::string _root;
};

std::unique_ptr<FujiConfigStore>
create_fuji_config_store(const std::string& root)
{
    return std::make_unique<PosixFujiConfigStore>(root.empty() ? "." : root);
}

} // namespace fujinet::platform
