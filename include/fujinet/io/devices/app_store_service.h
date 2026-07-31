#pragma once

#include "fujinet/io/devices/app_store.h"
#include "fujinet/io/devices/virtual_device.h"

#include <memory>

namespace fujinet::io {

class AppStoreService : public VirtualService {
public:
    explicit AppStoreService(std::shared_ptr<AppStore> store);

    IOResponse handle(const IORequest& request) override;

private:
    std::shared_ptr<AppStore> _store;

    IOResponse handle_stat(const IORequest& request);
    IOResponse handle_read(const IORequest& request);
    IOResponse handle_write(const IORequest& request);
    IOResponse handle_delete(const IORequest& request);
    IOResponse handle_list(const IORequest& request);
};

} // namespace fujinet::io
