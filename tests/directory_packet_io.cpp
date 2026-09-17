#include "directory_packet_io.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <iostream>
#include <sys/file.h>
#include <sys/random.h>

namespace fujinet::native_test {
using fujinet::io::PacketIOStatus;
using fujinet::io::PacketReceiveResult;

namespace {

bool directory_usable(const std::string& directory)
{
    struct stat st {};
    if (::stat(directory.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        return false;
    }
    return ::access(directory.c_str(), R_OK | W_OK | X_OK) == 0;
}

std::string join_file(const std::string& directory, const char* name)
{
    if (directory.empty()) {
        return name;
    }
    if (directory.back() == '/') {
        return directory + name;
    }
    return directory + "/" + name;
}

PacketIOStatus unlink_if_present(const std::string& path)
{
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) {
        return PacketIOStatus::Ok;
    }
    return PacketIOStatus::ResetFailed;
}

ssize_t read_retry(int fd, void* buffer, std::size_t count)
{
    while (true) {
        const ssize_t n = ::read(fd, buffer, count);
        if (n >= 0 || errno != EINTR) {
            return n;
        }
    }
}

ssize_t write_retry(int fd, const void* buffer, std::size_t count)
{
    while (true) {
        const ssize_t n = ::write(fd, buffer, count);
        if (n >= 0 || errno != EINTR) {
            return n;
        }
    }
}


std::string read_control(const std::string& path)
{
    // Controls are tiny regular files. Never block on a malformed FIFO or
    // follow a replaced symlink while the peer is proving bounded recovery.
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return {};
    struct stat st {};
    char value[34]; // largest valid token is 33 bytes; one excess byte rejects
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return {};
    }
    const auto count = read_retry(fd, value, sizeof(value));
    ::close(fd);
    return count > 0 ? std::string(value, static_cast<std::size_t>(count)) : std::string{};
}

bool publish_control(const std::string& path, const std::string& value)
{
    const std::string tmp = path + ".tmp";
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK | O_NOFOLLOW, 0644);
    if (fd < 0) return false;
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || ::ftruncate(fd, 0) != 0) {
        ::close(fd);
        return false;
    }
    const auto n = write_retry(fd, value.data(), value.size());
    const bool ok = n == static_cast<ssize_t>(value.size()) && ::fsync(fd) == 0;
    ::close(fd);
    return ok && ::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string fresh_challenge()
{
    unsigned char random[16];
    if (::getrandom(random, sizeof(random), 0) != sizeof(random)) return {};
    static const char hex[] = "0123456789abcdef";
    std::string value;
    for (const auto byte : random) {
        value += hex[byte >> 4];
        value += hex[byte & 15];
    }
    return value + "\n";
}

} // namespace

DirectoryPacketIO::DirectoryPacketIO(std::string directory,
                                     std::size_t capacity,
                                     DirectoryPacketRole role, bool managed_peer)
    : IPacketIO(capacity > kDirectoryPacketMaxCapacity ? kDirectoryPacketMaxCapacity
                                                       : capacity)
    , _directory(std::move(directory))
    , _role(role)
{
    if (!managed_peer && discard_records() != PacketIOStatus::Ok) {
        _reset_required = true;
    }
}

DirectoryPacketIO::~DirectoryPacketIO()
{
    if (_lock_fd >= 0) ::close(_lock_fd);
}

bool DirectoryPacketIO::start_peer()
{
    if (_role != DirectoryPacketRole::Host || _peer_started) return false;
    _lock_fd = ::open(join_file(_directory, "PEER.lock").c_str(), O_CREAT | O_RDWR, 0644);
    if (_lock_fd < 0 || ::flock(_lock_fd, LOCK_EX | LOCK_NB) != 0) return false;
    // Never clear AMBIGUOUS here: a host restart cannot authorize replay.
    for (const char* name : {"ACK", "BARRIER", "RECOVER", "CHALLENGE", "IDENTITY", "FAULT", "RELEASE"}) {
        if (unlink_if_present(join_file(_directory, name)) != PacketIOStatus::Ok)
            return false;
    }
    if (reset() != PacketIOStatus::Ok) return false;
    _challenge = fresh_challenge();
    _peer_started = !_challenge.empty() &&
        publish_control(join_file(_directory, "CHALLENGE"), _challenge);
    return _peer_started;
}

