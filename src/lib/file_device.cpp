#include "fujinet/io/devices/file_device.h"

#include "fujinet/fs/filesystem.h"
#include "fujinet/core/logging.h"
#include "fujinet/io/core/io_message.h"

extern "C" {
#include "cJSON.h"
}

namespace fujinet::io {

using fujinet::fs::FileInfo;
using fujinet::fs::IFileSystem;
using fujinet::fs::StorageManager;

static const char* TAG = "io";

enum class FileCommand : std::uint8_t {
    ListDirectory = 0x01,
    // future: Open, Read, Write, Remove, etc.
};

FileDevice::FileDevice(StorageManager& storage)
    : _storage(storage)
{}

// Small helper to build a base response
static IOResponse make_base_response(const IORequest& req, StatusCode status)
{
    IOResponse resp;
    resp.id       = req.id;
    resp.deviceId = req.deviceId;
    resp.command  = req.command;
    resp.status   = status;
    return resp;
}

IOResponse FileDevice::handle(const IORequest& request)
{
    auto cmd = static_cast<FileCommand>(request.command & 0xFF);

    switch (cmd) {
        case FileCommand::ListDirectory:
            return handle_list_directory(request);

        default: {
            auto resp = make_base_response(request, StatusCode::Unsupported);
            return resp;
        }
    }
}

IOResponse FileDevice::handle_list_directory(const IORequest& request)
{
    IOResponse resp = make_base_response(request, StatusCode::Ok);

    // Payload: JSON { "fs": "sd0", "path": "/foo" }
    const std::string json_str(request.payload.begin(), request.payload.end());

    cJSON* root = cJSON_ParseWithLength(json_str.c_str(), json_str.size());
    if (!root) {
        FN_LOGW(TAG, "Invalid JSON in ListDirectory payload");
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    auto delete_json = [] (cJSON* p) { cJSON_Delete(p); };
    std::unique_ptr<cJSON, decltype(delete_json)> root_guard(root, delete_json);

    cJSON* fs_item   = cJSON_GetObjectItemCaseSensitive(root, "fs");
    cJSON* path_item = cJSON_GetObjectItemCaseSensitive(root, "path");

    if (!cJSON_IsString(fs_item) || !cJSON_IsString(path_item) ||
        fs_item->valuestring == nullptr || path_item->valuestring == nullptr) {
        FN_LOGW(TAG, "ListDirectory missing 'fs' or 'path'");
        resp.status = StatusCode::InvalidRequest;
        return resp;
    }

    const std::string fsname = fs_item->valuestring;
    const std::string path   = path_item->valuestring;

    IFileSystem* fs = _storage.get(fsname);
    if (!fs) {
        FN_LOGW(TAG, "Filesystem '%s' not found", fsname.c_str());
        resp.status = StatusCode::DeviceNotFound;
        return resp;
    }

    std::vector<FileInfo> entries;
    if (!fs->listDirectory(path, entries)) {
        FN_LOGW(TAG, "listDirectory failed for fs='%s', path='%s'",
                fsname.c_str(), path.c_str());
        resp.status = StatusCode::IOError;
        return resp;
    }

    auto basename_from_path = [](const std::string& p) -> std::string {
        if (p.empty()) return {};
        auto pos = p.find_last_of('/');
        if (pos == std::string::npos) return p;
        if (pos + 1 >= p.size()) return p;
        return p.substr(pos + 1);
    };

    cJSON* out = cJSON_CreateArray();
    if (!out) {
        resp.status = StatusCode::InternalError;
        return resp;
    }
    std::unique_ptr<cJSON, decltype(delete_json)> out_guard(out, delete_json);

    for (const auto& e : entries) {
        cJSON* obj = cJSON_CreateObject();
        if (!obj) {
            resp.status = StatusCode::InternalError;
            return resp;
        }

        const std::string& path_in_fs = e.path;
        const std::string name        = basename_from_path(e.path);

        cJSON_AddStringToObject(obj, "path", path_in_fs.c_str());
        cJSON_AddStringToObject(obj, "name", name.c_str());
        cJSON_AddNumberToObject(obj, "sizeBytes",
                                static_cast<double>(e.sizeBytes));
        cJSON_AddBoolToObject(obj, "dir", e.isDirectory);

        cJSON_AddItemToArray(out, obj);
    }

    char* out_str = cJSON_PrintUnformatted(out);
    if (!out_str) {
        resp.status = StatusCode::InternalError;
        return resp;
    }

    std::string out_json(out_str);
    cJSON_free(out_str);

    resp.status = StatusCode::Ok;
    resp.payload.assign(out_json.begin(), out_json.end());
    return resp;
}

} // namespace fujinet::io
