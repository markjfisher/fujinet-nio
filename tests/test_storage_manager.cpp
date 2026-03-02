#include "doctest.h"

#include "fujinet/fs/storage_manager.h"
#include "fujinet/fs/filesystem.h"

#include <algorithm>

using namespace fujinet::fs;

// Mock filesystem implementation for testing
class MockFileSystem : public IFileSystem {
public:
    explicit MockFileSystem(const std::string& name) : _name(name) {}
    
    FileSystemKind kind() const override { return FileSystemKind::Unknown; }
    std::string name() const override { return _name; }
    bool exists(const std::string& path) override { return false; }
    bool isDirectory(const std::string& path) override { return false; }
    bool createDirectory(const std::string& path) override { return false; }
    bool removeFile(const std::string& path) override { return false; }
    bool removeDirectory(const std::string& path) override { return false; }
    bool rename(const std::string& from, const std::string& to) override { return false; }
    std::unique_ptr<IFile> open(const std::string& path, const char* mode) override { return nullptr; }
    bool stat(const std::string& path, FileInfo& outInfo) override { return false; }
    bool listDirectory(const std::string& path, std::vector<FileInfo>& outEntries) override { return false; }
    
private:
    std::string _name;
};

TEST_CASE("StorageManager: register and get filesystem")
{
    StorageManager manager;
    
    // Register mock filesystems
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("sd")));
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("tnfs")));
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("http")));
    
    // Check we can retrieve them
    CHECK(manager.get("sd") != nullptr);
    CHECK(manager.get("tnfs") != nullptr);
    CHECK(manager.get("http") != nullptr);
    
    // Check non-existent filesystem returns null
    CHECK(manager.get("nonexistent") == nullptr);
}

TEST_CASE("StorageManager: register duplicate filesystem")
{
    StorageManager manager;
    
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("sd")));
    CHECK_FALSE(manager.registerFileSystem(std::make_unique<MockFileSystem>("sd"))); // Duplicate
}

TEST_CASE("StorageManager: unregister filesystem")
{
    StorageManager manager;
    
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("sd")));
    CHECK(manager.unregisterFileSystem("sd"));
    CHECK(manager.get("sd") == nullptr);
    
    // Unregistering non-existent filesystem returns false
    CHECK_FALSE(manager.unregisterFileSystem("nonexistent"));
}

TEST_CASE("StorageManager: list names")
{
    StorageManager manager;
    
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("sd")));
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("tnfs")));
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("http")));
    
    auto names = manager.listNames();
    
    CHECK(names.size() == 3);
    CHECK(std::find(names.begin(), names.end(), "sd") != names.end());
    CHECK(std::find(names.begin(), names.end(), "tnfs") != names.end());
    CHECK(std::find(names.begin(), names.end(), "http") != names.end());
}

TEST_CASE("StorageManager: get by scheme")
{
    StorageManager manager;
    
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("sd")));
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("tnfs")));
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("http")));
    
    CHECK(manager.getByScheme("sd") != nullptr);
    CHECK(manager.getByScheme("tnfs") != nullptr);
    CHECK(manager.getByScheme("http") != nullptr);
    CHECK(manager.getByScheme("nonexistent") == nullptr);
}

TEST_CASE("StorageManager: resolveUri")
{
    StorageManager manager;
    
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("sd")));
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("tnfs")));
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("http")));
    
    // Test sd: scheme
    auto [sdFs, sdPath] = manager.resolveUri("sd:/path/to/file.ssd");
    CHECK(sdFs != nullptr);
    CHECK(sdFs->name() == "sd");
    CHECK(sdPath == "/path/to/file.ssd");
    
    // Test tnfs: scheme
    auto [tnfsFs, tnfsPath] = manager.resolveUri("tnfs://server/path/to/image.atr");
    CHECK(tnfsFs != nullptr);
    CHECK(tnfsFs->name() == "tnfs");
    CHECK(tnfsPath == "/path/to/image.atr");
    
    // Test http: scheme
    auto [httpFs, httpPath] = manager.resolveUri("http://example.com/files/disk.dsk");
    CHECK(httpFs != nullptr);
    CHECK(httpFs->name() == "http");
    CHECK(httpPath == "/files/disk.dsk");
    
    // Test unknown scheme
    auto [unknownFs, unknownPath] = manager.resolveUri("unknown:/path/to/file");
    CHECK(unknownFs == nullptr);
    CHECK(unknownPath == "");
}

TEST_CASE("StorageManager: resolveUri with no scheme")
{
    StorageManager manager;
    
    // Without any filesystems registered, should return null
    auto [fs1, path1] = manager.resolveUri("/absolute/path");
    CHECK(fs1 == nullptr);
    
    // Register host filesystem
    CHECK(manager.registerFileSystem(std::make_unique<MockFileSystem>("host")));
    
    // Should still return null for no scheme (not implemented yet)
    auto [fs2, path2] = manager.resolveUri("/absolute/path");
    CHECK(fs2 == nullptr);
}
