#include "fujinet/platform/posix/fs_factory.h"
#include "fujinet/fs/fs_stdio.h"

namespace fujinet::platform::posix {

std::unique_ptr<fujinet::fs::IFileSystem>
create_host_filesystem(const std::string& rootDir)
{
    using fujinet::fs::FileSystemKind;
    return fujinet::fs::create_stdio_filesystem(
        rootDir,
        "host",
        FileSystemKind::HostPosix
    );
}

} // namespace fujinet::platform::posix
