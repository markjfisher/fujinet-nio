#include "fujinet/console/fs_shell.h"

#include "fujinet/console/console_engine.h" // IConsoleTransport
#include "fujinet/fs/filesystem.h"
#include "fujinet/fs/storage_manager.h"
#include "fujinet/platform/time.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace fujinet::console {

namespace {

static std::string fmt_time_ls(std::chrono::system_clock::time_point tp)
{
    if (tp == std::chrono::system_clock::time_point{}) {
        return "??? ?? ??:??";
    }

    char buf[16];
    const std::uint64_t secs = static_cast<std::uint64_t>(std::chrono::system_clock::to_time_t(tp));
    if (!fujinet::platform::format_time_utc_ls(secs, buf, sizeof(buf))) {
        return "??? ?? ??:??";
    }
    return std::string(buf);
}

static std::string fmt_size(std::uint64_t bytes)
{
    // Fixed-ish width, human-ish (base-1024). Examples:
    //   314 -> "314"
    //   5271 -> "5.1K"
    //   102400 -> "100K"
    char buf[16];
    if (bytes < 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(bytes));
        return std::string(buf);
    }
    if (bytes < 1024ULL * 1024ULL) {
        const double k = static_cast<double>(bytes) / 1024.0;
        if (k >= 100.0) {
            std::snprintf(buf, sizeof(buf), "%.0fK", k);
        } else if (k >= 10.0) {
            std::snprintf(buf, sizeof(buf), "%.1fK", k);
        } else {
            std::snprintf(buf, sizeof(buf), "%.2fK", k);
        }
        return std::string(buf);
    }
    if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        const double m = static_cast<double>(bytes) / (1024.0 * 1024.0);
        if (m >= 100.0) {
            std::snprintf(buf, sizeof(buf), "%.0fM", m);
        } else if (m >= 10.0) {
            std::snprintf(buf, sizeof(buf), "%.1fM", m);
        } else {
            std::snprintf(buf, sizeof(buf), "%.2fM", m);
        }
        return std::string(buf);
    }
    const double g = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    if (g >= 100.0) {
        std::snprintf(buf, sizeof(buf), "%.0fG", g);
    } else if (g >= 10.0) {
        std::snprintf(buf, sizeof(buf), "%.1fG", g);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2fG", g);
    }
    return std::string(buf);
}

static std::string pad_left(std::string_view s, std::size_t width)
{
    if (s.size() >= width) return std::string(s);
    std::string out;
    out.reserve(width);
    out.append(width - s.size(), ' ');
    out.append(s.data(), s.size());
    return out;
}

static std::string fs_join(std::string_view base, std::string_view rel)
{
    if (base.empty()) return std::string(rel);
    if (base.back() == '/') {
        if (!rel.empty() && rel.front() == '/') {
            return std::string(base) + std::string(rel.substr(1));
        }
        return std::string(base) + std::string(rel);
    }
    if (!rel.empty() && rel.front() == '/') {
        return std::string(base) + std::string(rel);
    }
    std::string out(base);
    out.push_back('/');
    out.append(rel.data(), rel.size());
    return out;
}

static std::string fs_norm(std::string_view in)
{
    // Normalize to absolute POSIX-like path.
    std::vector<std::string_view> parts;
    std::string_view s = in;

    // split on '/'
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == '/') ++i;
        const std::size_t start = i;
        while (i < s.size() && s[i] != '/') ++i;
        if (i == start) break;
        parts.push_back(s.substr(start, i - start));
    }

    std::vector<std::string_view> stack;
    for (auto p : parts) {
        if (p == "." || p.empty()) continue;
        if (p == "..") {
            if (!stack.empty()) stack.pop_back();
            continue;
        }
        stack.push_back(p);
    }

    std::string out;
    out.push_back('/');
    for (std::size_t k = 0; k < stack.size(); ++k) {
        if (k != 0) out.push_back('/');
        out.append(stack[k].data(), stack[k].size());
    }
    return out;
}

struct FsPath {
    std::string fs;
    std::string path; // absolute within fs
};

static bool parse_fs_path(
    std::string_view spec,
    std::string_view cur_fs,
    std::string_view cur_path,
    FsPath& out)
{
    // Supports:
    // - "sd0:/dir"
    // - "sd0:" (meaning "/")
    // - "/dir" absolute in current fs
    // - "dir" relative in current fs
    const std::size_t colon = spec.find(':');
    if (colon != std::string_view::npos) {
        out.fs = std::string(spec.substr(0, colon));
        std::string_view p = spec.substr(colon + 1);
        if (p.empty()) p = "/";
        if (!p.empty() && p.front() != '/') {
            std::string tmp("/");
            tmp.append(p.data(), p.size());
            out.path = fs_norm(tmp);
        } else {
            out.path = fs_norm(p);
        }
        return true;
    }

    if (cur_fs.empty()) {
        return false;
    }
    out.fs = std::string(cur_fs);
    if (!spec.empty() && spec.front() == '/') {
        out.path = fs_norm(spec);
        return true;
    }
    out.path = fs_norm(fs_join(cur_path, spec));
    return true;
}

} // namespace