bool DirectoryPacketIO::service_barrier()
{
    if (!_peer_started) return false;
    const auto barrier = join_file(_directory, ("BARRIER." + _challenge.substr(0, 32)).c_str());
    const auto request = read_control(barrier);
    if (!request.empty()) {
        if (unlink_if_present(barrier) != PacketIOStatus::Ok) return false;
        if (request != _challenge) return true; // stale/interrupted participant
        // IOService::serviceOnce handles and sends synchronously. We are outside
        // core.tick: no handler is running, and _held is the only deferred reply.
        _held.clear();
        if (unlink_if_present(join_file(_directory, "FAULT")) != PacketIOStatus::Ok ||
            unlink_if_present(join_file(_directory, "RELEASE")) != PacketIOStatus::Ok)
            return false;
        if (reset() != PacketIOStatus::Ok) return false;
        const auto next = fresh_challenge();
        if (next.empty() || !publish_control(join_file(_directory, "CHALLENGE"), next))
            return false;
        _challenge = next; // rotate BEFORE publishing the acknowledgement
        if (!publish_control(join_file(_directory, "ACK"), request)) return false;
        std::cout << "native-test barrier drained " << request << std::flush;
    }
    if (read_control(join_file(_directory, "RELEASE")) == "release\n") {
        if (unlink_if_present(join_file(_directory, "RELEASE")) != PacketIOStatus::Ok)
            return false;
        if (_held.empty()) return true; // unused release cannot affect a future fault
        auto reply = std::move(_held);
        _held.clear();
        if (send(reply.data(), reply.size()) != PacketIOStatus::Ok) return false;
    }
    return true;
}

std::string DirectoryPacketIO::receive_path() const
{
    return join_file(_directory,
                     _role == DirectoryPacketRole::Host ? kDirectoryPacketToHostName
                                                        : kDirectoryPacketToGuestName);
}

std::string DirectoryPacketIO::send_path() const
{
    return join_file(_directory,
                     _role == DirectoryPacketRole::Host ? kDirectoryPacketToGuestName
                                                        : kDirectoryPacketToHostName);
}

PacketIOStatus DirectoryPacketIO::discard_records()
{
    if (!directory_usable(_directory)) {
        return PacketIOStatus::Unavailable;
    }

    PacketIOStatus status = PacketIOStatus::Ok;
    const char* names[] = {
        kDirectoryPacketToHostName,
        kDirectoryPacketToGuestName,
    };
    for (const char* name : names) {
        const std::string path = join_file(_directory, name);
        const PacketIOStatus pkt = unlink_if_present(path);
        const PacketIOStatus tmp = unlink_if_present(path + ".tmp");
        if (pkt != PacketIOStatus::Ok) {
            status = pkt;
        }
        if (tmp != PacketIOStatus::Ok) {
            status = tmp;
        }
    }
    return status;
}

PacketReceiveResult DirectoryPacketIO::receive(std::uint8_t* buffer, std::size_t limit)
{
    if (_reset_required) {
        return {PacketIOStatus::ResetRequired};
    }
    if (!directory_usable(_directory)) {
        return {PacketIOStatus::Unavailable};
    }

    const std::string path = receive_path();
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        if (errno == ENOENT) {
            return {PacketIOStatus::NoData};
        }
        return {PacketIOStatus::Unavailable};
    }
    if (!S_ISREG(st.st_mode)) {
        (void)unlink_if_present(path);
        return {PacketIOStatus::Unavailable};
    }

    const auto size = static_cast<std::size_t>(st.st_size);
    if (size == 0) {
        (void)unlink_if_present(path);
        return {PacketIOStatus::EmptyPacket};
    }
    if (size > capacity() || size > limit) {
        (void)unlink_if_present(path);
        return {PacketIOStatus::Oversized};
    }
    if (buffer == nullptr) {
        (void)unlink_if_present(path);
        return {PacketIOStatus::Oversized};
    }

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            return {PacketIOStatus::NoData};
        }
        return {PacketIOStatus::Unavailable};
    }

    std::vector<std::uint8_t> staging(size);
    std::size_t got = 0;
    while (got < size) {
        const ssize_t n = read_retry(fd, staging.data() + got, size - got);
        if (n <= 0) {
            ::close(fd);
            (void)unlink_if_present(path);
            return {PacketIOStatus::Truncated};
        }
        got += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (unlink_if_present(path) != PacketIOStatus::Ok) {
        return {PacketIOStatus::Unavailable};
    }
    std::memcpy(buffer, staging.data(), size);
    return {PacketIOStatus::Ok, size};
}

