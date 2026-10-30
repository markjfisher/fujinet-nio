// HotSync builds only (FN_WITH_HOTSYNC): the ESP32 build compiles every
// source, so elsewhere this file compiles to nothing.
#if FN_WITH_HOTSYNC == 1

#include "fujinet/hotsync/hotsync_service.h"

#include "fujinet/core/logging.h"
#include "fujinet/hotsync/cradle_channel.h"
#include "fujinet/fs/filesystem.h"
#include "fujinet/hotsync/fs_storage.h"
#include "fujinet/hotsync/http_fetch.h"
#include "fujinet/hotsync/netsync_transport.h"
#include "fujinet/hotsync/padp_transport.h"
#include "fujinet/hotsync/sha256.h"
#include "fujinet/hotsync/tcp_link.h"
#include "fujinet/net/tcp_socket_ops.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <utility>

namespace fujinet::hotsync {

namespace {

constexpr const char* TAG = "hotsync";

// Until the network is up a bind fails; try again this often (docs/hotsync.md,
// Design, says why this polls rather than waiting for a SystemEvents event).
constexpr std::uint64_t kListenRetryMs = 5000;
constexpr auto kIdlePoll = std::chrono::milliseconds(100);

std::uint32_t new_user_id()
{
    std::random_device rd;
    std::uint32_t id = 0;
    while (id == 0) {
        id = rd();
    }
    return id;
}

} // namespace

bool parse_backup_policy(const std::string& text, BackupPolicy& out)
{
    if (text == "none") {
        out = BackupPolicy::None;
    } else if (text == "flagged") {
        out = BackupPolicy::Flagged;
    } else if (text == "all") {
        out = BackupPolicy::All;
    } else {
        return false;
    }
    return true;
}

const char* backup_policy_name(BackupPolicy policy)
{
    switch (policy) {
    case BackupPolicy::None: return "none";
    case BackupPolicy::Flagged: return "flagged";
    case BackupPolicy::All: return "all";
    }
    return "flagged";
}

HotSyncService::HotSyncService(net::ITcpSocketOps& ops, fs::IFileSystem& fs, HotSyncSettings settings,
                               Clock clock, CradleChannel* cradle, CradleWindow window)
    : _ops(ops), _fs(fs), _settings(std::move(settings)), _clock(std::move(clock)), _cradle(cradle),
      _window(window)
{
    _status.port = _settings.port;
    _status.cradle = _cradle ? "idle" : "off";
    FsStorage(_fs, _settings.root).create_install_folder();
}

HotSyncService::~HotSyncService()
{
    if (_listenFd >= 0) {
        _ops.close(_listenFd);
    }
}

bool HotSyncService::start_listening()
{
    const std::string port = std::to_string(_settings.port);
    net::AddrInfo* res = nullptr;
    if (_ops.getaddrinfo(nullptr, port.c_str(), _ops.tcp_stream_passive_addrinfo_hints(), &res) != 0 || !res) {
        if (res) {
            _ops.freeaddrinfo(res);
        }
        return false;
    }

    int fd = -1;
    for (auto* ai = res; ai; ai = _ops.addrinfo_next(ai)) {
        fd = _ops.socket(_ops.addrinfo_family(ai), _ops.addrinfo_socktype(ai), _ops.addrinfo_protocol(ai));
        if (fd < 0) {
            continue;
        }
        _ops.apply_listen_socket_options(fd);
        net::SockLen addrlen = 0;
        const struct sockaddr* addr = _ops.addrinfo_addr(ai, &addrlen);
        if (_ops.bind(fd, addr, addrlen) == 0 && _ops.listen(fd, 1) == 0) {
            (void)_ops.set_nonblocking(fd);
            break;
        }
        _ops.close(fd);
        fd = -1;
    }
    _ops.freeaddrinfo(res);

    if (fd < 0) {
        return false;
    }
    _listenFd = fd;
    FN_LOGI(TAG, "NetSync listening on TCP %u", static_cast<unsigned>(_settings.port));
    std::lock_guard<std::mutex> lock(_statusMutex);
    _status.listening = true;
    return true;
}

bool HotSyncService::poll()
{
    bool worked = poll_network();
    if (_cradle) {
        worked = poll_cradle() || worked;
    }
    return worked;
}

bool HotSyncService::poll_network()
{
    if (_settings.port == 0) {
        return false;
    }
    if (_listenFd < 0) {
        const std::uint64_t now = _ops.now_ms();
        if (now < _nextListenAttemptMs) {
            return false;
        }
        if (!start_listening()) {
            _nextListenAttemptMs = now + kListenRetryMs;
            return false;
        }
    }

    const int fd = _ops.accept(_listenFd, nullptr, nullptr);
    if (fd < 0) {
        return false;
    }
    (void)_ops.set_nonblocking(fd);
    _ops.apply_stream_socket_options(fd, /*nodelay=*/true, /*keepalive=*/true);
    serve(fd);
    return true;
}

bool HotSyncService::poll_cradle()
{
    if (_ops.now_ms() < _nextWindowMs) {
        return false;
    }
    if (!_cradle->claim(kCmpInitialBaud)) {
        set_cradle_state("app");
        _nextWindowMs = _ops.now_ms() + _window.gapMs;
        return false;
    }
    set_cradle_state("listening");
    {
        std::lock_guard<std::mutex> lock(_statusMutex);
        ++_status.cradleWindows;
    }

    // The window bounds only the wait for WAKEUP; once a Palm has woken, the
    // line stays lent for the whole sync, rate change included.
    PadpTransport transport(_cradle->link());
    if (transport.wait_for_wakeup(_window.listenMs)) {
        FN_LOGI(TAG, "cradle: Palm WAKEUP");
        set_cradle_state("syncing");
        if (transport.answer_wakeup()) {
            run_session(transport, "cradle");
        } else {
            FN_LOGW(TAG, "cradle: Palm did not accept the rate");
            std::lock_guard<std::mutex> lock(_statusMutex);
            ++_status.rejected;
        }
    }
    _cradle->release();
    set_cradle_state("idle");
    _nextWindowMs = _ops.now_ms() + _window.gapMs;
    return true;
}

void HotSyncService::set_cradle_state(const char* state)
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    _status.cradle = state;
}

