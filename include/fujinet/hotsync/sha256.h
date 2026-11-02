#pragma once

// SHA-256 (FIPS 180-4), for checking a hotsync.fetch download against the
// digest its publisher gave. Portable, so it needs no TLS library.

#include "fujinet/hotsync/bytes.h"

#include <string>

namespace fujinet::hotsync {

// The digest of `data` as 64 lower-case hex digits.
std::string sha256_hex(const Bytes& data);

} // namespace fujinet::hotsync
