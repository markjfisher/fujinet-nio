#include "directory_packet_io.h"

#include "fujinet/build/profile.h"
#include "fujinet/core/bootstrap.h"
#include "fujinet/core/core.h"
#include "fujinet/core/device_init.h"
#include "fujinet/core/logging.h"
#include "fujinet/platform/posix/fs_factory.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

using fujinet::build::BuildProfile;
using fujinet::build::ChannelKind;
using fujinet::build::Machine;
using fujinet::build::TransportKind;

namespace {

using fujinet::native_test::DirectoryPacketChannel;
using fujinet::native_test::DirectoryPacketIO;
using fujinet::native_test::DirectoryPacketRole;
using fujinet::native_test::kNativeTestIdentityToken;
using fujinet::native_test::write_native_test_identity;

constexpr const char* TAG = "native-test";
constexpr std::size_t kAdapterCapacity = 65535;

std::atomic_bool g_stop{false};

void handle_stop(int)
{
    g_stop.store(true);
}

void print_help()
{
    std::cout
        << "fujinet-nio-native-test\n"
        << "Identity: " << kNativeTestIdentityToken << "\n"
        << "Host endpoint for complete raw FujiBus records via a shared directory.\n"
        << "Test facility only: not a production backend and not a serial path.\n"
        << "\n"
        << "Usage:\n"
        << "  fujinet-nio-native-test --dir DIR\n"
        << "  FN_NATIVE_TEST_DIR=DIR fujinet-nio-native-test\n"
        << "  fujinet-nio-native-test --help\n";
}

std::string resolve_directory(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            return {};
        }
        if (arg == "--dir" || arg == "-d") {
            if (i + 1 >= argc) {
                return {};
            }
            return argv[++i];
        }
        if (arg.size() >= 6 && arg.substr(0, 6) == "--dir=") {
            return std::string(arg.substr(6));
        }
        if (!arg.empty() && arg[0] != '-') {
            return std::string(arg);
        }
    }
    if (const char* env = std::getenv("FN_NATIVE_TEST_DIR")) {
        if (env[0] != '\0') {
            return env;
        }
    }
    return {};
}

bool wants_help(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--help" || arg == "-h") {
            return true;
        }
    }
    return false;
}

BuildProfile native_test_profile()
{
    // Constructed locally. Never current_build_profile() (SLIP/Zorro-PTY).
    // primaryChannel is unused: this runner never calls create_channel_for_profile.
    return BuildProfile{
        .machine = Machine::Generic,
        .primaryTransport = TransportKind::FujiBusNative,
        .primaryChannel = ChannelKind::Pty,
        .name = kNativeTestIdentityToken,
        .hw = {},
    };
}

} // namespace

int main(int argc, char** argv)
{
    if (wants_help(argc, argv)) {
        print_help();
        return 0;
    }

    const std::string directory = resolve_directory(argc, argv);
    if (directory.empty()) {
        print_help();
        std::cerr << "error: missing --dir or FN_NATIVE_TEST_DIR\n";
        return 2;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        FN_LOGE(TAG, "Failed to create native-test directory '%s': %s",
                directory.c_str(), ec.message().c_str());
        return 1;
    }

    const auto identity = std::filesystem::path(directory) /
                          fujinet::native_test::kDirectoryPacketIdentityName;
    std::filesystem::remove(identity, ec);
    if (ec) return 1;
    std::signal(SIGINT, handle_stop);
    std::signal(SIGTERM, handle_stop);

    auto packets = std::make_unique<DirectoryPacketIO>(
        directory, kAdapterCapacity, DirectoryPacketRole::Host);
    if (packets->requires_reset()) {
        FN_LOGE(TAG, "Native-test startup record cleanup failed");
        return 1;
    }

    const auto profile = native_test_profile();
    FN_LOGI(TAG, "fujinet-nio-native-test starting (identity=%s)", kNativeTestIdentityToken);
    FN_LOGI(TAG, "Profile name: %.*s",
            static_cast<int>(profile.name.size()), profile.name.data());
    FN_LOGI(TAG, "Record directory: %s", directory.c_str());
    std::cout << "fujinet-nio-native-test starting (identity=" << kNativeTestIdentityToken
              << ")\n"
              << "Profile name: " << profile.name << "\n"
              << std::flush;

    fujinet::core::FujinetCore core;

    const auto hostRoot = std::filesystem::path(directory) / "host-fs";
    std::filesystem::create_directories(hostRoot, ec);
    auto hostFs = fujinet::platform::posix::create_host_filesystem(hostRoot.string());
    if (!hostFs || !core.storageManager().registerFileSystem(std::move(hostFs))) {
        FN_LOGE(TAG, "Failed to register host filesystem");
        (void)packets->reset();
        return 1;
    }

    fujinet::core::register_file_device(core);
    fujinet::core::register_clock_device(core);

    auto channel = std::make_unique<DirectoryPacketChannel>(packets.get());
    if (!fujinet::core::setup_transports(core, *channel, profile, nullptr)) {
        FN_LOGE(TAG, "FujiBusNative setup failed (no packet capability)");
        (void)packets->reset();
        return 1;
    }
    if (!write_native_test_identity(directory)) {
        FN_LOGE(TAG, "Failed to write IDENTITY as %s", kNativeTestIdentityToken);
        (void)packets->reset();
        return 1;
    }

    while (!g_stop.load()) {
        core.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::filesystem::remove(identity, ec);
    (void)packets->reset();
    FN_LOGI(TAG, "fujinet-nio-native-test exiting (identity=%s)", kNativeTestIdentityToken);
    return 0;
}