void HotSyncService::run()
{
    while (!_stop.load()) {
        if (!poll()) {
            std::this_thread::sleep_for(kIdlePoll);
            continue;
        }
        if (_stackProbe) {
            const std::size_t free = _stackProbe();
            std::lock_guard<std::mutex> lock(_statusMutex);
            _status.taskStackMinFree = free;
        }
    }
}

void HotSyncService::serve(int fd)
{
    TcpLink link(_ops, fd);
    NetSyncTransport transport(link);
    // A port probe or a stray client is not a Palm; count it apart from syncs.
    if (!transport.accept()) {
        FN_LOGW(TAG, "NetSync handshake failed; not a Palm");
        std::lock_guard<std::mutex> lock(_statusMutex);
        ++_status.rejected;
        return;
    }
    FN_LOGI(TAG, "NetSync connection");
    run_session(transport, "network");
}

void HotSyncService::run_session(DlpTransport& transport, const char* source)
{
    {
        std::lock_guard<std::mutex> lock(_statusMutex);
        _status.syncing = true;
        ++_status.syncs;
    }

    FsStorage storage(_fs, _settings.root);
    SessionOptions options;
    options.defaultUserName = _settings.userName;
    options.newUserId = new_user_id();
    options.backup = _settings.backup;
    options.maxDatabaseBytes = _settings.maxDatabaseBytes;
    if (_clock) {
        options.now = _clock();
    }
    const SessionReport report = Session(transport, storage, options).run();

    std::lock_guard<std::mutex> lock(_statusMutex);
    _status.syncing = false;
    if (report.error != DlpError::None) {
        ++_status.failures;
    }
    _status.lastSource = source;
    _status.lastTime = now_text();
    _status.lastUser = report.userName;
    _status.lastResult = report.error == DlpError::None ? "ok" : dlp_error_name(report.error);
    _status.lastInstalled = report.installed;
    _status.lastInstallFailures = report.installFailures;
    _status.lastBackedUp = report.backedUp;
    _status.lastBackupFailures = report.backupFailures;
}

std::string HotSyncService::now_text() const
{
    if (!_clock) {
        return {};
    }
    const DlpDateTime t = _clock();
    if (t.year == 0) {
        return {};
    }
    char text[40]; // room for any u8/u16 values, not just valid dates
    std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u", t.year, t.month, t.day, t.hour,
                  t.minute, t.second);
    return text;
}

namespace {

// A Palm database file name that is safe on FAT: letters, digits, '.', '-',
// '_' and space, ending in an extension the install step takes.
bool installable_name(const std::string& name)
{
    if (name.empty() || name.size() > 64 || name[0] == '.') {
        return false;
    }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '-' || c == '_' || c == ' ';
        if (!ok) {
            return false;
        }
    }
    const std::size_t dot = name.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext == "prc" || ext == "pdb" || ext == "pqa";
}

} // namespace

std::string HotSyncService::fetch_install(const std::string& url, std::string& name, std::size_t& size,
                                          const std::string& sha256)
{
    HttpUrl parsed;
    if (!parse_http_url(url, parsed)) {
        return "not an http:// URL";
    }
    if (name.empty()) {
        std::string path = parsed.path.substr(0, parsed.path.find_first_of("?#"));
        name = path.substr(path.rfind('/') + 1);
    }
    if (!installable_name(name)) {
        return "'" + name + "' is not a .prc, .pdb or .pqa name";
    }
    Bytes data;
    const std::string err = http_fetch(_ops, parsed, _settings.maxDatabaseBytes, data);
    if (!err.empty()) {
        return err;
    }
    if (data.empty()) {
        return "the file is empty";
    }
    if (!sha256.empty()) {
        std::string expected = sha256;
        for (char& c : expected) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        const std::string actual = sha256_hex(data);
        if (actual != expected) {
            return "SHA-256 is " + actual + ", not the one given; nothing queued";
        }
    }
    if (!FsStorage(_fs, _settings.root).queue_install(name, data)) {
        return "cannot write to " + _settings.root + "/install";
    }
    size = data.size();
    FN_LOGI(TAG, "queued %s (%u bytes) from %s", name.c_str(), static_cast<unsigned>(size), url.c_str());
    return {};
}

HotSyncStatus HotSyncService::status() const
{
    std::lock_guard<std::mutex> lock(_statusMutex);
    return _status;
}

} // namespace fujinet::hotsync

#endif // FN_WITH_HOTSYNC
