#include "doctest.h"

#include "fujinet/io/core/channel.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace fujinet::tnfs;
using fujinet::fs::DirSort;

namespace {

struct Ent {
    std::string name;
    bool dir;
    std::uint32_t size;
    std::uint32_t mtime;
};

// In-memory TNFS server behind a Channel: decodes the client's datagrams and queues replies.
class FakeTnfsChannel final : public fujinet::io::Channel {
public:
    struct Config {
        std::vector<Ent> entries;
        std::string dirPath = "/d";
        std::uint8_t verMajor = 1, verMinor = 2;
        bool supportX = true;      // OPENDIRX answers (else 0x16)
        bool supportSeek = true;
        std::size_t perReply = 5;  // READDIRX entries per datagram
        std::uint16_t session = 0x1234; // what MOUNT assigns; tnfsd's first session is 0
        bool silent = false;       // never answers
    };

    explicit FakeTnfsChannel(std::shared_ptr<Config> cfg, std::shared_ptr<std::map<int, int>> counts)
        : _cfg(std::move(cfg)), _counts(std::move(counts)) {}

    bool available() override { return !_replies.empty(); }

    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override
    {
        if (_replies.empty()) return 0;
        auto& r = _replies.front();
        const std::size_t n = std::min(maxLen, r.size());
        std::memcpy(buffer, r.data(), n);
        _replies.pop_front();
        return n;
    }

    void write(const std::uint8_t* b, std::size_t len) override
    {
        if (len < 4) return;
        const std::uint8_t seq = b[2];
        const std::uint8_t cmd = b[3];
        ++(*_counts)[cmd];
        if (_cfg->silent && cmd != CMD_MOUNT) return;
        const std::uint8_t* p = b + 4;
        const std::size_t plen = len - 4;
        std::vector<std::uint8_t> pl;
        switch (cmd) {
        case CMD_MOUNT:
            pl = {0, _cfg->verMinor, _cfg->verMajor, 0xE8, 0x03};
            send(seq, cmd, pl, _cfg->session);
            return;
        case CMD_OPENDIRX: {
            if (!_cfg->supportX) { send(seq, cmd, {RESULT_FUNCTION_UNIMPLEMENTED}); return; }
            const std::uint8_t dirOpt = p[0], sortOpt = p[1];
            const char* pat = reinterpret_cast<const char*>(p + 4);
            const char* path = pat + std::strlen(pat) + 1;
            if (std::string(path) != _cfg->dirPath) { send(seq, cmd, {RESULT_FILE_NOT_FOUND}); return; }
            _cur = _cfg->entries;
            if (!(sortOpt & DIRSORT_NONE)) {
                const bool dirsFirst = !(dirOpt & DIROPT_NO_FOLDERSFIRST);
                std::stable_sort(_cur.begin(), _cur.end(), [&](const Ent& a, const Ent& c) {
                    if (dirsFirst && a.dir != c.dir) return a.dir;
                    return lower(a.name) < lower(c.name);
                });
            }
            _pos = 0;
            send(seq, cmd, {0, 3, static_cast<std::uint8_t>(_cur.size() & 0xFF),
                            static_cast<std::uint8_t>(_cur.size() >> 8)});
            return;
        }
        case CMD_SEEKDIR:
            if (!_cfg->supportSeek) { send(seq, cmd, {RESULT_FUNCTION_UNIMPLEMENTED}); return; }
            _pos = p[1] | (p[2] << 8) | (p[3] << 16);
            send(seq, cmd, {0});
            return;
        case CMD_READDIRX: {
            const std::size_t maxE = p[1] == 0 ? 255 : p[1];
            if (_pos >= _cur.size()) { send(seq, cmd, {RESULT_END_OF_FILE}); return; }
            const std::size_t n = std::min({maxE, _cfg->perReply, _cur.size() - _pos});
            const bool eof = _pos + n >= _cur.size();
            pl = {0, static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(eof ? DIRSTATUS_EOF : 0),
                  static_cast<std::uint8_t>(_pos & 0xFF), static_cast<std::uint8_t>(_pos >> 8)};
            for (std::size_t i = 0; i < n; ++i) {
                const Ent& e = _cur[_pos + i];
                pl.push_back(e.dir ? DIRENTRY_DIR : 0);
                put32(pl, e.size); put32(pl, e.mtime); put32(pl, e.mtime);
                pl.insert(pl.end(), e.name.begin(), e.name.end());
                pl.push_back(0);
            }
            _pos += n;
            send(seq, cmd, pl);
            return;
        }
        case CMD_OPENDIR: {
            if (std::string(reinterpret_cast<const char*>(p)) != _cfg->dirPath) { send(seq, cmd, {RESULT_FILE_NOT_FOUND}); return; }
            _cur = _cfg->entries; _pos = 0;
            send(seq, cmd, {0, 4});
            return;
        }
        case CMD_READDIR: {
            if (_pos >= _cur.size()) { send(seq, cmd, {RESULT_END_OF_FILE}); return; }
            pl = {0};
            const std::string& n = _cur[_pos++].name;
            pl.insert(pl.end(), n.begin(), n.end());
            pl.push_back(0);
            send(seq, cmd, pl);
            return;
        }
        case CMD_STAT: {
            const std::string path(reinterpret_cast<const char*>(p), plen ? plen - 1 : 0);
            pl = {0};
            if (path == _cfg->dirPath) {
                pl.push_back(0); pl.push_back(0x40);
                pl.insert(pl.end(), 4 + 4 + 12, 0);
                send(seq, cmd, pl);
                return;
            }
            for (const auto& e : _cfg->entries) {
                if (_cfg->dirPath + "/" + e.name == path) {
                    pl.push_back(0); pl.push_back(e.dir ? 0x40 : 0x80);
                    pl.insert(pl.end(), 4, 0);
                    put32(pl, e.size); put32(pl, 0); put32(pl, e.mtime); put32(pl, 0);
                    send(seq, cmd, pl);
                    return;
                }
            }
            send(seq, cmd, {RESULT_FILE_NOT_FOUND});
            return;
        }
        default:
            send(seq, cmd, {0});
        }
    }

private:
    static std::string lower(std::string s)
    {
        for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }
    static void put32(std::vector<std::uint8_t>& v, std::uint32_t x)
    {
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
    }
    void send(std::uint8_t seq, std::uint8_t cmd, std::vector<std::uint8_t> payload, std::uint16_t sess = 0)
    {
        if (sess == 0) sess = _cfg->session;
        std::vector<std::uint8_t> r{static_cast<std::uint8_t>(sess & 0xFF), static_cast<std::uint8_t>(sess >> 8), seq, cmd};
        r.insert(r.end(), payload.begin(), payload.end());
        _replies.push_back(std::move(r));
    }