PacketIOStatus DirectoryPacketIO::send(const std::uint8_t* packet, std::size_t size)
{
    if (_reset_required) {
        return PacketIOStatus::ResetRequired;
    }
    if (!directory_usable(_directory)) {
        return PacketIOStatus::Unavailable;
    }
    if (size == 0) {
        return PacketIOStatus::EmptyPacket;
    }
    if (size > capacity() || packet == nullptr) {
        return PacketIOStatus::Oversized;
    }

    if (_peer_started && _role == DirectoryPacketRole::Host) {
        const auto faultPath = join_file(_directory, "FAULT");
        const auto fault = read_control(faultPath);
        if (fault == "hold\n" || fault == "drop\n") {
            if (unlink_if_present(faultPath) != PacketIOStatus::Ok)
                return PacketIOStatus::SendFailed;
            if (fault == "hold\n") _held.assign(packet, packet + size);
            std::cout << "native-test actual reply " << fault << std::flush;
            return PacketIOStatus::Ok;
        }
    }

    const std::string dest = send_path();
    struct stat st {};
    if (::stat(dest.c_str(), &st) == 0) {
        return PacketIOStatus::Backpressure;
    }

    const std::string tmp = dest + ".tmp";
    (void)unlink_if_present(tmp);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);
    if (fd < 0) {
        return directory_usable(_directory) ? PacketIOStatus::SendFailed
                                            : PacketIOStatus::Unavailable;
    }

    std::size_t put = 0;
    while (put < size) {
        const ssize_t n = write_retry(fd, packet + put, size - put);
        if (n <= 0) {
            ::close(fd);
            (void)unlink_if_present(tmp);
            return PacketIOStatus::SendFailed;
        }
        put += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        ::close(fd);
        (void)unlink_if_present(tmp);
        return PacketIOStatus::SendFailed;
    }
    ::close(fd);

    if (::stat(dest.c_str(), &st) == 0) {
        (void)unlink_if_present(tmp);
        return PacketIOStatus::Backpressure;
    }
    if (::rename(tmp.c_str(), dest.c_str()) != 0) {
        const int err = errno;
        (void)unlink_if_present(tmp);
        if (err == EEXIST) {
            return PacketIOStatus::Backpressure;
        }
        return directory_usable(_directory) ? PacketIOStatus::SendFailed
                                            : PacketIOStatus::Unavailable;
    }
    return PacketIOStatus::Ok;
}

PacketIOStatus DirectoryPacketIO::reset()
{
    const PacketIOStatus status = discard_records();
    _reset_required = (status != PacketIOStatus::Ok);
    return status;
}

bool DirectoryPacketChannel::available()
{
    ++byteCalls;
    return false;
}

std::size_t DirectoryPacketChannel::read(std::uint8_t*, std::size_t)
{
    ++byteCalls;
    return 0;
}

void DirectoryPacketChannel::write(const std::uint8_t*, std::size_t)
{
    ++byteCalls;
}

bool write_native_test_identity(const std::string& directory)
{
    if (!directory_usable(directory)) {
        return false;
    }
    const std::string path = join_file(directory, kDirectoryPacketIdentityName);
    const std::string body = std::string(kNativeTestIdentityToken) + "\n";
    const std::string tmp = path + ".tmp";
    (void)unlink_if_present(tmp);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    const ssize_t n = ::write(fd, body.data(), body.size());
    if (n != static_cast<ssize_t>(body.size()) || ::fsync(fd) != 0) {
        ::close(fd);
        (void)unlink_if_present(tmp);
        return false;
    }
    ::close(fd);
    (void)::unlink(path.c_str());
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        (void)unlink_if_present(tmp);
        return false;
    }
    return true;
}

} // namespace fujinet::native_test
