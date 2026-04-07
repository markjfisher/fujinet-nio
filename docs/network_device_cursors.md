# NetworkDevice Cursor Semantics

This document explains why the NetworkDevice v1 protocol uses 32-bit offsets
as stream cursors for non-seekable resources such as TCP connections, and why
hosts must track them even when the host only wants to read or write data
sequentially.

It is intended to clarify the design tradeoff for firmware and host
implementations, especially on 8-bit systems where maintaining 32-bit counters
is a real cost.

Related documents:

- `docs/network_device_protocol.md`
- `docs/network_device_tcp.md`

---

## 1. The Short Version

The cursor exists because the NetworkDevice v1 wire protocol is already defined
around `Read(handle, offset, len)` and `Write(handle, offset, data)`.

That means every read and write request already carries an `offset`, even for
resources that are not files and cannot actually seek.

For TCP, that `offset` cannot mean "go to byte N in the stream" because TCP is
a live, non-seekable byte stream. Instead, the offset is reinterpreted as:

- how many bytes the host has already consumed from the read side, or
- how many bytes the host has already handed over to the write side.

In other words, the offset becomes a monotonic cursor rather than a seek
position.

This is not because TCP itself needs repositioning. It is because an
offset-based host/device protocol needs a safe meaning for the offset field
when used with a non-seekable stream.

---

## 2. Why TCP Needs a Cursor at All

TCP provides only these semantics:

- receive the next bytes in order
- send the next bytes in order

TCP does **not** provide:

- read from an earlier position
- write at an earlier position
- replay a previously read region
- insert data into the middle of the stream

So when the NetworkDevice protocol says:

    Read(handle, offset, len)

and:

    Write(handle, offset, data)

there are only two possible designs for TCP:

1. Ignore the offset
2. Require the offset to match the current stream position

The v1 TCP design chooses option 2 because option 1 is unsafe and ambiguous.

---

## 3. What Goes Wrong If Offsets Are Ignored

If the backend simply ignored the offset for TCP, many failure cases would turn
into silent corruption instead of a clean protocol error.

### Case A: Duplicate write request

Suppose the host sends 100 bytes, but the command is retried because of a bus
retry, timeout, or a host-side logic error.

If offsets are ignored:

- the backend cannot tell that this is a replay of an earlier write request
- it may send the same 100 bytes again
- the remote peer sees duplicated application data

For line-oriented or framed protocols this can be disastrous. A command might
be executed twice, a message might be duplicated, or a binary protocol stream
may become invalid.

If offsets are enforced:

- the first write advances the write cursor
- the retried write still carries the old offset
- the backend returns `InvalidRequest`
- the host learns immediately that it is out of sync

### Case B: Duplicate read request

Suppose the host asks to read at offset 1000, gets data, but due to a retry bug
asks again at offset 1000.

If offsets are ignored:

- TCP cannot replay the old bytes
- the backend will return the next bytes currently available
- the host may believe it has re-read the same region, but it has actually
  consumed later data
- host state and stream state diverge silently

If offsets are enforced:

- the second request is rejected
- the host knows it must not repeat an already-consumed read position

### Case C: Out-of-order requests

Suppose a buggy client issues reads or writes out of order.

If offsets are ignored:

- the backend still performs the next TCP operation available
- the host's internal model of the stream becomes false
- later recovery becomes nearly impossible because the protocol has no way to
  reconstruct what the host thinks happened

If offsets are enforced:

- the bad request fails immediately
- the error is detected at the point where synchronization is lost

### Case D: Partial writes and retry handling

TCP `send()` may accept fewer bytes than requested.

If the protocol has no enforced cursor:

- the host has no precise contract for what the backend accepted
- retry behavior becomes implementation-defined
- repeated sends can duplicate or skip data

If the protocol uses a cursor:

- the backend advances the cursor only by bytes actually accepted
- the host retries from the new cursor value
- both sides agree exactly on how much of the stream has been committed

---

## 4. Why This Matters Even on a "Simple" Sequential Read

An 8-bit client may reasonably say:

> I do not want random access. I just want to keep reading the stream.

That is understandable, but even a purely sequential reader still needs a way
to prove to the backend which part of the stream it believes it is up to.

Without that, the device cannot distinguish between:

- a valid next read
- a duplicated old read command
- an out-of-order read
- a host that lost track of bytes already consumed

The cursor is therefore not adding seekability to TCP. It is adding
**synchronization** between host and device.

That synchronization is what turns host retries and implementation mistakes
into detectable protocol errors instead of silent byte-stream corruption.

---

## 5. Why the Overhead Feels Unpleasant on 8-Bit Hosts

The downside is real.

On a small host, tracking a 32-bit cursor means:

