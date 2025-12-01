#include "doctest.h"
#include "fujinet/io/core/io_message.h"

using namespace fujinet::io;

TEST_CASE("Simple IORequest/IOResponse smoke test") {
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

    CHECK(resp.id == req.id);
    CHECK(resp.deviceId == req.deviceId);
    CHECK(resp.payload.size() == 3);
}
