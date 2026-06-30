#include "fujinet/diag/diagnostic_provider.h"

#include "fujinet/core/core.h"
#include "fujinet/diag/diagnostic_parse.h"
#include "fujinet/io/devices/app_store.h"

#include <cctype>
#include <cstdint>
#include <string>

namespace fujinet::diag {

namespace {

std::string to_string(std::string_view sv)
{
    return std::string(sv.data(), sv.size());
}

std::string join_tail(const DiagArgsView& args, std::size_t first)
{
    std::string out;
    for (std::size_t i = first; i < args.argv.size(); ++i) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(args.argv[i].data(), args.argv[i].size());
    }
    return out;
}

bool is_text(const std::vector<std::uint8_t>& data)
{
    for (std::uint8_t b : data) {
        if (b == '\r' || b == '\n' || b == '\t') {
            continue;
        }
        if (b < 0x20 || b == 0x7F) {
            return false;
        }
    }
    return true;
}

std::string hex_preview(const std::vector<std::uint8_t>& data)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(data.size() * 3);
    for (std::size_t i = 0; i < data.size(); ++i) {
        if (i != 0) out.push_back(' ');
        const auto b = data[i];
        out.push_back(hex[(b >> 4) & 0x0F]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

class AppStoreDiagnosticProvider final : public IDiagnosticProvider {
public:
    explicit AppStoreDiagnosticProvider(fujinet::core::FujinetCore& core)
        : _core(core)
    {}

    std::string_view provider_id() const noexcept override { return "appstore"; }

    void list_commands(std::vector<DiagCommandSpec>& out) const override
    {
        out.push_back(DiagCommandSpec{
            .name = "appstore.stat",
            .summary = "inspect an application storage key",
            .usage = "appstore.stat <namespace> <key>",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "appstore.list",
            .summary = "list keys in an application storage namespace",
            .usage = "appstore.list <namespace> [start] [max-bytes]",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "appstore.read",
            .summary = "read an application storage key",
            .usage = "appstore.read <namespace> <key>",
            .safe = true,
        });
        out.push_back(DiagCommandSpec{
            .name = "appstore.write",
            .summary = "write text to an application storage key",
            .usage = "appstore.write <namespace> <key> <text>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "appstore.delete",
            .summary = "delete an application storage key",
            .usage = "appstore.delete <namespace> <key>",
            .safe = false,
        });
        out.push_back(DiagCommandSpec{
            .name = "appstore.rename",
            .summary = "rename an application storage key",
            .usage = "appstore.rename <namespace> <old-key> <new-key>",
            .safe = false,
        });
    }

    DiagResult execute(const DiagArgsView& args) override
    {
        if (args.argv.empty()) {
            return DiagResult::invalid_args("missing command");
        }

        const std::string_view cmd = args.argv[0];
        if (cmd == "appstore.stat") return cmd_stat(args);
        if (cmd == "appstore.list") return cmd_list(args);
        if (cmd == "appstore.read") return cmd_read(args);
        if (cmd == "appstore.write") return cmd_write(args);
        if (cmd == "appstore.delete") return cmd_delete(args);
        if (cmd == "appstore.rename") return cmd_rename(args);

        return DiagResult::not_found("unknown appstore command");
    }

private:
    fujinet::io::AppStore store() { return fujinet::io::AppStore(_core.storageManager()); }

    DiagResult require_available(fujinet::io::AppStore& s)
    {
        if (!s.available()) {
            return DiagResult::not_ready("application storage backing filesystem is not available\r\n");
        }
        return DiagResult::ok();
    }

    DiagResult cmd_stat(const DiagArgsView& args)
    {
        if (args.argv.size() != 3) {
            return DiagResult::invalid_args("usage: appstore.stat <namespace> <key>\r\n");
        }

        auto s = store();
        auto ready = require_available(s);
        if (ready.status != DiagStatus::Ok) return ready;

        fujinet::io::AppStore::Stat st{};
        if (!s.stat(args.argv[1], args.argv[2], st)) {
            return DiagResult::error("failed to stat key\r\n");
        }

        DiagResult r = DiagResult::ok();
        r.kv.emplace_back("exists", st.exists ? "true" : "false");
        r.kv.emplace_back("size_bytes", std::to_string(st.sizeBytes));
        r.kv.emplace_back("modified_unix_time", std::to_string(st.modifiedUnixTime));
        r.text = "exists: ";
        r.text += st.exists ? "true" : "false";
        r.text += "\r\nsize_bytes: ";
        r.text += std::to_string(st.sizeBytes);
        r.text += "\r\nmodified_unix_time: ";
        r.text += std::to_string(st.modifiedUnixTime);
        r.text += "\r\n";
        return r;
    }

    DiagResult cmd_list(const DiagArgsView& args)
    {
        if (args.argv.size() < 2 || args.argv.size() > 4) {
            return DiagResult::invalid_args("usage: appstore.list <namespace> [start] [max-bytes]\r\n");
        }

        std::uint32_t start = 0;
        std::uint32_t max_bytes = 2048;
        if (args.argv.size() >= 3 && !parse_decimal_u32(args.argv[2], start)) {
            return DiagResult::invalid_args("start must be a decimal number\r\n");
        }
        if (args.argv.size() >= 4 && !parse_decimal_u32(args.argv[3], max_bytes)) {
            return DiagResult::invalid_args("max-bytes must be a decimal number\r\n");
        }
        if (start > 0xFFFF || max_bytes == 0 || max_bytes > 0xFFFF) {
            return DiagResult::invalid_args("start and max-bytes must fit in uint16; max-bytes must be non-zero\r\n");
        }

        auto s = store();
        auto ready = require_available(s);
        if (ready.status != DiagStatus::Ok) return ready;

        fujinet::io::AppStore::ListResult list{};
        if (!s.list(args.argv[1], static_cast<std::uint16_t>(start), static_cast<std::uint16_t>(max_bytes), list)) {
            return DiagResult::error("failed to list namespace\r\n");
        }

        DiagResult r = DiagResult::ok();
        r.kv.emplace_back("more", list.more ? "true" : "false");
        r.kv.emplace_back("count", std::to_string(list.keys.size()));
        for (const auto& key : list.keys) {
            r.text += key;
            r.text += "\r\n";
        }
        if (list.more) {
            r.text += "[more]\r\n";
        }
        return r;
    }

    DiagResult cmd_read(const DiagArgsView& args)
    {
        if (args.argv.size() != 3) {
            return DiagResult::invalid_args("usage: appstore.read <namespace> <key>\r\n");
        }

        auto s = store();
        auto ready = require_available(s);
        if (ready.status != DiagStatus::Ok) return ready;

        fujinet::io::AppStore::Stat st{};
        if (!s.stat(args.argv[1], args.argv[2], st)) {
            return DiagResult::error("failed to stat key\r\n");
        }
        if (!st.exists) {
            return DiagResult::not_found("key not found\r\n");
        }
        if (st.sizeBytes > 0xFFFF) {
            return DiagResult::invalid_args("diagnostic read is limited to 65535 bytes; use protocol tooling for larger values\r\n");
        }

        fujinet::io::AppStore::ReadResult read{};
        if (!s.read(args.argv[1], args.argv[2], 0, static_cast<std::uint16_t>(st.sizeBytes == 0 ? 1 : st.sizeBytes), read)) {
            return DiagResult::error("failed to read key\r\n");
        }
        read.data.resize(static_cast<std::size_t>(st.sizeBytes));

        DiagResult r = DiagResult::ok();
        r.kv.emplace_back("exists", "true");
        r.kv.emplace_back("size_bytes", std::to_string(read.data.size()));
        if (is_text(read.data)) {
            r.text.assign(reinterpret_cast<const char*>(read.data.data()), read.data.size());
            if (r.text.empty() || r.text.back() != '\n') {
                r.text += "\r\n";
            }
        } else {
            r.text = hex_preview(read.data);
            r.text += "\r\n";
        }
        return r;
    }

    DiagResult cmd_write(const DiagArgsView& args)
    {
        if (args.argv.size() < 4) {
            return DiagResult::invalid_args("usage: appstore.write <namespace> <key> <text>\r\n");
        }

        const std::string value = join_tail(args, 3);
        if (value.size() > 0xFFFF) {
            return DiagResult::invalid_args("diagnostic write is limited to 65535 bytes; use protocol tooling for larger values\r\n");
        }

        auto s = store();
        auto ready = require_available(s);
        if (ready.status != DiagStatus::Ok) return ready;

        fujinet::io::AppStore::WriteResult written{};
        if (!s.write(args.argv[1], args.argv[2], 0, reinterpret_cast<const std::uint8_t*>(value.data()),
                     static_cast<std::uint16_t>(value.size()), written)) {
            return DiagResult::error("failed to write key\r\n");
        }

        DiagResult r = DiagResult::ok();
        r.kv.emplace_back("written", std::to_string(written.written));
        r.text = "written: ";
        r.text += std::to_string(written.written);
        r.text += "\r\n";
        return r;
    }

    DiagResult cmd_delete(const DiagArgsView& args)
    {
        if (args.argv.size() != 3) {
            return DiagResult::invalid_args("usage: appstore.delete <namespace> <key>\r\n");
        }

        auto s = store();
        auto ready = require_available(s);
        if (ready.status != DiagStatus::Ok) return ready;

        fujinet::io::AppStore::DeleteResult removed{};
        if (!s.remove(args.argv[1], args.argv[2], removed)) {
            return DiagResult::error("failed to delete key\r\n");
        }

        DiagResult r = DiagResult::ok();
        r.kv.emplace_back("deleted", removed.deleted ? "true" : "false");
        r.text = "deleted: ";
        r.text += removed.deleted ? "true" : "false";
        r.text += "\r\n";
        return r;
    }

    DiagResult cmd_rename(const DiagArgsView& args)
    {
        if (args.argv.size() != 4) {
            return DiagResult::invalid_args("usage: appstore.rename <namespace> <old-key> <new-key>\r\n");
        }

        auto s = store();
        auto ready = require_available(s);
        if (ready.status != DiagStatus::Ok) return ready;

        if (!s.rename(args.argv[1], args.argv[2], args.argv[3])) {
            return DiagResult::error("failed to rename key\r\n");
        }
        return DiagResult::ok("renamed\r\n");
    }

    fujinet::core::FujinetCore& _core;
};

} // namespace

std::unique_ptr<IDiagnosticProvider> create_app_store_diagnostic_provider(fujinet::core::FujinetCore& core)
{
    return std::make_unique<AppStoreDiagnosticProvider>(core);
}

} // namespace fujinet::diag
