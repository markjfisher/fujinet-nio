#pragma once

// Test harness only. Compiled into fujinet-nio-tests and fujinet-nio-native-test,
// not the production POSIX library.

#include "fujinet/io/core/channel.h"
#include "fujinet/io/core/packet_io.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fujinet::native_test {

inline constexpr std::size_t kDirectoryPacketMaxCapacity = 65535;
inline constexpr const char* kDirectoryPacketToHostName = "to-host.pkt";
inline constexpr const char* kDirectoryPacketToGuestName = "to-guest.pkt";
inline constexpr const char* kDirectoryPacketIdentityName = "IDENTITY";
inline constexpr const char* kNativeTestIdentityToken = "native-test";

enum class DirectoryPacketRole {
    Host,   // Core: receive to-host.pkt, send to-guest.pkt
    Client, // Independent peer: inverted names
};

// Test-only shared-directory IPacketIO. Complete records are whole files after
// an atomic *.tmp + rename. Not a Zorro, SLIP, or bridge ABI.
class DirectoryPacketIO : public fujinet::io::IPacketIO {
public:
    DirectoryPacketIO(std::string directory,
                      std::size_t capacity,
                      DirectoryPacketRole role);
    ~DirectoryPacketIO() override = default;

    fujinet::io::PacketReceiveResult receive(std::uint8_t* buffer, std::size_t capacity) override;
    fujinet::io::PacketIOStatus send(const std::uint8_t* packet, std::size_t size) override;
    fujinet::io::PacketIOStatus reset() override;

    const std::string& directory() const { return _directory; }
    DirectoryPacketRole role() const { return _role; }
    bool requires_reset() const { return _reset_required; }

private:
    std::string receive_path() const;
    std::string send_path() const;
    fujinet::io::PacketIOStatus discard_records();

    const std::string _directory;
    const DirectoryPacketRole _role;
    bool _reset_required{false};
};

class DirectoryPacketChannel : public fujinet::io::Channel {
public:
    explicit DirectoryPacketChannel(fujinet::io::IPacketIO* adapter) : adapter(adapter) {}

    fujinet::io::IPacketIO* packet_io() override { return adapter; }
    bool available() override;
    std::size_t read(std::uint8_t* buffer, std::size_t maxLen) override;
    void write(const std::uint8_t* buffer, std::size_t len) override;

    fujinet::io::IPacketIO* adapter{nullptr};
    std::size_t byteCalls{0};
};

bool write_native_test_identity(const std::string& directory);

} // namespace fujinet::native_test
