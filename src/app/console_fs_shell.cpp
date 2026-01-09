#include "fujinet/console/fs_shell.h"

#include "fujinet/console/console_commands.h"
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

static bool has_wildcard(std::string_view s)
{
    return s.find('*') != std::string_view::npos || s.find('?') != std::string_view::npos;
}

static bool glob_match(std::string_view pat, std::string_view s)
{
    // Very small glob: '*' matches any sequence, '?' matches one char.
    // No character classes/escapes.
    std::size_t pi = 0, si = 0;
    std::size_t star = std::string_view::npos;
    std::size_t star_si = 0;

    while (si < s.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si])) {
            ++pi;
            ++si;
            continue;
        }
        if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            star_si = si;
            continue;
        }
        if (star != std::string_view::npos) {
            pi = star + 1;
            ++star_si;
            si = star_si;
            continue;
        }
        return false;
    }

    while (pi < pat.size() && pat[pi] == '*') ++pi;
    return pi == pat.size();
}

static std::string parent_path(std::string_view abs)
{
    if (abs.empty() || abs == "/") return "/";
    std::string_view s = abs;
    while (s.size() > 1 && s.back() == '/') s.remove_suffix(1);
    const std::size_t slash = s.find_last_of('/');
    if (slash == std::string_view::npos || slash == 0) return "/";
    return std::string(s.substr(0, slash));
}

static std::string leaf_name(std::string_view abs)
{
    if (abs.empty() || abs == "/") return "";
    std::string_view s = abs;
    while (s.size() > 1 && s.back() == '/') s.remove_suffix(1);
    const std::size_t slash = s.find_last_of('/');
    if (slash == std::string_view::npos) return std::string(s);
    if (slash + 1 >= s.size()) return "";
    return std::string(s.substr(slash + 1));
}

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

struct RmFlags {
    bool force{false};     // -f
    bool recursive{false}; // -r
};

static void parse_rm_flags(const std::vector<std::string_view>& argv, std::size_t& idx, RmFlags& out)
{
    // Accept -f, -r, -rf, -fr (repeated).
    while (idx < argv.size()) {
        const std::string_view a = argv[idx];
        if (a.size() < 2 || a[0] != '-') break;
        if (a == "--") { ++idx; break; }
        bool any = false;
        for (std::size_t i = 1; i < a.size(); ++i) {
            if (a[i] == 'f') { out.force = true; any = true; }
            else if (a[i] == 'r') { out.recursive = true; any = true; }
            else { any = false; break; }
        }
        if (!any) break;
        ++idx;
    }
}

static bool delete_tree(
    fujinet::fs::IFileSystem& fs,
    const std::string& absPath,
    const RmFlags& flags,
    bool require_dir_for_rmdir,
    IConsoleTransport& io)
{
    fujinet::fs::FileInfo st{};
    if (!fs.stat(absPath, st)) {
        if (!flags.force) {
            io.write_line("error: not found");
        }
        return flags.force;
    }

    if (st.isDirectory) {
        if (!flags.recursive) {
            io.write_line("error: is a directory (use -r)");
            return false;
        }

        std::vector<fujinet::fs::FileInfo> entries;
        if (!fs.listDirectory(absPath, entries)) {
            if (!flags.force) io.write_line("error: ls failed");
            return flags.force;
        }

        for (const auto& e : entries) {
            // Entries are absolute paths already.
            if (!delete_tree(fs, e.path, flags, false, io)) {
                if (!flags.force) return false;
            }
        }

        // Finally remove directory itself.
        if (!fs.removeDirectory(absPath)) {
            if (!flags.force) io.write_line("error: rmdir failed");
            return flags.force;
        }
        return true;
    }

    if (require_dir_for_rmdir) {
        io.write_line("error: not a directory");
        return false;
    }

    if (!fs.removeFile(absPath)) {
        if (!flags.force) io.write_line("error: rm failed");
        return flags.force;
    }
    return true;
}

static void fs_resolve_target(
    const std::vector<std::string_view>& argv,
    std::string_view cwd_fs,
    std::string_view cwd_path,
    FsPath& out,
    bool& ok
)
{
    ok = false;
    if (argv.size() >= 2) {
        ok = parse_fs_path(argv[1], cwd_fs, cwd_path, out);
        return;
    }
    if (cwd_fs.empty()) {
        return;
    }
    out.fs = std::string(cwd_fs);
    out.path = std::string(cwd_path);
    ok = true;
}

} // namespace

