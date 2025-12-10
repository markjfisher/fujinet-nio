#include "fujinet/io/devices/fuji_device.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/core/logging.h"

extern "C" {
#include "cJSON.h"
}

namespace fujinet::io {

using fujinet::config::FujiConfigStore;
using fujinet::config::FujiConfig;

using protocol::FujiCommandId;
using protocol::FujiDeviceId;
using protocol::to_fuji_command;

FujiDevice::FujiDevice(ResetHandler resetHandler,
                       std::unique_ptr<FujiConfigStore> configStore,
                       fs::StorageManager& storage)
    : _resetHandler(std::move(resetHandler))
    , _configStore(std::move(configStore))
    , _storage(storage)
{
    load_config();
}

IOResponse FujiDevice::make_base_response(const IORequest& request,
                                          StatusCode status) const
{
    IOResponse resp;
    resp.id       = request.id;
    resp.deviceId = request.deviceId;
    resp.command  = request.command;
    resp.status   = status;
    return resp;
}

IOResponse FujiDevice::handle(const IORequest& request)
{
    auto cmd = to_fuji_command(request.command);

    switch (cmd) {
        case FujiCommandId::Reset:
            return handle_reset(request);

        case FujiCommandId::DEBUG:
            return handle_debug(request);

        // later:
        // case FujiCommandId::GetSsid:
        //     return handle_get_ssid(request);

        default:
            return handle_unknown(request);
    }
}

void FujiDevice::poll()
{
    // Background work later (autosave, timers, etc).
}

IOResponse FujiDevice::handle_reset(const IORequest& request)
{
    // We *could* respond first, then reset.
    auto resp = make_base_response(request, StatusCode::Ok);

    if (_resetHandler) {
        _resetHandler();
    }

    // On ESP32, reset handler will likely never return.
    return resp;
}

IOResponse FujiDevice::handle_unknown(const IORequest& request)
{
    return make_base_response(request, StatusCode::Unsupported);
}

void FujiDevice::load_config()
{
    if (_configStore) {
        _config = _configStore->load();
    }
}

void FujiDevice::save_config()
{
    if (_configStore) {
        _configStore->save(_config);
    }
}

IOResponse FujiDevice::handle_debug(const IORequest& request)
{
    IOResponse resp = make_base_response(request, StatusCode::Ok);

    // Interpret payload as UTF-8 JSON text: { "fs": "sd0", "path": "/some/dir" }
    const std::string json_str(request.payload.begin(), request.payload.end());

    cJSON* root = cJSON_ParseWithLength(json_str.c_str(), json_str.size());
    if (!root) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto delete_json = [] (cJSON* p) { cJSON_Delete(p); };
    std::unique_ptr<cJSON, decltype(delete_json)> root_guard(root, delete_json);

    cJSON* fs_item   = cJSON_GetObjectItemCaseSensitive(root, "fs");
    cJSON* path_item = cJSON_GetObjectItemCaseSensitive(root, "path");

    if (!cJSON_IsString(fs_item) || !cJSON_IsString(path_item) ||
        fs_item->valuestring == nullptr || path_item->valuestring == nullptr) {
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    const std::string fsname = fs_item->valuestring;
    const std::string path   = path_item->valuestring;

    // Look up filesystem by name
    fs::IFileSystem* fs = _storage.get(fsname);
    if (!fs) {
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    // List directory
    std::vector<fs::FileInfo> entries;
    if (!fs->listDirectory(path, entries)) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    // Helper: basename from FileInfo::path ("/foo/bar.txt" → "bar.txt")
    auto basename_from_path = [](const std::string& p) -> std::string {
        if (p.empty()) return {};
        auto pos = p.find_last_of('/');
        if (pos == std::string::npos) return p;
        if (pos + 1 >= p.size()) return p; // weird but safe
        return p.substr(pos + 1);
    };

    // Build JSON array of entries
    cJSON* out = cJSON_CreateArray();
    if (!out) {
        resp.status = StatusCode::IOError;
        return resp;
    }
    std::unique_ptr<cJSON, decltype(delete_json)> out_guard(out, delete_json);

    for (const auto& e : entries) {
        cJSON* obj = cJSON_CreateObject();
        if (!obj) {
            resp.status = StatusCode::IOError;
            return resp;
        }

        const std::string& path_in_fs = e.path;
        const std::string name        = basename_from_path(e.path);

        // Path is always relative to FS root (e.g. "/foo/bar.txt")
        cJSON_AddStringToObject(obj, "path", path_in_fs.c_str());
        cJSON_AddStringToObject(obj, "name", name.c_str());
        cJSON_AddNumberToObject(obj, "sizeBytes",
                                static_cast<double>(e.sizeBytes));
        cJSON_AddBoolToObject(obj, "dir", e.isDirectory);

        cJSON_AddItemToArray(out, obj); // array now owns obj
    }

    // Serialize without pretty-printing to keep it compact
    char* out_str = cJSON_PrintUnformatted(out);
    if (!out_str) {
        resp.status = StatusCode::IOError;
        return resp;
    }

    std::string out_json(out_str);
    cJSON_free(out_str);

    resp.status = StatusCode::Ok;
    resp.payload.assign(out_json.begin(), out_json.end());
    return resp;
}



} // namespace fujinet::io
