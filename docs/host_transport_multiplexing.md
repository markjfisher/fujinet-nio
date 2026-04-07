# Host Transport Multiplexing Notes

This document captures an important limitation discovered while testing
`fujinet-nio` over a POSIX PTY channel: multiple host-side client processes must
not open and use the same PTY/serial endpoint concurrently unless a dedicated
broker/multiplexing layer exists in front of that endpoint.

This is primarily a future-design note for possible "FujiNet As A Service"
(FaaS) scenarios. It is not a requirement for the normal ESP32-style deployment
where a single physical host talks to a single FujiNet device.

---

## 1. What Was Observed

During testing, two different host-side applications were pointed at the same
POSIX PTY endpoint exposed by `fujinet-nio`:

- a BBC emulator running a BASIC application
- a Linux test client (`tcp_get` from `fujinet-nio-lib`)

Both applications opened the same PTY slave path, for example:

    /tmp/fujinet-pty -> /dev/pts/8

The observed behavior was:

- the BBC client worked correctly when running alone
- starting the Linux client immediately caused failures
- the Linux client sometimes timed out waiting for responses that `fujinet-nio`
  had clearly sent
- the BBC client then began receiving protocol errors such as
  `StatusCode::InvalidRequest`

At first glance this looked like handle/session corruption inside FujiNet. It
was not.

---

## 2. Why FujiNet Handles Did Not Prevent This

FujiNet handle allocation works at the protocol layer, after a complete request
has already arrived at the device.

For example, separate clients can successfully receive distinct handle values
for separate TCP sessions. That part of the design is functioning correctly.

However, the PTY itself is below the handle/session layer.

The PTY is just a shared byte stream. If two unrelated host processes open the
same PTY slave and both start speaking FujiBus-over-SLIP, then:

- both processes write into the same byte stream
- both processes may read from the same incoming byte stream
- one process can consume bytes that logically belong to the other
- one process can flush or discard bytes that logically belong to the other
- request/response pairing becomes unreliable before handle-based routing even
  has a chance to help

So:

- **handles isolate FujiNet sessions inside the protocol**
- **they do not isolate multiple host processes sharing one raw transport**

---

## 3. Root Cause

The root cause is that the current POSIX client model assumes exclusive access
to the transport endpoint.

In `fujinet-nio-lib`, the Linux transport simply opens the PTY/serial device
directly, for example with:

```c
open(port, O_RDWR | O_NOCTTY | O_NONBLOCK)
```

There is currently:

- no exclusivity enforcement
- no lock file or advisory locking contract
- no `TIOCEXCL`
- no broker process
- no request router shared between multiple client processes

That means concurrent opens are possible, but not safe.

---

## 4. Why The Failures Looked Random

When two applications share one PTY endpoint, the failure mode depends on
timing.

Possible symptoms include:

- a client timing out even though `fujinet-nio` sent a valid response
- a client receiving a response intended for another client
- a client missing bytes because another client consumed them first
- cursor mismatches and `InvalidRequest` responses caused by host-side state
  drifting out of sync after one bad exchange
- behavior changing after restart because the timing and initial state change

This explains why a second client could appear to "break" a first client at the
exact moment it started, even though the two FujiNet handles were distinct.

The conflict happened on the shared transport stream, not in handle allocation.

---

## 5. Important Conclusion

For the current PTY/serial transport model:

> One raw endpoint must be owned by exactly one host-side transport client at a
> time.

This should be treated as a practical rule unless and until a proper
multiplexing layer is implemented.

This applies to:

- PTY-backed POSIX testing
- likely direct serial access as well
- any raw byte-stream channel where FujiBus-over-SLIP is being spoken directly
  by multiple host processes

---

## 6. Why This Usually Does Not Matter On Real Hardware

In the common ESP32 deployment, FujiNet is attached to a single physical host.
That host owns the serial/USB connection, and there is no expectation that two
independent user-space applications will both speak raw FujiBus to the same
device concurrently.

So this limitation is mostly a concern for:

- POSIX/PTTY development setups
- emulator integration
- host tooling
- any future service-oriented deployment model

---

## 7. Future "FujiNet As A Service" Direction

If multi-client host access becomes a real requirement, the correct solution is
not to let multiple processes open the PTY directly.

The correct solution is a **host-side broker/multiplexing daemon**.

That daemon would:

- own the real FujiNet endpoint exclusively
- speak FujiBus-over-SLIP to `fujinet-nio`
- accept requests from multiple local client applications over a separate IPC
  mechanism such as a Unix domain socket
- serialize or route those requests safely
- deliver responses back to the correct client

This keeps the raw transport single-owner while still enabling multiple client
applications to share one FujiNet instance.

---

## 8. Where Such A Broker Should Live

If implemented in the future, the broker should live **outside the core
`fujinet-nio` engine**, though it could still live in the same repository as a
host-side tool.

Reasoning:

- `fujinet-nio` core is organized around channels, transports, core routing, and
  virtual devices
- multi-process host access arbitration is a host-side deployment concern, not a
  virtual device concern
- this is mainly relevant to POSIX/service scenarios, not the embedded firmware
  model

So the clean separation is:

- `fujinet-nio` remains the device/protocol server
- a future broker is an optional host-side companion process

---

## 9. Short-Term Practical Guidance

Until any broker exists, host tools and test clients should assume the
transport endpoint is exclusive.

Practical consequences:

- do not run two FujiNet client applications against the same PTY slave at the
  same time
- do not assume concurrent PTY access is safe just because FujiNet allocates
  different handles
- if needed in the future, add explicit exclusive-open checks in host transport
  code so failures are immediate and obvious

---

## 10. Bottom Line

The observed client failures were not evidence that FujiNet handle allocation or
network session isolation was fundamentally broken.

They were evidence that:

- the PTY/serial transport layer is currently single-owner in practice
- concurrent host access to one raw endpoint is unsafe
- any future multi-client deployment requires a broker layer in front of the raw
  transport

This is an important distinction:

- **session multiplexing inside FujiNet already exists via handles**
- **transport multiplexing between multiple host processes does not**

Those are separate problems at different layers.