    std::shared_ptr<Config> _cfg;
    std::shared_ptr<std::map<int, int>> _counts;
    std::deque<std::vector<std::uint8_t>> _replies;
    std::vector<Ent> _cur;
    std::size_t _pos{0};
};

struct Rig {
    std::shared_ptr<FakeTnfsChannel::Config> cfg = std::make_shared<FakeTnfsChannel::Config>();
    std::shared_ptr<std::map<int, int>> counts = std::make_shared<std::map<int, int>>();
    std::unique_ptr<ITnfsClient> client;

    void make_entries(int files)
    {
        cfg->entries.push_back({"zdir", true, 0, 50});
        cfg->entries.push_back({"Adir", true, 0, 60});
        for (int i = 0; i < files; ++i) {
            char n[32];
            std::snprintf(n, sizeof(n), "%cfile%03d.atr", (i % 2) ? 'b' : 'C', i);
            cfg->entries.push_back({n, false, static_cast<std::uint32_t>(1000 + i), static_cast<std::uint32_t>(2000 + i)});
        }
    }
    void connect()
    {
        client = make_udp_tnfs_client(std::make_unique<FakeTnfsChannel>(cfg, counts));
        REQUIRE(client->mount("/"));
    }
    int n(int cmd) { return (*counts)[cmd]; }
};

} // namespace

TEST_CASE("TNFS session 0 is a real session: the first one tnfsd hands out")
{
    Rig r;
    r.cfg->session = 0;
    r.make_entries(3);
    r.connect();

    std::vector<TnfsDirEntry> out;
    std::size_t total = 0;
    CHECK(r.client->listDirectoryRange("/d", DirSort::NameDirsFirst, 0, 8, out, total) == TnfsListStatus::Ok);
    CHECK(total == 5);
    CHECK(out.size() == 5);
}

TEST_CASE("TNFS OPENDIRX/READDIRX range: sizes and times without stat")
{
    Rig r;
    r.make_entries(30); // 32 entries
    r.connect();

    std::vector<TnfsDirEntry> out;
    std::size_t total = 0;
    CHECK(r.client->listDirectoryRange("/d", DirSort::NameDirsFirst, 0, 8, out, total) == TnfsListStatus::Ok);
    CHECK(total == 32);
    REQUIRE(out.size() == 8);
    CHECK(out[0].name == "Adir");
    CHECK(out[0].isDir);
    CHECK(out[1].name == "zdir");
    CHECK(out[2].name == "bfile001.atr"); // case-insensitive: b < C
    CHECK(out[2].size == 1001);
    CHECK(out[2].mTime == 2001);
    CHECK(r.n(CMD_STAT) == 0);
    CHECK(r.n(CMD_OPENDIR) == 0);
    CHECK(r.n(CMD_CLOSEDIR) == 1);
    CHECK(r.n(CMD_READDIRX) == 2); // 5 entries per reply
}