bool FsShell::register_commands(ConsoleCommandRegistry& reg, IConsoleTransport& io)
{
    bool ok = true;
    ok &= reg.register_command(ConsoleCommandSpec{"fs", "list mounted filesystems", "fs"}, [&](const auto& argv) {
        return this->cmd_fs(io, argv);
    });
    ok &= reg.register_command(ConsoleCommandSpec{"pwd", "show current filesystem path", "pwd"}, [&](const auto& argv) {
        return this->cmd_pwd(io, argv);
    });
    ok &= reg.register_command(ConsoleCommandSpec{"cd", "change directory; use fs:/ to select filesystem", "cd <fs:/>|<path>"}, [&](const auto& argv) {
        return this->cmd_cd(io, argv);
    });
    ok &= reg.register_command(ConsoleCommandSpec{"ls", "list directory (or stat file)", "ls [<fs:/>|<path>]"}, [&](const auto& argv) {
        return this->cmd_ls(io, argv);
    });
    ok &= reg.register_command(ConsoleCommandSpec{"mkdir", "create directory", "mkdir <path>"}, [&](const auto& argv) {
        return this->cmd_mkdir(io, argv);
    });
    ok &= reg.register_command(ConsoleCommandSpec{"rm", "remove file(s)", "rm [-f] [-r] <path...>"}, [&](const auto& argv) {
        return this->cmd_rm(io, argv);
    });
    ok &= reg.register_command(ConsoleCommandSpec{"rmdir", "remove directory", "rmdir [-f] [-r] <path>"}, [&](const auto& argv) {
        return this->cmd_rmdir(io, argv);
    });
    ok &= reg.register_command(ConsoleCommandSpec{"mv", "rename within a filesystem", "mv <from> <to>"}, [&](const auto& argv) {
        return this->cmd_mv(io, argv);
    });
    if (!ok) {
        io.write_line("error: FsShell failed to register one or more commands (name collision?)");
    }
    return ok;
}

