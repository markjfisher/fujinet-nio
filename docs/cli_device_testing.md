# Command-Line Device Testing (HTTP & TCP)

This document describes how to manually test a running **fujinet-nio**
instance from the command line using the Python tooling in `py/fujinet_tools`
and local test servers for HTTP and TCP.

The goal is to make **interactive debugging and validation** easy during
development, without writing new firmware or test code.

---

## 1. Prerequisites

### Local environment
- Python 3.9+
- `pyserial`
- Docker (for test services)

### FujiNet tooling
All commands below use the `fujinet` CLI entry point provided by:

```
py/fujinet_tools/cli.py
```

Typical invocation:

```
fujinet --port /dev/ttyACM0 net ...
```

Adjust `--port`, `--baud`, and `--debug` as required for your setup.

---

## 2. Starting local test services (HTTP + TCP)

The repository provides a consolidated helper script:

```
scripts/start_test_services.sh
```

This script runs Docker containers for both HTTP and TCP testing.

### Start HTTP test server (httpbin)

```
./scripts/start_test_services.sh http
```

- Exposes: `http://localhost:8080`
- Useful for testing:
  - GET / POST / PUT
  - Headers
  - Chunked responses
  - Error codes

### Start TCP echo server

```
./scripts/start_test_services.sh tcp
```

- Exposes: `tcp://localhost:7777`
- Anything sent is echoed back
- Traffic is logged to the terminal (`socat -v -v`)

### Start both services

```
./scripts/start_test_services.sh both
```

- HTTP runs detached
- TCP echo runs in foreground with live traffic logs

### Stop all test services

```
./scripts/start_test_services.sh stop
```

### Check status

```
./scripts/start_test_services.sh status
```

---

## 3. HTTP testing from the CLI

### Simple GET

```
fujinet --port /dev/ttyACM0 net get http://localhost:8080/get
```

### POST with body

```
fujinet --port /dev/ttyACM0 net send \
  --method POST \
  --data "hello world" \
  http://localhost:8080/post
```

### Inspect headers and status

```
fujinet --port /dev/ttyACM0 net head http://localhost:8080/headers
```

These commands exercise:
- HTTP request lifecycle
- Body streaming via `Write`
- Response reading via `Read`
- Metadata via `Info`

---

## 4. TCP testing with the REPL

The TCP backend is stream-oriented and non-seekable. For this reason, a
dedicated **interactive REPL** is provided.

### Start a TCP REPL

```
fujinet --port /dev/ttyACM0 net tcp repl \
  --wait-connected \
  --show-info \
  tcp://<HOST_IP>:7777
```

Notes:
- Use your host’s LAN IP (not `127.0.0.1`) when testing from ESP32.
- `--wait-connected` polls `Info()` until the TCP connection is established.
- `--show-info` prints TCP pseudo headers on connect.

### REPL commands

Inside the REPL:

```
help                     Show available commands
send <text>              Send UTF-8 text
sendhex <aabbcc>         Send raw bytes (hex)
recv [n]                 Read up to n bytes
drain [n]                Keep reading until idle or n bytes
info                     Show Info() pseudo headers
offsets                  Show read/write cursors
halfclose                Send zero-length write (TX shutdown)
close                    Close the handle
quit / exit              Exit the REPL
```

### Example session

```
tcp> send hello
[tcp] sent 5 bytes

tcp> recv 32
[tcp] 5 bytes: b'hello'
hex: 68656c6c6f

tcp> offsets
read_offset=5 write_offset=5

tcp> info
X-FujiNet-Scheme: tcp
X-FujiNet-Connected: 1
X-FujiNet-RxAvailable: 0
```

This validates:
- Sequential offsets
- Stream send/receive
- Pseudo headers via `Info`
- Non-blocking `NotReady` behavior

---

## 5. Common testing patterns

### Validate sequential offset enforcement
- Send bytes
- Attempt a write with an old offset
- Confirm the device returns `InvalidRequest`

### Validate backpressure
- Send large data in small write chunks
- Observe `DeviceBusy` retries in debug output

### Validate EOF handling
- Use a server that sends data then closes
- Confirm `Read` returns `eof=1` after buffer drains

Example one-shot server:

```
while true; do
  { echo -ne "hello from server\r\n"; } | nc -l -p 7777 -q 1
done
```

---

## 6. Notes on TCP vs HTTP

- HTTP is request/response oriented
- TCP is a bidirectional stream
- Both use the same v1 commands:
  - Open / Write / Read / Info / Close
- For TCP:
  - Offsets are monotonic cursors
  - `Info()` uses pseudo headers
  - No HTTP status or content length

See:
- `docs/network_device_protocol.md`
- `docs/network_device_tcp.md`

---

## 7. Troubleshooting

### TCP connect never completes
- Check IP address (ESP32 cannot reach `127.0.0.1`)
- Ensure Docker port is published (`-p 7777:7777`)
- Use `Info()` to inspect `X-FujiNet-Connecting`

### Reads return NotReady
- This is normal for streams
- Retry or use `drain`

### USB/CDC appears “stuck”
- Enable `--debug` to see SLIP framing
- Restart the REPL and reconnect

---

## 8. Related documentation

- `docs/developer_onboarding.md`
- `docs/network_device_protocol.md`
- `docs/network_device_tcp.md`

This document is intended to evolve alongside the Python tooling and
NetworkDevice protocol extensions.
