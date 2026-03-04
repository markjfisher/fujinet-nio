
#include "doctest.h"

#include "fujinet/fs/tnfs_filesystem.h"
#include "fujinet/tnfs/tnfs_protocol.h"

#include <memory>
#include <cstring>

using namespace fujinet::fs;
using namespace fujinet::tnfs;

// Mock TNFS client implementation for testing
class MockTnfsClient : public ITnfsClient {
public:
    bool mount(const std::string& mountPath, const std::string& user, const std::string& password) override {
        return true; // Always succeed for testing
    }

    bool umount() override {
        return true;
    }

    bool stat(const std::string& path, TnfsStat& stat) override {
        if (path == "/testfile") {
            stat.isDir = false;
            stat.filesize = 1024;
            stat.aTime = 123456789;
            stat.mTime = 987654321;
            stat.cTime = 123456789;
            stat.mode = 0;
            return true;
        } else if (path == "/testdir") {
            stat.isDir = true;
            stat.filesize = 0;
            stat.aTime = 123456789;
            stat.mTime = 987654321;
            stat.cTime = 123456789;
            stat.mode = 0;
            return true;
        } else if (path == "/testdir/file1.txt" || path == "/testdir/file2.txt") {
            stat.isDir = false;
            stat.filesize = 512;
            stat.aTime = 123456789;
            stat.mTime = 987654321;
            stat.cTime = 123456789;
            stat.mode = 0;
            return true;
        } else if (path == "/testdir/subdir") {
            stat.isDir = true;
            stat.filesize = 0;
            stat.aTime = 123456789;
            stat.mTime = 987654321;
            stat.cTime = 123456789;
            stat.mode = 0;
            return true;
        }
        return false;
    }

    bool exists(const std::string& path) override {
        return path == "/testfile" || path == "/testdir";
    }

    bool isDirectory(const std::string& path) override {
        return path == "/testdir";
    }

    bool createDirectory(const std::string& path) override {
        return true;
    }

    bool removeDirectory(const std::string& path) override {
        return true;
    }

    bool removeFile(const std::string& path) override {
        return true;
    }

    bool rename(const std::string& from, const std::string& to) override {
        return true;
    }

    std::vector<std::string> listDirectory(const std::string& path) override {
        if (path == "/testdir") {
            return {"file1.txt", "file2.txt", "subdir"};
        }
        return {};
    }

    int open(const std::string& path, uint16_t openMode, uint16_t createPerms) override {
        if (path == "/testfile") {
            return 1; // Return valid file handle
        }
        return -1;
    }

    bool close(int fileHandle) override {
        return true;
    }

    std::size_t read(int fileHandle, void* buffer, std::size_t bytes) override {
        if (fileHandle == 1) {
            // Return some dummy data
            std::memset(buffer, 'A', bytes);
            return bytes;
        }
        return 0;
    }

    std::size_t write(int fileHandle, const void* buffer, std::size_t bytes) override {
        if (fileHandle == 1) {
            return bytes;
        }
        return 0;
    }

    bool seek(int fileHandle, uint32_t offset) override {
        if (fileHandle == 1) {
            return true;
        }
        return false;
    }

    uint32_t tell(int fileHandle) override {
        if (fileHandle == 1) {
            return 0;
        }
        return 0;
    }
};

TEST_CASE("TnfsFileSystem: create and basic properties")
{
    auto mockClient = std::make_shared<MockTnfsClient>();
    auto fs = make_tnfs_filesystem(mockClient);

    CHECK(fs != nullptr);
    CHECK(fs->name() == "tnfs");
    CHECK(fs->kind() == FileSystemKind::NetworkTnfs);
}

TEST_CASE("TnfsFileSystem: file operations")
{
    auto mockClient = std::make_shared<MockTnfsClient>();
    auto fs = make_tnfs_filesystem(mockClient);

    CHECK(fs->exists("/testfile"));
    CHECK_FALSE(fs->exists("/nonexistent"));

    CHECK_FALSE(fs->isDirectory("/testfile"));
    CHECK(fs->isDirectory("/testdir"));

    FileInfo info;
    CHECK(fs->stat("/testfile", info));
    CHECK(info.isDirectory == false);
    CHECK(info.sizeBytes == 1024);
}

TEST_CASE("TnfsFileSystem: directory operations")
{
    auto mockClient = std::make_shared<MockTnfsClient>();
    auto fs = make_tnfs_filesystem(mockClient);

    std::vector<FileInfo> entries;
    CHECK(fs->listDirectory("/testdir", entries));
    CHECK(entries.size() == 3);

    // Check if all expected entries are present
    bool hasFile1 = false, hasFile2 = false, hasSubdir = false;
    for (const auto& entry : entries) {
        if (entry.path.find("file1.txt") != std::string::npos) {
            hasFile1 = true;
        } else if (entry.path.find("file2.txt") != std::string::npos) {
            hasFile2 = true;
        } else if (entry.path.find("subdir") != std::string::npos) {
            hasSubdir = true;
        }
    }

    CHECK(hasFile1);
    CHECK(hasFile2);
    CHECK(hasSubdir);
}

TEST_CASE("TnfsFileSystem: open file")
{
    auto mockClient = std::make_shared<MockTnfsClient>();
    auto fs = make_tnfs_filesystem(mockClient);

    auto file = fs->open("/testfile", "rb");
    CHECK(file != nullptr);
}

TEST_CASE("TnfsFileSystem: operations on invalid paths")
{
    auto mockClient = std::make_shared<MockTnfsClient>();
    auto fs = make_tnfs_filesystem(mockClient);

    FileInfo info;
    CHECK_FALSE(fs->stat("/nonexistent", info));
    CHECK_FALSE(fs->isDirectory("/nonexistent"));

    std::vector<FileInfo> entries;
    CHECK_FALSE(fs->listDirectory("/nonexistent", entries));
    CHECK(entries.empty());

    auto file = fs->open("/nonexistent", "rb");
    CHECK(file == nullptr);
}