bool FsShell::cmd_fs(IConsoleTransport& io, const std::vector<std::string_view>& /*argv*/)
{
    auto names = _storage.listNames();
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

bool FsShell::cmd_pwd(IConsoleTransport& io, const std::vector<std::string_view>& /*argv*/)
{
    if (_cwd_fs.empty()) {
        io.write_line("(no filesystem selected)");
        return true;
    }
    io.write(_cwd_fs);
    io.write(":");
    io.write_line(_cwd_path);
    return true;
}

bool FsShell::cmd_cd(IConsoleTransport& io, const std::vector<std::string_view>& argv)
{
    if (argv.size() < 2) {
        return cmd_pwd(io, argv);
    }

    FsPath target;
    if (!parse_fs_path(argv[1], _cwd_fs, _cwd_path, target)) {
        io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
        return true;
    }

    auto* fs = _storage.get(target.fs);
    if (!fs) {
        io.write_line("error: unknown filesystem");
        return true;
    }
    if (!fs->exists(target.path) || !fs->isDirectory(target.path)) {
        io.write_line("error: not a directory");
        return true;
    }

    _cwd_fs = target.fs;
    _cwd_path = target.path;
    return true;
}

bool FsShell::cmd_ls(IConsoleTransport& io, const std::vector<std::string_view>& argv)
{
    FsPath target;
    bool ok = false;
    fs_resolve_target(argv, _cwd_fs, _cwd_path, target, ok);
    if (!ok) {
        io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
        return true;
    }

    auto* fs = _storage.get(target.fs);
    if (!fs) {
        io.write_line("error: unknown filesystem");
        return true;
    }

    // Wildcard support: if the last path component contains '*' or '?', list the parent and filter.
    std::string filter_pat;
    if (has_wildcard(target.path)) {
        filter_pat = leaf_name(target.path);
        target.path = parent_path(target.path);
        if (filter_pat.empty()) {
            io.write_line("error: bad pattern");
            return true;
        }
    }

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
        if (!filter_pat.empty()) {
            const std::string leaf(leaf_name(e.path));
            if (!glob_match(filter_pat, leaf)) {
                continue;
            }
        }
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

bool FsShell::cmd_mkdir(IConsoleTransport& io, const std::vector<std::string_view>& argv)
{
    FsPath target;
    bool ok = false;
    fs_resolve_target(argv, _cwd_fs, _cwd_path, target, ok);
    if (!ok) {
        io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
        return true;
    }

    auto* fs = _storage.get(target.fs);
    if (!fs) {
        io.write_line("error: unknown filesystem");
        return true;
    }
    if (!fs->createDirectory(target.path)) {
        io.write_line("error: mkdir failed");
    }
    return true;
}

bool FsShell::cmd_rm(IConsoleTransport& io, const std::vector<std::string_view>& argv)
{
    if (_cwd_fs.empty()) {
        io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
        return true;
    }
    if (argv.size() < 2) {
        io.write_line("error: usage: rm [-f] [-r] <path...>");
        return true;
    }

    std::size_t idx = 1;
    RmFlags flags;
    parse_rm_flags(argv, idx, flags);
    if (idx >= argv.size()) {
        io.write_line("error: usage: rm [-f] [-r] <path...>");
        return true;
    }

    bool all_ok = true;
    for (; idx < argv.size(); ++idx) {
        FsPath target;
        if (!parse_fs_path(argv[idx], _cwd_fs, _cwd_path, target)) {
            if (!flags.force) io.write_line("error: bad path");
            all_ok = false;
            continue;
        }

        auto* fs = _storage.get(target.fs);
        if (!fs) {
            if (!flags.force) io.write_line("error: unknown filesystem");
            all_ok = false;
            continue;
        }

        // If wildcard, expand against parent dir.
        if (has_wildcard(target.path)) {
            const std::string pat = leaf_name(target.path);
            const std::string dir = parent_path(target.path);
            std::vector<fujinet::fs::FileInfo> entries;
            if (!fs->listDirectory(dir, entries)) {
                if (!flags.force) io.write_line("error: ls failed");
                all_ok = false;
                continue;
            }
            bool matched = false;
            for (const auto& e : entries) {
                const std::string leaf = leaf_name(e.path);
                if (!glob_match(pat, leaf)) continue;
                matched = true;
                if (!delete_tree(*fs, e.path, flags, false, io)) {
                    all_ok = false;
                }
            }
            if (!matched && !flags.force) {
                io.write_line("error: not found");
                all_ok = false;
            }
            continue;
        }

        if (!delete_tree(*fs, target.path, flags, false, io)) {
            all_ok = false;
        }
    }

    (void)all_ok;
    return true;
}

bool FsShell::cmd_rmdir(IConsoleTransport& io, const std::vector<std::string_view>& argv)
{
    if (argv.size() < 2) {
        io.write_line("error: usage: rmdir [-f] [-r] <path>");
        return true;
    }

    std::size_t idx = 1;
    RmFlags flags;
    parse_rm_flags(argv, idx, flags);
    if (idx >= argv.size()) {
        io.write_line("error: usage: rmdir [-f] [-r] <path>");
        return true;
    }

    // Only operate on the first path (like current shell behavior).
    FsPath target;
    if (!parse_fs_path(argv[idx], _cwd_fs, _cwd_path, target)) {
        io.write_line("error: bad path");
        return true;
    }

    auto* fs = _storage.get(target.fs);
    if (!fs) {
        io.write_line("error: unknown filesystem");
        return true;
    }

    // rmdir must not delete files.
    (void)delete_tree(*fs, target.path, flags, true, io);
    return true;
}

bool FsShell::cmd_mv(IConsoleTransport& io, const std::vector<std::string_view>& argv)
{
    if (argv.size() < 3) {
        io.write_line("error: usage: mv <from> <to>");
        return true;
    }

    FsPath from;
    FsPath to;
    if (!parse_fs_path(argv[1], _cwd_fs, _cwd_path, from) ||
        !parse_fs_path(argv[2], _cwd_fs, _cwd_path, to)) {
        io.write_line("error: no filesystem selected (try: fs, then cd <fs>:/)");
        return true;
    }
    if (from.fs != to.fs) {
        io.write_line("error: mv across filesystems is not supported");
        return true;
    }

    auto* fs = _storage.get(from.fs);
    if (!fs) {
        io.write_line("error: unknown filesystem");
        return true;
    }

    // If destination is an existing directory (or explicitly ends with '/'), append basename(from).
    fujinet::fs::FileInfo to_st{};
    std::string dst = to.path;
    const bool to_slash = (!argv[2].empty() && argv[2].back() == '/');
    const bool to_is_dir = (fs->stat(to.path, to_st) && to_st.isDirectory);
    if (to_slash && !to_is_dir) {
        io.write_line("error: destination ends with '/' but is not a directory");
        return true;
    }
    if (to_is_dir) {
        const std::string base = leaf_name(from.path);
        if (dst.size() > 1 && dst.back() == '/') dst.pop_back();
        dst += "/";
        dst += base;
    }

    if (!fs->rename(from.path, dst)) {
        io.write_line("error: mv failed");
    }
    return true;
}

} // namespace fujinet::console


