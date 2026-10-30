// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/fs_storage.h"

#include "fujinet/fs/filesystem.h"

#include <algorithm>
#include <utility>

namespace fujinet::hotsync {

FsStorage::FsStorage(fs::IFileSystem& fs, std::string root)
    : _fs(fs), _root(std::move(root))
{
    while (_root.size() > 1 && _root.back() == '/') {
        _root.pop_back();
    }
}

std::string FsStorage::path(const std::string& folder, const std::string& fileName) const
{
    std::string p = (_root == "/" ? std::string() : _root) + "/" + folder;
    if (!fileName.empty()) {
        p += "/" + fileName;
    }
    return p;
}

bool FsStorage::ensure_dir(const std::string& dir)
{
    if (_fs.isDirectory(dir)) {
        return true;
    }
    // createDirectory need not be recursive, so build each level in turn.
    for (std::size_t slash = dir.find('/', 1);; slash = dir.find('/', slash + 1)) {
        const std::string level = dir.substr(0, slash);
        if (!_fs.isDirectory(level) && !_fs.createDirectory(level)) {
            return false;
        }
        if (slash == std::string::npos) {
            return true;
        }
    }
}

bool FsStorage::create_install_folder()
{
    return ensure_dir(path("install"));
}

std::vector<std::string> FsStorage::pending_installs()
{
    std::vector<std::string> names;
    const std::string dir = path("install");
    std::vector<fs::FileInfo> entries;
    if (!ensure_dir(dir) || !_fs.listDirectory(dir, entries)) {
        return names;
    }
    for (const fs::FileInfo& entry : entries) {
        if (entry.isDirectory) {
            continue;
        }
        // Entries may come back as full paths or bare names.
        const std::size_t slash = entry.path.rfind('/');
        names.push_back(slash == std::string::npos ? entry.path : entry.path.substr(slash + 1));
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool FsStorage::read_file(const std::string& file, Bytes& out, std::size_t maxBytes)
{
    fs::FileInfo info;
    if (!_fs.stat(file, info) || info.isDirectory || info.sizeBytes == 0 || info.sizeBytes > maxBytes) {
        return false;
    }
    auto f = _fs.open(file, "rb");
    if (!f) {
        return false;
    }
    out.resize(static_cast<std::size_t>(info.sizeBytes));
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t got = f->read(out.data() + done, out.size() - done);
        if (got == 0) {
            break;
        }
        done += got;
    }
    return done == out.size();
}

bool FsStorage::write_file(const std::string& dir, const std::string& fileName, const Bytes& data)
{
    if (!ensure_dir(dir)) {
        return false;
    }
    auto f = _fs.open(dir + "/" + fileName, "wb");
    if (!f) {
        return false;
    }
    const bool ok = f->write(data.data(), data.size()) == data.size();
    return f->flush() && ok;
}

bool FsStorage::queue_install(const std::string& fileName, const Bytes& data)
{
    const std::string partial = path("incoming", fileName);
    const std::string queued = path("install", fileName);
    if (!write_file(path("incoming"), fileName, data) || !ensure_dir(path("install"))) {
        return false;
    }
    if (_fs.exists(queued)) {
        _fs.removeFile(queued);
    }
    if (!_fs.rename(partial, queued)) {
        _fs.removeFile(partial);
        return false;
    }
    return true;
}

bool FsStorage::read_install(const std::string& fileName, Bytes& out, std::size_t maxBytes)
{
    return read_file(path("install", fileName), out, maxBytes);
}

bool FsStorage::mark_installed(const std::string& fileName)
{
    const std::string done = path("installed", fileName);
    if (!ensure_dir(path("installed"))) {
        return false;
    }
    if (_fs.exists(done)) {
        _fs.removeFile(done);
    }
    return _fs.rename(path("install", fileName), done);
}

bool FsStorage::write_backup(const std::string& user, const std::string& fileName, const Bytes& data)
{
    return write_file(path("backup/" + user), fileName, data);
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
