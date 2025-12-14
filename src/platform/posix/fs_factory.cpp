#include "fujinet/platform/posix/fs_factory.h"
#include "fujinet/fs/fs_stdio.h"
#include "fujinet/core/logging.h"

#include <filesystem>
#include <system_error>

namespace fujinet::platform::posix {

static const char* TAG = "fs";

std::unique_ptr<fujinet::fs::IFileSystem>
create_host_filesystem(const std::string& rootDir)
{
    namespace fs = std::filesystem;
    using fujinet::fs::FileSystemKind;

    fs::path root(rootDir);

    // Policy:
    // - If the root path is relative, we consider it "owned" by the app
    //   and create it if missing.
    // - Absolute paths are assumed to be user-/system-managed.
    if (root.is_relative() && fs::exists(root)) {
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec) {
            FN_LOGE(TAG,
                    "Failed to create host filesystem root '%s': %s",
                    rootDir.c_str(),
                    ec.message().c_str());
            // We still return the FS; open() will fail later with a clearer error
        }
    }

    return fujinet::fs::create_stdio_filesystem(
        rootDir,
        "host",
        FileSystemKind::HostPosix
    );
}

} // namespace fujinet::platform::posix
