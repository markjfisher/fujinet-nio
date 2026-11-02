#pragma once

#include <cstddef>

namespace fujinet::io {

// Lets something else share a FujiBus transport's line: the Palm cradle
// (docs/hotsync.md, "Cradle") lends it to HotSync between FujiBus requests.
//
// The transport reports what its framer and its request decoder see as they
// do their normal work, so a coordinator needs no SLIP or FujiBus knowledge
// and nothing is decoded twice. The transport offers the line only at its
// idle point: inside receive(), once the framer has no packet to deliver.
// IOService has then handled every request the transport delivered and
// written its response, and the coordinator is told whether the framer still
// holds bytes, so one that takes the line only there, and only when the
// framer holds nothing, can never cut into a request or a response.
//
// All calls are made on the core task, inside the transport's poll() and
// receive().
class IBusLineCoordinator {
public:
    virtual ~IBusLineCoordinator() = default;

    // `count` bytes came off the line.
    virtual void bytes_received(std::size_t count) = 0;

    // A complete frame arrived: a FujiBus request (`valid`), or one the
    // transport dropped.
    virtual void frame_received(bool valid) = 0;

    // The idle point. `partial` is true while the framer still holds bytes,
    // usually the start of a frame that has not finished arriving. Returns
    // true when the transport must drop them: the coordinator has taken the
    // line, so the rest would arrive at another rate, or the frame stalled.
    virtual bool line_idle(bool partial) = 0;
};

} // namespace fujinet::io
