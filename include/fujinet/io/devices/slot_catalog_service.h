#pragma once

#include "fujinet/io/devices/app_store.h"
#include "fujinet/io/devices/slot_catalog.h"
#include "fujinet/io/devices/virtual_device.h"
#include "fujinet/fs/storage_manager.h"

#include <memory>

namespace fujinet::io {

class SlotCatalogService : public VirtualService {
public:
    SlotCatalogService(fs::StorageManager& storage, std::shared_ptr<AppStore> store);

    IOResponse handle(const IORequest& request) override;

private:
    fs::StorageManager& _storage;
    std::shared_ptr<AppStore> _store;
    SlotCatalog _catalog;

    IOResponse handle_get(const IORequest& request);
    IOResponse handle_put(const IORequest& request);
    IOResponse handle_delete(const IORequest& request);
    IOResponse handle_range(const IORequest& request);
};

} // namespace fujinet::io
