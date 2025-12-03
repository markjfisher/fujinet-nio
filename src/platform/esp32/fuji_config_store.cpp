#include "fujinet/platform/fuji_config_store_factory.h"

#include <memory>

namespace fujinet::platform {

using fujinet::core::FujiConfig;
using fujinet::core::FujiConfigStore;

class Esp32FujiConfigStore : public FujiConfigStore {
public:
    explicit Esp32FujiConfigStore(std::string root)
        : _root(std::move(root)) {}

    FujiConfig load() override {
        FujiConfig cfg;
        // TODO: actually load from SD/flash using _root
        return cfg;
    }

    void save(const FujiConfig& cfg) override {
        (void)cfg;
        // TODO: write to SD/flash
    }

private:
    std::string _root;
};

std::unique_ptr<FujiConfigStore>
create_fuji_config_store(const std::string& rootHint)
{
    // For now, ignore SD-vs-flash decision; just pick something.
    std::string root = rootHint.empty() ? "/sdcard/fujinet" : rootHint;
    return std::make_unique<Esp32FujiConfigStore>(std::move(root));
}

} // namespace fujinet::platform
