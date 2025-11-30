// File: io/core/request_handler.h
#pragma once

#include "fujinet/io/core/io_message.h"

namespace fujinet::io {

class IRequestHandler {
public:
    virtual ~IRequestHandler() = default;

    // Handle a single request and return a response.
    // Implementations may route to devices, modes, etc.
    virtual IOResponse handleRequest(const IORequest& request) = 0;
};

} // namespace fujinet::io
