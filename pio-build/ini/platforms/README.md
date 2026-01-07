# platform ini files

This directory contains the platform-specific ini files for the Fujinet firmware.

## Naming convention

The files are named as follows:

```
platformio-<transport>-<channel>-<console>-<board>.ini
```

Where:

- `<transport>` is the transport/wire-protocol name, e.g. `fujibus`, `legacy`, etc.
- `<channel>` is the host<->device channel/link, e.g. `usbcdc`, `sio`, `gpio`, etc.
- `<console>` is the console transport choice: `consolecdc`, `consoleuart`, or `noconsole`.
- `<board>` is the board name, e.g. `s3-wroom-1-n16r8`, etc.

NOTE: Only fujibus transport currently works with usbcdc. The legacy/sio is just documented example but will not build yet.
