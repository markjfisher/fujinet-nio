#pragma once

// DiskDevice v1 uses the shared byte codec (same pattern as net_codec.h).

#include "fujinet/io/devices/byte_codec.h"

namespace fujinet::io::diskproto {

using Reader = fujinet::io::bytecodec::Reader;

using fujinet::io::bytecodec::write_u8;
using fujinet::io::bytecodec::write_u16le;
using fujinet::io::bytecodec::write_u32le;
using fujinet::io::bytecodec::write_u64le;
using fujinet::io::bytecodec::write_bytes;
using fujinet::io::bytecodec::write_lp_u16_string;

} // namespace fujinet::io::diskproto


