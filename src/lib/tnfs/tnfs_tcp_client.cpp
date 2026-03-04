#include "fujinet/tnfs/tnfs_protocol.h"
#include "fujinet/tnfs/tnfs_client_common.h"

namespace fujinet::tnfs {

std::unique_ptr<ITnfsClient> make_tcp_tnfs_client(std::unique_ptr<fujinet::io::Channel> channel)
{
    return std::make_unique<CommonTnfsClient>(std::move(channel), "TCP");
}

} // namespace fujinet::tnfs
