#pragma once

#include <cstdint>
#include <vector>

namespace fujinet::io {

// Logical device identifier inside your system.
// Transports (RS232/SIO/IEC/…) map bus-level addressing to this.
using DeviceID  = std::uint8_t;

// Used to correlate request/response pairs.
// Transports generate these; devices just echo them back.
using RequestID = std::uint32_t;

// High-level type of request.
enum class RequestType : std::uint8_t
{
    Command,    // Generic command/operation
    Read,       // Host wants to read data from device
    Write,      // Host sends data to be written/stored
    Open,       // Open a logical channel / file / session
    Close,      // Close logical channel / file / session
    Control     // Misc control (ioctl-style), not fitting the above
};

// Unified view of a host → device operation.
struct IORequest
{
    RequestID   id{};       // Correlates to IOResponse::id
    DeviceID    deviceId{}; // Which virtual device this is for
    RequestType type{RequestType::Command};

    // Optional extra command / subcode, device-specific.
    std::uint8_t command{0};

    // Raw payload from host to device.
    std::vector<std::uint8_t> payload;
};

// Result of a device handling an IORequest.
enum class StatusCode : std::uint8_t
{
    Ok = 0,             // Success
    DeviceNotFound,
    InvalidRequest,
    DeviceBusy,
    NotReady,
    IOError,
    Timeout,
    Unsupported,
};

// Device → host response.
struct IOResponse
{
    RequestID   id{};       // Must match the originating IORequest::id
    DeviceID    deviceId{}; // Echoed back for sanity / debugging
    StatusCode  status{StatusCode::Ok};

    // Raw payload back to the host.
    std::vector<std::uint8_t> payload;
};

} // namespace fujinet::io
