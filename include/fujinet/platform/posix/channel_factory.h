// include/fujinet/platform/posix/channel_factory.h
#pragma once

#include <memory>
#include "fujinet/io/core/channel.h"

namespace fujinet::platform::posix {

std::unique_ptr<fujinet::io::Channel> create_default_channel();

} // namespace fujinet::platform::posix
