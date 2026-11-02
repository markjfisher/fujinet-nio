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

bool FsStorage::list_files(const std::string& dir, std::vector<std::string>& names)
{
    std::vector<fs::FileInfo> entries;
    if (!_fs.isDirectory(dir) || !_fs.listDirectory(dir, entries)) {
        return false;
    }
    for (const fs::FileInfo& entry : entries) {
        if (entry.isDirectory) {
            continue;
        }
        // Entries may come back as full paths or bare names.
        const std::size_t slash = entry.path.rfind('/');
        names.push_back(slash == std::string::npos ? entry.path : entry.path.substr(slash + 1));
    }
    return true;
}

std::vector<std::string> FsStorage::pending_installs()
{
    std::vector<std::string> names;
    ensure_dir(path("install"));
    list_files(path("install"), names);
    list_files(path("installing"), names);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

bool FsStorage::peek_install(const std::string& fileName, Bytes& out, std::size_t maxBytes)
{
    const std::string queued = path("install", fileName);
    return read_file(_fs.exists(queued) ? queued : path("installing", fileName), out, maxBytes);
}

bool FsStorage::claim_install(const std::string& fileName)
{
    const std::string queued = path("install", fileName);
    const std::string claimed = path("installing", fileName);
    if (!ensure_dir(path("installing"))) {
        return false;
    }
    // A file queued since an earlier sync claimed one of this name is newer.
    // If it is replaced while being claimed, leave both for the next sync.
    if (_fs.exists(queued)) {
        return replace_file(queued, claimed);
    }
    return _fs.exists(claimed);
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

// True only when every byte was written and flushed. The file is closed on
// return, before anything renames it.
bool FsStorage::write_file(const std::string& file, const Bytes& data)
{
    auto f = _fs.open(file, "wb");
    if (!f) {
        return false;
    }
    const bool written = f->write(data.data(), data.size()) == data.size();
    return f->flush() && written;
}

// Moves `from` to `to`, replacing any file there only once `from` is in its
// place. On failure `to` is as it was and `from` is left for the caller.
bool FsStorage::replace_file(const std::string& from, const std::string& to)
{
    const std::string aside = to + ".old";
    // A replace cut short after setting the old file aside left only that.
    if (!_fs.exists(to) && _fs.exists(aside)) {
        _fs.rename(aside, to);
    }
    if (_fs.rename(from, to)) {
        return true; // nothing at `to`, or a rename that replaces (POSIX)
    }
    if (!_fs.exists(from) || !_fs.exists(to)) {
        return false;
    }
    if (_fs.exists(aside)) {
        _fs.removeFile(aside);
    }
    if (!_fs.rename(to, aside)) {
        return false;
    }
    if (!_fs.rename(from, to)) {
        _fs.rename(aside, to);
        return false;
    }
    _fs.removeFile(aside);
    return true;
}

bool FsStorage::queue_install(const std::string& fileName, const Bytes& data)
{
    const std::string partial = path("incoming", fileName);
    if (!ensure_dir(path("incoming")) || !ensure_dir(path("install"))) {
        return false;
    }
    if (!write_file(partial, data) || !replace_file(partial, path("install", fileName))) {
        _fs.removeFile(partial);
        return false;
    }
    return true;
}

bool FsStorage::read_install(const std::string& fileName, Bytes& out, std::size_t maxBytes)
{
    return read_file(path("installing", fileName), out, maxBytes);
}

bool FsStorage::mark_installed(const std::string& fileName)
{
    return ensure_dir(path("installed")) &&
           replace_file(path("installing", fileName), path("installed", fileName));
}

bool FsStorage::write_backup(const std::string& user, const std::string& fileName, const Bytes& data)
{
    const std::string dir = path("backup/" + user);
    const std::string partial = dir + "/" + fileName + ".new";
    if (!ensure_dir(dir)) {
        return false;
    }
    if (!write_file(partial, data) || !replace_file(partial, dir + "/" + fileName)) {
        _fs.removeFile(partial);
        return false;
    }
    return true;
}

} // namespace fujinet::hotsync
