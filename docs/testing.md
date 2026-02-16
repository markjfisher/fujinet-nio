# FujiNet-NIO Testing Guide

This document describes how to test the FujiNet-NIO firmware and the fujinet-nio-lib 6502 library.

## Prerequisites

- FujiNet-NIO firmware built and flashed to ESP32
- Serial connection to FujiNet device (USB CDC or UART)
- Docker (for running test services)
- Python 3.10+ with required packages

## Test Services

The test services provide HTTP, HTTPS, TCP, and TLS endpoints for testing.

### Starting Services

```bash
# Start all services (HTTP, HTTPS, TCP, TLS)
./scripts/start_test_services.sh all

# Start individual services
./scripts/start_test_services.sh http   # HTTP only (port 8080)
./scripts/start_test_services.sh https  # HTTPS proxy (port 8443)
./scripts/start_test_services.sh tcp    # TCP echo (port 7777)
./scripts/start_test_services.sh tls    # TLS echo (port 7778)

# Check status
./scripts/start_test_services.sh status

# Stop all services
./scripts/start_test_services.sh stop
```

### Service Endpoints

| Service | Port | Description |
|---------|------|-------------|
| HTTP | 8080 | httpbin service for HTTP testing |
| HTTPS | 8443 | nginx reverse proxy with TLS |
| TCP | 7777 | socat echo server |
| TLS | 7778 | nginx stream proxy with TLS |

### Network Access

Services bind to all interfaces (0.0.0.0) so they're accessible from the FujiNet device on your local network. The script displays both localhost and network URLs:

```
Starting httpbin:
  Local:   http://localhost:8080
  Network: http://192.168.1.101:8080
```

Use the **Network** URL when testing from FujiNet.

## Integration Tests

The integration test runner executes YAML-defined test steps against the FujiNet device.

### Running Tests

```bash
# Run all tests (ESP32 mode)
integration-tests/run_integration.py --port /dev/ttyUSB0 --esp32 --ip 192.168.1.101 --fs sd0

# List available tests
integration-tests/run_integration.py --port /dev/ttyUSB0 --esp32 --ip 192.168.1.101 --fs sd0 --list

# Run specific test file
integration-tests/run_integration.py --port /dev/ttyUSB0 --esp32 --ip 192.168.1.101 --fs sd0 --only-file 00_http.yaml

# Run specific test group
integration-tests/run_integration.py --port /dev/ttyUSB0 --esp32 --ip 192.168.1.101 --fs sd0 --only-group http

# Show output on success
integration-tests/run_integration.py --port /dev/ttyUSB0 --esp32 --ip 192.168.1.101 --fs sd0 --show-output
```

### Test Files

| File | Group | Description |
|------|-------|-------------|
| `00_http.yaml` | http | HTTP/HTTPS GET, POST, streaming |
| `01_http_request_headers.yaml` | http | Request header handling |
| `03_http_streaming_body.yaml` | http | Large body streaming |
| `10_tcp.yaml` | tcp | TCP echo tests |
| `11_tls.yaml` | tls | TLS echo tests with test CA |
| `20_clock.yaml` | clock | Clock device tests |
| `30_filesystem.yaml` | fs | Filesystem operations |
| `40_disk_raw.yaml` | disk | Raw disk image tests |
| `41_disk_ssd.yaml` | disk | SSD disk image tests |

## Manual Testing with CLI

The `scripts/fujinet` CLI tool provides direct access to FujiNet commands.

### Network Commands

```bash
# HTTP GET
./scripts/fujinet -p /dev/ttyUSB0 net get "http://192.168.1.101:8080/get"

# HTTPS with test CA (self-signed cert)
./scripts/fujinet -p /dev/ttyUSB0 net get "https://192.168.1.101:8443/get?testca=1"

# Open connection
./scripts/fujinet -p /dev/ttyUSB0 net open "http://192.168.1.101:8080/get"

# Write data
./scripts/fujinet -p /dev/ttyUSB0 net write 0 --data "Hello World"

# Read data
./scripts/fujinet -p /dev/ttyUSB0 net read 0 --max-bytes 1024

# Close connection
./scripts/fujinet -p /dev/ttyUSB0 net close 0
```

### TLS Testing

```bash
# Open TLS connection with test CA
./scripts/fujinet -p /dev/ttyUSB0 net open "tls://192.168.1.101:7778?testca=1"

# Write data
./scripts/fujinet -p /dev/ttyUSB0 net write 0 --data "Hello TLS!"

# Read echo response
./scripts/fujinet -p /dev/ttyUSB0 net read 0 --max-bytes 64

# Close connection
./scripts/fujinet -p /dev/ttyUSB0 net close 0
```

### Filesystem Commands

```bash
# List files
./scripts/fujinet -p /dev/ttyUSB0 list sd0 /

# Read file
./scripts/fujinet -p /dev/ttyUSB0 read sd0 /test.txt

# Write file
./scripts/fujinet -p /dev/ttyUSB0 write sd0 /test.txt /tmp/test.txt

# Get file info
./scripts/fujinet -p /dev/ttyUSB0 stat sd0 /test.txt
```

## Self-Signed Certificates

For HTTPS and TLS testing, we use self-signed certificates generated with the FujiNet Test CA.

### Certificate Location

```
integration-tests/certs/
  ca.crt      - CA certificate (embedded in firmware)
  ca.key      - CA private key
  server.crt  - Server certificate (for nginx)
  server.key  - Server private key (for nginx)
```

### Using Test CA

Add `?testca=1` to HTTPS URLs or use `tls://` URLs with the flag:

```bash
# HTTPS with test CA
./scripts/fujinet -p /dev/ttyUSB0 net get "https://192.168.1.101:8443/get?testca=1"

# TLS with test CA
./scripts/fujinet -p /dev/ttyUSB0 net open "tls://192.168.1.101:7778?testca=1"
```

### Regenerating Certificates

If certificates expire (valid for 10 years):

```bash
./integration-tests/certs/generate_certs.sh

# Then update the embedded CA in firmware
# Copy ca.crt contents to include/fujinet/platform/esp32/test_ca_cert.h
# Rebuild firmware
```

## Testing fujinet-nio-lib

The 6502 library can be tested on Linux using the native transport.

### Building

```bash
cd ../fujinet-nio-lib
make TARGET=linux
```

### Running Examples

```bash
# HTTP GET
FN_TEST_URL="http://192.168.1.101:8080/get" FN_PORT=/dev/ttyUSB0 ./bin/linux/http_get

# HTTPS with test CA
FN_TEST_URL="https://192.168.1.101:8443/get?testca=1" FN_PORT=/dev/ttyUSB0 ./bin/linux/http_get
```

## Troubleshooting

### Connection Refused

1. Check services are running: `./scripts/start_test_services.sh status`
2. Check firewall allows ports 8080, 8443, 7777, 7778
3. Use network IP (not localhost) for FujiNet tests

### Certificate Errors

1. Ensure firmware has the embedded test CA
2. Use `?testca=1` flag in URLs
3. Check certificates haven't expired

### Timeout Errors

1. Check FujiNet is connected and responsive
2. Check serial port is correct
3. Try with `--timeout 30` for slower connections

### Debug Mode

Enable debug output:

```bash
./scripts/fujinet -p /dev/ttyUSB0 --debug net get "http://192.168.1.101:8080/get"
```

## CI/CD Integration

For automated testing:

```bash
# Start services in background
./scripts/start_test_services.sh all &
sleep 5

# Run tests
integration-tests/run_integration.py --port /dev/ttyUSB0 --esp32 --ip $HOST_IP --fs sd0

# Cleanup
./scripts/start_test_services.sh stop