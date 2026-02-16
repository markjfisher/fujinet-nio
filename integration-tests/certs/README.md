# FujiNet Test Certificates

This directory contains self-signed certificates for HTTPS/TLS testing with FujiNet.

## Files

| File | Purpose |
|------|---------|
| `ca.crt` | CA certificate - embedded in firmware for verification |
| `ca.key` | CA private key - keep secure, used to sign server certs |
| `server.crt` | Server certificate - used by nginx HTTPS proxy |
| `server.key` | Server private key - used by nginx HTTPS proxy |
| `generate_certs.sh` | Script to regenerate all certificates |

## How It Works

1. **CA Certificate (`ca.crt`)** is embedded in the ESP32 firmware at:
   - [`include/fujinet/platform/esp32/test_ca_cert.h`](../../include/fujinet/platform/esp32/test_ca_cert.h)

2. **Server Certificate (`server.crt`)** is used by the nginx HTTPS proxy:
   - [`scripts/start_test_services.sh`](../../scripts/start_test_services.sh) copies these to `/tmp/fujinet-https-certs/`

3. **Firmware uses the embedded CA** when URL contains `?testca=1`:
   - `https://192.168.1.101:8443/get?testca=1`

## Regenerating Certificates

If you need to regenerate the certificates (e.g., they expired):

```bash
./generate_certs.sh
```

Then update the firmware:
1. Copy the new `ca.crt` contents to `include/fujinet/platform/esp32/test_ca_cert.h`
2. Rebuild the ESP32 firmware

## Certificate Details

- **Validity**: 10 years (2026-2036)
- **CA CN**: `FujiNet Test CA`
- **Server CN**: `localhost`
- **Supported SANs**:
  - DNS: `localhost`, `*.local`
  - IP: `127.0.0.1`, `192.168.1.1-3`, `10.0.0.1`, `172.17.0.1`

## Testing

1. Start the HTTPS proxy:
   ```bash
   ./scripts/start_test_services.sh https
   ```

2. Test with curl (uses system CA, so need `-k` for self-signed):
   ```bash
   curl -k https://localhost:8443/get
   ```

3. Test with FujiNet (uses embedded test CA):
   ```bash
   FN_TEST_URL="https://192.168.1.101:8443/get?testca=1" FN_PORT=/dev/ttyUSB0 ./bin/linux/http_get
   ```

## Security Note

These certificates are **ONLY FOR TESTING**. They are:
- Self-signed (not trusted by any system)
- Shared publicly in this repository
- Not intended for production use

For production HTTPS, use certificates from a trusted CA (Let's Encrypt, etc.).