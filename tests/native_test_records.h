#pragma once

#include "directory_packet_io.h"

#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/io/protocol/fuji_bus_packet.h"
#include "fujinet/io/protocol/wire_device_ids.h"
#include "fujinet/platform/posix/fs_factory.h"

#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace native_test_records {

using fujinet::core::FujinetCore;
using fujinet::io::PacketIOStatus;
using fujinet::io::PacketReceiveResult;
using fujinet::io::protocol::ByteBuffer;
using fujinet::io::protocol::FujiBusPacket;
using fujinet::io::protocol::WireDeviceId;
using fujinet::native_test::DirectoryPacketChannel;
using fujinet::native_test::DirectoryPacketIO;
using fujinet::native_test::DirectoryPacketRole;
using fujinet::native_test::kDirectoryPacketIdentityName;
using fujinet::native_test::kDirectoryPacketToGuestName;
using fujinet::native_test::kDirectoryPacketToHostName;
using fujinet::native_test::kNativeTestIdentityToken;
using fujinet::native_test::write_native_test_identity;

inline std::filesystem::path make_temp_dir()
{
    std::string templ = "/tmp/fujinet-nio-native-test-XXXXXX";
    char* path = ::mkdtemp(templ.data());
    if (!path) {
        return {};
    }
    return path;
}

inline bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    std::size_t put = 0;
    while (put < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + put, bytes.size() - put);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        put += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return true;
}

inline std::vector<std::uint8_t> make_raw_request(WireDeviceId device,
                                                  std::uint8_t command,
                                                  const std::vector<std::uint8_t>& payload)
{
    FujiBusPacket pkt(device, command);
    if (!payload.empty()) {
        pkt.setData(ByteBuffer(payload.begin(), payload.end()));
    }
    const auto raw = pkt.serializeRaw();
    return {raw.begin(), raw.end()};
}

class NativeTestClient {
public:
    explicit NativeTestClient(std::string directory, std::size_t capacity = 65535)
        : _io(std::move(directory), capacity, DirectoryPacketRole::Client)
    {}

    PacketIOStatus send_raw(const std::vector<std::uint8_t>& raw)
    {
        return _io.send(raw.data(), raw.size());
    }

    PacketReceiveResult receive_raw(std::vector<std::uint8_t>& out)
    {
        out.assign(_io.capacity(), 0);
        const auto result = _io.receive(out.data(), out.size());
        if (result.status == PacketIOStatus::Ok) {
            out.resize(result.size);
        } else {
            out.clear();
        }
        return result;
    }

    std::optional<std::vector<std::uint8_t>>
    wait_record(std::chrono::steady_clock::time_point deadline)
    {
        std::vector<std::uint8_t> out;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = receive_raw(out);
            if (result.status == PacketIOStatus::Ok) {
                return out;
            }
            if (result.status != PacketIOStatus::NoData) {
                return std::nullopt;
            }
            ::usleep(2000);
        }
        return std::nullopt;
    }

    DirectoryPacketIO& io() { return _io; }

private:
    DirectoryPacketIO _io;
};

class NativeTestHostStack {
public:
    NativeTestHostStack(std::string directory, std::size_t capacity)
        : _directory(std::move(directory))
        , _packets(_directory, capacity, DirectoryPacketRole::Host)
        , _channel(&_packets)
    {
        const auto hostRoot = std::filesystem::path(_directory) / "host-fs";
        std::error_code ec;
        std::filesystem::create_directories(hostRoot, ec);
        auto hostFs = fujinet::platform::posix::create_host_filesystem(hostRoot.string());
        _registered_fs = hostFs && _core.storageManager().registerFileSystem(std::move(hostFs));
        fujinet::core::register_file_device(_core);
        fujinet::core::register_clock_device(_core);

        // Local FujiBusNative profile; never current_build_profile() or channel factory.
        const fujinet::build::BuildProfile profile{
            .machine = fujinet::build::Machine::Generic,
            .primaryTransport = fujinet::build::TransportKind::FujiBusNative,
            .primaryChannel = fujinet::build::ChannelKind::Pty,
            .name = kNativeTestIdentityToken,
            .hw = {},
        };
        _transport = fujinet::core::setup_transports(_core, _channel, profile, nullptr);
    }

    bool ready() const { return _registered_fs && _transport != nullptr; }

    void tick() { _core.tick(); }

    DirectoryPacketChannel& channel() { return _channel; }
    DirectoryPacketIO& packets() { return _packets; }

