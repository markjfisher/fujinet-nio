#pragma once

#include <cstdint>
#include <vector>

namespace fujinet::io {

// Logical device identifier inside your system.
// Transports (FujiBus / SIO / IEC / …) map bus-level
// addressing (e.g. FujiDeviceId on the wire) to this.
using DeviceID  = std::uint8_t;

// Used to correlate request/response pairs.
// Transports generate these; devices just echo them back.
using RequestID = std::uint32_t;

// High-level type of request.
//
// This is intentionally broad and protocol-agnostic. A given wire protocol
// (e.g. FujiBus/FEP-004) can map its semantics onto these categories, or
// simply always use RequestType::Command if that’s simpler.
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
//
// This is what transports (Fuji serial, SIO, IEC, etc.) convert their
// wire-level packets into, and what VirtualDevice implementations consume.
struct IORequest
{
    // Correlates to IOResponse::id
    RequestID   id{};       

    // Which virtual device this is for (post-routing).
    // For FujiBus, this is derived from the FujiDeviceId on the wire.
    DeviceID    deviceId{}; 

    // Broad operation type; may be ignored by some protocols/devices.
    RequestType type{RequestType::Command};

    // Optional extra command / subcode, device- or protocol-specific.
    //
    // For FujiBus, this maps nicely to FujiCommandId (0x00–0xFF), but
    // we use 16 bits here so other protocols can use larger ranges if
    // needed.
    std::uint16_t command{0};

    // Optional parameter list (out-of-band from the main payload).
    //
    // For FujiBus/FEP-004, this corresponds to the ordered list of
    // 1/2/4-byte parameters described in the spec. Transports are
    // free to ignore this entirely if their protocol doesn’t have an
    // explicit param concept.
    std::vector<std::uint32_t> params;

    // Raw payload from host to device.
    // Interpretation is device- and protocol-specific.
    std::vector<std::uint8_t>  payload;
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
//
// Transports will typically map this back onto their wire-level response
// format (e.g. FujiBus status + payload, SIO ACK/NAK + data, etc.).
struct IOResponse
{
    // Must match the originating IORequest::id
    RequestID   id{};       

    // Echoed back for sanity / debugging.
    DeviceID    deviceId{}; 

    // Result of the operation.
    StatusCode  status{StatusCode::Ok};

    // Optional command echo. For many protocols this will simply mirror
    // IORequest::command, but transports are free to ignore or repurpose
    // it if their wire format has a different notion of "response type".
    std::uint16_t command{0};

    // Raw payload back to the host.
    std::vector<std::uint8_t> payload;
};

} // namespace fujinet::io
