#pragma once

// Back-compat wrapper around the shared byte codec.
// FileDevice historically used fujinet::io::fileproto::Reader + write_uXXle helpers.
// Those are now implemented in include/fujinet/io/devices/byte_codec.h.

#include "fujinet/io/devices/byte_codec.h"

namespace fujinet::io::fileproto {

using Reader = fujinet::io::bytecodec::Reader;

using fujinet::io::bytecodec::write_u8;
using fujinet::io::bytecodec::write_u16le;
using fujinet::io::bytecodec::write_u32le;
using fujinet::io::bytecodec::write_u64le;
using fujinet::io::bytecodec::write_bytes;
using fujinet::io::bytecodec::write_sv;

} // namespace fujinet::io::fileproto