    std::optional<std::vector<std::uint8_t>>
    exchange(NativeTestClient& client,
             const std::vector<std::uint8_t>& request,
             std::chrono::milliseconds timeout)
    {
        if (client.send_raw(request) != PacketIOStatus::Ok) {
            return std::nullopt;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            tick();
            std::vector<std::uint8_t> out;
            const auto once = client.receive_raw(out);
            if (once.status == PacketIOStatus::Ok) {
                return out;
            }
            if (once.status != PacketIOStatus::NoData) {
                return std::nullopt;
            }
            ::usleep(1000);
        }
        return std::nullopt;
    }

private:
    std::string _directory;
    bool _registered_fs{false};
    DirectoryPacketIO _packets;
    DirectoryPacketChannel _channel;
    FujinetCore _core;
    fujinet::io::ITransport* _transport{nullptr};
};

class NativeTestRunnerProcess {
public:
    NativeTestRunnerProcess() = default;
    NativeTestRunnerProcess(const NativeTestRunnerProcess&) = delete;
    NativeTestRunnerProcess& operator=(const NativeTestRunnerProcess&) = delete;

    ~NativeTestRunnerProcess() { terminate(); }

    bool spawn(const char* runner_path,
               const std::filesystem::path& directory,
               const std::vector<std::string>& extra_env = {})
    {
        terminate();
        int fds[2];
        if (::pipe(fds) != 0) {
            return false;
        }
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
        if (pid == 0) {
            ::close(fds[0]);
            ::dup2(fds[1], STDOUT_FILENO);
            ::dup2(fds[1], STDERR_FILENO);
            ::close(fds[1]);
            for (const auto& entry : extra_env) {
                ::putenv(const_cast<char*>(entry.c_str()));
            }
            const char* args[] = {runner_path, "--dir", directory.c_str(), nullptr};
            ::execv(runner_path, const_cast<char**>(args));
            ::_exit(127);
        }
        ::close(fds[1]);
        _pid = pid;
        _stdout = fds[0];
        const int flags = ::fcntl(_stdout, F_GETFL, 0);
        if (flags < 0 || ::fcntl(_stdout, F_SETFL, flags | O_NONBLOCK) < 0) {
            terminate();
            return false;
        }
        return true;
    }

    bool wait_for_identity_file(const std::filesystem::path& directory,
                                std::chrono::milliseconds timeout)
    {
        const auto path = directory / kDirectoryPacketIdentityName;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (std::filesystem::exists(path)) {
                return true;
            }
            if (!running()) {
                return false;
            }
            drain_stdout();
            ::usleep(2000);
        }
        return std::filesystem::exists(path);
    }

    bool running() const
    {
        if (_pid <= 0) {
            return false;
        }
        const int result = ::kill(_pid, 0);
        return result == 0;
    }

    void drain_stdout()
    {
        if (_stdout < 0) {
            return;
        }
        char buf[256];
        while (true) {
            const ssize_t n = ::read(_stdout, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            _output.append(buf, static_cast<std::size_t>(n));
        }
    }

    const std::string& output() const { return _output; }

    void terminate()
    {
        if (_pid > 0) {
            ::kill(_pid, SIGTERM);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
            int status = 0;
            while (std::chrono::steady_clock::now() < deadline) {
                const pid_t r = ::waitpid(_pid, &status, WNOHANG);
                if (r == _pid) {
                    _pid = -1;
                    break;
                }
                ::usleep(5000);
            }
            if (_pid > 0) {
                ::kill(_pid, SIGKILL);
                ::waitpid(_pid, &status, 0);
                _pid = -1;
            }
        }
        drain_stdout();
        if (_stdout >= 0) {
            ::close(_stdout);
            _stdout = -1;
        }
    }

    pid_t pid() const { return _pid; }

private:
    pid_t _pid{-1};
    int _stdout{-1};
    std::string _output;
};

inline std::string capture_runner_help(const char* runner_path)
{
    int fds[2];
    if (::pipe(fds) != 0) {
        return {};
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return {};
    }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);
        const char* args[] = {runner_path, "--help", nullptr};
        ::execv(runner_path, const_cast<char**>(args));
        ::_exit(127);
    }
    ::close(fds[1]);
    std::string out;
    char buf[256];
    while (true) {
        const ssize_t n = ::read(fds[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return out;
}

inline std::string read_identity_file(const std::filesystem::path& directory)
{
    const auto path = directory / kDirectoryPacketIdentityName;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return {};
    }
    std::string body;
    char buf[64];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        body.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return body;
}

} // namespace native_test_records