TEST_CASE("TNFS range uses SEEKDIR to start in the middle, and reads forward without it")
{
    Rig r;
    r.make_entries(30);
    r.connect();
    std::vector<TnfsDirEntry> out;
    std::size_t total = 0;
    CHECK(r.client->listDirectoryRange("/d", DirSort::NameDirsFirst, 10, 4, out, total) == TnfsListStatus::Ok);
    REQUIRE(out.size() == 4);
    CHECK(r.n(CMD_SEEKDIR) == 1);
    CHECK(out[0].name == "bfile017.atr"); // 2 dirs, then bfile001, 003, ... (index 10 = 9th odd)
    CHECK(r.n(CMD_READDIRX) == 1);
    CHECK(out[3].name == "bfile023.atr");

    // A server without SEEKDIR: read forward and drop the leading entries.
    Rig f;
    f.make_entries(30);
    f.cfg->supportSeek = false;
    f.connect();
    CHECK(f.client->listDirectoryRange("/d", DirSort::NameDirsFirst, 10, 4, out, total) == TnfsListStatus::Ok);
    REQUIRE(out.size() == 4);
    CHECK(out[0].name == "bfile017.atr");
    CHECK(out[3].name == "bfile023.atr");
    CHECK(f.n(CMD_READDIRX) == 3);
}

TEST_CASE("TNFS range past the end returns nothing but the total; last page is short")
{
    Rig r;
    r.make_entries(30);
    r.connect();
    std::vector<TnfsDirEntry> out;
    std::size_t total = 0;
    CHECK(r.client->listDirectoryRange("/d", DirSort::Name, 100, 8, out, total) == TnfsListStatus::Ok);
    CHECK(total == 32);
    CHECK(out.empty());
    CHECK(r.client->listDirectoryRange("/d", DirSort::Name, 30, 8, out, total) == TnfsListStatus::Ok);
    CHECK(out.size() == 2);
    CHECK(r.client->listDirectoryRange("/missing", DirSort::Name, 0, 8, out, total) == TnfsListStatus::Error);
}

TEST_CASE("TNFS 1.0 server: fallback lists names and stats only the requested range")
{
    Rig r;
    r.make_entries(30);
    r.cfg->verMajor = 1;
    r.cfg->verMinor = 0;
    r.connect();

    std::vector<TnfsDirEntry> out;
    std::size_t total = 0;
    CHECK(r.client->listDirectoryRange("/d", DirSort::Name, 4, 6, out, total) == TnfsListStatus::Ok);
    CHECK(total == 32);
    REQUIRE(out.size() == 6);
    CHECK(r.n(CMD_OPENDIRX) == 0); // version below 1.1: not even tried
    CHECK(r.n(CMD_OPENDIR) == 1);
    CHECK(r.n(CMD_READDIR) == 33); // names are read for the whole directory
    CHECK(r.n(CMD_STAT) == 6);     // but only the range is stat'ed
    // Name order (no directories-first in the fallback): Adir, bfile001, ..., cfile000 ...
    CHECK(out[0].name == "bfile007.atr");
    CHECK(out[0].size == 1007);
    CHECK(out[0].mTime == 2007);
}

TEST_CASE("TNFS server that rejects OPENDIRX: fallback, remembered for later calls")
{
    Rig r;
    r.make_entries(30);
    r.cfg->supportX = false;
    r.connect();

    std::vector<TnfsDirEntry> out;
    std::size_t total = 0;
    CHECK(r.client->listDirectoryRange("/d", DirSort::Name, 0, 5, out, total) == TnfsListStatus::Ok);
    CHECK(out.size() == 5);
    CHECK(r.n(CMD_OPENDIRX) == 1);
    CHECK(r.n(CMD_STAT) == 5);
    CHECK(r.client->listDirectoryRange("/d", DirSort::Name, 5, 5, out, total) == TnfsListStatus::Ok);
    CHECK(r.n(CMD_OPENDIRX) == 1); // not probed again
    CHECK(r.n(CMD_STAT) == 10);
}

TEST_CASE("TNFS listing deadline: a server that never answers fails with Timeout in bounded time")
{
    Rig r;
    r.make_entries(30);
    r.connect();
    r.cfg->silent = true;

    std::vector<TnfsDirEntry> out;
    std::size_t total = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const auto st = r.client->listDirectoryRange("/d", DirSort::Name, 0, 5, out, total,
                                                 std::chrono::milliseconds(300));
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(st == TnfsListStatus::Timeout);
    CHECK(out.empty());
    CHECK(ms >= 250);
    CHECK(ms < 1400); // far below one 3 x 1.5 s retry cycle
}

TEST_CASE("TNFS listing deadline default is ten seconds")
{
    CHECK(kListingDeadline == std::chrono::milliseconds(10000));
}
