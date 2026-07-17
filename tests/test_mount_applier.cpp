#include "doctest.h"

#include "fake_fs.h"

#include "fujinet/config/fuji_config.h"
#include "fujinet/disk/disk_service.h"
#include "fujinet/disk/image_registry.h"
#include "fujinet/fs/mount_applier.h"
#include "fujinet/fs/storage_manager.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace {

std::unique_ptr<fujinet::tests::MemoryFileSystem> make_host_fs_with_boot()
{
    auto fs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");
    REQUIRE(fs->createDirectory("/boot"));
    const std::vector<std::uint8_t> bytes{0, 1, 2, 3};
    REQUIRE(fs->create_file("/boot/autorun.atr", bytes));
    return fs;
}

} // namespace

TEST_CASE("apply_boot_mount skips normal mode")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Normal;
    boot.configUri = "persist:/boot/autorun.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK(!disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount applies config disk as pending read-only mount")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/autorun.atr";
    boot.readOnly = true;

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);
    auto pending = disk.get_pending_mount(0);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "persist:/boot/autorun.atr");
    CHECK(pending->mode == "r");
    CHECK(pending->enabled == true);
}

TEST_CASE("apply_boot_mount skips missing config disk")
{
    fujinet::fs::StorageManager storage;
    auto fs = std::make_unique<fujinet::tests::MemoryFileSystem>("host");
    REQUIRE(storage.registerFileSystem(std::move(fs)));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/missing.atr";

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 0) == 0);
    CHECK(!disk.get_pending_mount(0).has_value());
}

TEST_CASE("apply_boot_mount uses the bootstrap-selected active disk unit")
{
    fujinet::fs::StorageManager storage;
    REQUIRE(storage.registerFileSystem(make_host_fs_with_boot()));
    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/autorun.atr";
    boot.readOnly = true;

    CHECK(fujinet::apply_boot_mount(disk, storage, boot, 1) == 1);
    CHECK(!disk.get_pending_mount(0).has_value());
    auto pending = disk.get_pending_mount(1);
    REQUIRE(pending.has_value());
    CHECK(pending->uri == "persist:/boot/autorun.atr");
}

TEST_CASE("apply_config_mounts_excluding leaves boot runtime slot untouched")
{
    fujinet::fs::StorageManager storage;
    auto fs = make_host_fs_with_boot();
    REQUIRE(fs->create_file("/disk1.img", std::vector<std::uint8_t>{1}));
    REQUIRE(fs->create_file("/disk2.img", std::vector<std::uint8_t>{2}));
    REQUIRE(storage.registerFileSystem(std::move(fs)));

    fujinet::disk::DiskService disk(storage, fujinet::disk::ImageRegistry{});

    fujinet::config::BootConfig boot{};
    boot.mode = fujinet::config::BootMode::Config;
    boot.configUri = "persist:/boot/autorun.atr";
    boot.readOnly = true;

    REQUIRE(fujinet::apply_boot_mount(disk, storage, boot, 0) == 1);

    fujinet::config::MountConfig first{};
    first.slot = 1;
    first.uri = "host:/disk1.img";
    first.mode = "rw";
    first.enabled = true;

    fujinet::config::MountConfig second{};
    second.slot = 2;
    second.uri = "host:/disk2.img";
    second.mode = "rw";
    second.enabled = true;

    CHECK(fujinet::apply_config_mounts_excluding(disk, storage, {first, second}, {0}) == 1);

    auto bootPending = disk.get_pending_mount(0);
    REQUIRE(bootPending.has_value());
    CHECK(bootPending->uri == "persist:/boot/autorun.atr");

    auto secondPending = disk.get_pending_mount(1);
    REQUIRE(secondPending.has_value());
    CHECK(secondPending->uri == "host:/disk2.img");
}