bool FsShell::handle(
    fujinet::fs::StorageManager& storage,
    IConsoleTransport& io,
    const std::vector<std::string_view>& argv,
    std::string& cwd_fs,
    std::string& cwd_path
)
{
    if (argv.empty()) return false;
    const std::string_view cmd0 = argv[0];

    if (cmd0 == "fs") {
        auto names = storage.listNames();
        std::sort(names.begin(), names.end());
        if (names.empty()) {
            io.write_line("(no filesystems registered)");
            return true;
        }
        for (const auto& n : names) {
            io.write_line(n);
        }
        return true;
    }

    if (cmd0 == "pwd") {
        if (cwd_fs.empty()) {
            io.write_line("(no filesystem selected)");
            return true;
        }
        io.write(cwd_fs);
        io.write(":");
        io.write_line(cwd_path);
        return true;
    }

    if (cmd0 == "cd") {
        if (argv.size() < 2) {
            // keep it friendly
            std::vector<std::string_view> tmp;
            tmp.push_back("pwd");
            return handle(storage, io, tmp, cwd_fs, cwd_path);
        }

        FsPath target;
        if (!parse_fs_path(argv[1], cwd_fs, cwd_path, target)) {
            io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
            return true;
        }

        auto* fs = storage.get(target.fs);
        if (!fs) {
            io.write_line("error: unknown filesystem");
            return true;
        }
        if (!fs->exists(target.path) || !fs->isDirectory(target.path)) {
            io.write_line("error: not a directory");
            return true;
        }

        cwd_fs = target.fs;
        cwd_path = target.path;
        return true;
    }

    if (cmd0 == "mkdir" || cmd0 == "rmdir" || cmd0 == "ls") {
        FsPath target;
        if (argv.size() >= 2) {
            if (!parse_fs_path(argv[1], cwd_fs, cwd_path, target)) {
                io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
                return true;
            }
        } else {
            if (cwd_fs.empty()) {
                io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
                return true;
            }
            target.fs = cwd_fs;
            target.path = cwd_path;
        }

        auto* fs = storage.get(target.fs);
        if (!fs) {
            io.write_line("error: unknown filesystem");
            return true;
        }

        if (cmd0 == "mkdir") {
            if (!fs->createDirectory(target.path)) {
                io.write_line("error: mkdir failed");
            }
            return true;
        }
        if (cmd0 == "rmdir") {
            if (!fs->removeDirectory(target.path)) {
                io.write_line("error: rmdir failed");
            }
            return true;
        }

        // ls
        if (!fs->exists(target.path)) {
            io.write_line("error: not found");
            return true;
        }

        fujinet::fs::FileInfo st;
        if (!fs->stat(target.path, st)) {
            io.write_line("error: stat failed");
            return true;
        }

        if (!st.isDirectory) {
            io.write(target.fs);
            io.write(":");
            io.write_line(target.path);
            const char type = st.isDirectory ? 'd' : 'f';
            const std::string sz = fmt_size(st.sizeBytes);
            const std::string dt = fmt_time_ls(st.modifiedTime);
            io.write(std::string_view(&type, 1));
            io.write(" ");
            io.write(pad_left(sz, 8));
            io.write(" ");
            io.write(dt);
            io.write(" ");
            io.write_line(st.path);
            return true;
        }

        std::vector<fujinet::fs::FileInfo> entries;
        if (!fs->listDirectory(target.path, entries)) {
            io.write_line("error: ls failed");
            return true;
        }

        // Deterministic ordering: dirs first, then by path.
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
            return a.path < b.path;
        });

        io.write(target.fs);
        io.write(":");
        io.write(target.path);
        io.write(" (count=");
        io.write(std::to_string(entries.size()));
        io.write_line(")");

        auto leaf_name = [&](const std::string& p) -> std::string_view {
            const std::size_t slash = p.find_last_of('/');
            if (slash == std::string::npos) return p;
            return std::string_view(p).substr(slash + 1);
        };

        for (const auto& e : entries) {
            const char type = e.isDirectory ? 'd' : 'f';
            const std::string sz = fmt_size(e.sizeBytes);
            const std::string dt = fmt_time_ls(e.modifiedTime);
            io.write(std::string_view(&type, 1));
            io.write(" ");
            io.write(pad_left(sz, 8));
            io.write(" ");
            io.write(dt);
            io.write(" ");
            io.write_line(leaf_name(e.path));
        }

        return true;
    }

    if (cmd0 == "mv") {
        if (argv.size() < 3) {
            io.write_line("error: usage: mv <from> <to>");
            return true;
        }

        FsPath from;
        FsPath to;
        if (!parse_fs_path(argv[1], cwd_fs, cwd_path, from) ||
            !parse_fs_path(argv[2], cwd_fs, cwd_path, to)) {
            io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
            return true;
        }
        if (from.fs != to.fs) {
            io.write_line("error: mv across filesystems is not supported");
            return true;
        }

        auto* fs = storage.get(from.fs);
        if (!fs) {
            io.write_line("error: unknown filesystem");
            return true;
        }
        if (!fs->rename(from.path, to.path)) {
            io.write_line("error: mv failed");
        }
        return true;
    }

    return false;
}

} // namespace fujinet::console


