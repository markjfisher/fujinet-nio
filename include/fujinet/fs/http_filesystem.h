
#pragma once

#include "fujinet/fs/filesystem.h"

#include <functional>
#include <string_view>

namespace fujinet::io {
class INetworkProtocol;
}

namespace fujinet::fs {

using HttpProtocolFactory = std::function<std::unique_ptr<io::INetworkProtocol>(std::string_view schemeLower)>;

std::unique_ptr<IFileSystem> make_http_filesystem();
std::unique_ptr<IFileSystem> make_http_filesystem(HttpProtocolFactory protocolFactory);

} // namespace fujinet::fs
