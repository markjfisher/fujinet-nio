# platform ini files

This directory contains the platform-specific ini files for the Fujinet firmware.

## Naming convention

The files are named as follows:

```
platformio-<transport>-<channel>-<board>.ini
```

Where:

- `<transport>` is the transport/wire-protocol name, e.g. `fujibus`, `legacy`, etc.
- `<channel>` is the host<->device channel/link, e.g. `usbcdc`, `sio`, `gpio`, etc.
- `<board>` is the board name, e.g. `s3-wroom-1-n16r8`, etc.


console is an orthognal choice and must be set in "platformio.local.ini" for the console transport choice from: `consolecdc`, `consoleuart`, or `noconsole`.

```ini
[fujinet]
; ... other values
console_type = consolecdc
```

This can be set with the build script, or manually. 