- four-byte arithmetic for every read and write progression
- four-byte state storage per stream direction
- more complexity than a simple "give me the next chunk" API

That overhead can feel disproportionate when the host is only consuming a
sequential TCP stream.

This is especially noticeable when compared with older firmware designs that
behaved more like:

- "read next bytes"
- "write next bytes"

with no explicit cursor discipline on the host side.

Those older designs are simpler for clients, but they move ambiguity into the
device boundary. They rely on the assumption that commands are never replayed,
never reordered, and never repeated with stale state. Once those assumptions
are violated, the protocol has little or no ability to detect desynchronization.

The cursor-based design chooses explicit host complexity in exchange for a much
stronger correctness contract.

---

## 6. Why This Was Enforced for TCP Specifically

TCP is where the risk is easiest to see because it is a long-lived,
bidirectional, non-seekable stream.

For a file-like object, an offset naturally means a position in stored data.
For TCP, there is no stored object to reposition within.

If the NetworkDevice protocol had introduced a separate command family such as:

- `ReadNext(handle, len)`
- `WriteNext(handle, data)`

then TCP could have used those semantics directly.

But v1 already standardizes on requests that include offsets. Since the wire
format was not being extended, TCP had to assign those offsets a meaning that
preserves safety and determinism.

That meaning is the stream cursor.

---

## 7. Relationship to HTTP and Other Streaming Protocols

This is related to HTTP in the sense that HTTP response bodies are also often
consumed as forward-only streams.

The important point is not "HTTP repositioning" but rather this:

- the NetworkDevice API is offset-based
- some protocols are seekable
- some protocols are not seekable
- non-seekable protocols still need a defined interpretation of the offset field

Using a monotonic cursor provides one consistent answer for streamed data:

- the offset is not where to seek
- the offset is how much has already been consumed or accepted

TCP is simply the clearest example because actual repositioning is impossible.

---

## 8. What the Current Implementation Does

The TCP backend keeps two 32-bit counters per open handle:

- `read_cursor`: total bytes delivered to the host
- `write_cursor`: total bytes accepted for sending

For reads:

- the request offset must equal `read_cursor`
- if data is returned, `read_cursor` advances by the number of bytes delivered

For writes:

- the request offset must equal `write_cursor`
- if bytes are accepted, `write_cursor` advances by the number of bytes sent

If the provided offset does not match the expected cursor, the backend returns:

    StatusCode::InvalidRequest

This is intentionally strict. It makes stream misuse visible immediately.

---

## 9. Rollover Concern

The concern about long-lived connections is valid.

A 32-bit unsigned cursor wraps after 4 GiB. On a permanent or very long-lived
TCP connection, that can eventually happen.

This does **not** mean the cursor concept is unnecessary. It means the current
wire format has a finite sequence space.

In practice, rollover introduces these concerns:

- both host and device must treat the cursor consistently as a 32-bit value
- both sides must remain in lockstep across wraparound
- implementations that think of the value as an unbounded absolute byte count
  may mis-handle wrap

So rollover is a limitation of the chosen field width, not a reason the cursor
exists.

The cursor solves a correctness problem. The 32-bit size constrains how long
that correctness model can run before sequence numbers wrap.

---

## 10. Design Tradeoff

The tradeoff is straightforward:

### Simpler host API, no cursor tracking

Advantages:

- easier for 8-bit clients
- less host-side state
- less arithmetic overhead

Disadvantages:

- duplicate commands may duplicate writes silently
- duplicate reads cannot be distinguished from legitimate next reads
- out-of-order commands become silent corruption rather than detectable errors
- partial-write recovery is less precise
- host/device synchronization is implicit and fragile

### Cursor-based host API

Advantages:

- duplicate and stale commands are detected
- read and write progress is explicit
- partial progress can be retried deterministically
- non-seekable streams fit safely into an offset-based protocol

Disadvantages:

- 8-bit clients must maintain 32-bit counters
- the API feels heavier than a simple sequential stream interface
- rollover must eventually be considered on very long-lived connections

The current v1 TCP design deliberately chooses the second set of tradeoffs.

---

## 11. Bottom Line

The cursor is not there to give TCP file-like seek behavior.

It is there because the NetworkDevice v1 protocol is offset-based, and TCP is
not seekable. A monotonic cursor is the mechanism that turns the offset field
into a safe synchronization contract between host and device.

Without that contract, the device would have to ignore offsets, and then stale,
duplicated, or out-of-order requests could not be reliably detected. In a TCP
stream, that would lead to silent read desynchronization, duplicated writes, or
other hard-to-debug corruption.

So the cost to the host is real, but it buys something important: explicit,
deterministic stream state in a protocol that was originally built around
offset-bearing requests.
