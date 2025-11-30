#include <cassert>
#include <iostream>

#include "fujinet/io/core/io_message.h"

using namespace fujinet::io;

int main()
{
    std::cout << "[test_smoke] Running simple checks...\n";

    IORequest req;
    req.id       = 42;
    req.deviceId = 1;
    req.type     = RequestType::Read;
    req.command  = 0x10;
    req.payload  = {1, 2, 3};

    IOResponse resp;
    resp.id       = req.id;
    resp.deviceId = req.deviceId;
    resp.status   = StatusCode::Ok;
    resp.payload  = req.payload;

    assert(resp.id == req.id);
    assert(resp.deviceId == req.deviceId);
    assert(resp.payload.size() == 3);

    std::cout << "[test_smoke] OK\n";
    return 0;
}
