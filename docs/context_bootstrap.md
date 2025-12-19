# BOOTSTRAP SECTIONS


## NETWORK


I’m working on fujinet-nio.
We’ve signed off HTTP verbs + body lifecycle; next is TODO item 4: TCP backend (stream sockets).

Repo refs:
- TODO: https://raw.githubusercontent.com/markjfisher/fujinet-nio/refs/heads/master/todo/network.md
- NetworkDevice binary protocol spec (v1): https://raw.githubusercontent.com/markjfisher/fujinet-nio/refs/heads/master/docs/network_device_protocol.md
- (Possibly relevant overview): https://github.com/markjfisher/fujinet-nio/raw/refs/heads/master/docs/protocol_reference.md
- Architecture overview of fujinet-nio: https://github.com/markjfisher/fujinet-nio/raw/refs/heads/master/docs/architecture.md

Please design the TCP scheme/backend semantics that fit the existing v1 NetworkDevice protocol (Open/Read/Write/Info/Close), for both POSIX and ESP32.

Concretely:
1) URL form(s): tcp://host:port (and any query options like connect timeout, nodelay, etc).
2) How Open/Write/Read map to socket connect/send/recv, including:
   - sequential offset rules (still required?) for stream sockets
   - what “Read(handle, offset, maxBytes)” means when a TCP stream has no natural offsets
3) What Info() should return for TCP in v1 (given current Info payload format is HTTP-ish):
   - do we extend flags/fields, or define TCP-only meaning for http_status/content_length/headers?
   - how do we expose “connected/alive”, “bytes available”, and “last error” without breaking v1?
4) StatusCode mapping matrix for TCP (InvalidRequest/NotReady/DeviceBusy/IOError/etc) that matches the TODO contract style.
5) A minimal implementation plan:
   - POSIX sockets backend structure + timeouts/nonblocking strategy
   - ESP32 lwIP sockets backend structure + RX buffering/backpressure
   - tests we should add (platform-agnostic session tests vs backend parity)
Please answer with a crisp set of rules + recommended implementation approach.


Project: fujinet-nio NetworkDevice

Context:
- HTTP protocol is complete and tested
- Open / Read / Write / Info / Close are stable
- POSIX and ESP32 implementations aligned

Docs:
- Protocol specs:
  https://github.com/markjfisher/fujinet-nio/raw/refs/heads/master/docs/protocol_reference.md
  https://github.com/markjfisher/fujinet-nio/raw/refs/heads/master/docs/network_device_protocol.md
- Network TODO:
  https://github.com/markjfisher/fujinet-nio/raw/refs/heads/master/todo/network.md
- Architecture:
  https://github.com/markjfisher/fujinet-nio/raw/refs/heads/master/docs/architecture.md

Goal:
Design TCP protocol support that fits the existing NetworkDevice model.

Please focus on:
- protocol design first (no code yet)
- compatibility with v1
- test strategy


