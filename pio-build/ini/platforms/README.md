# platform ini files

This directory contains the platform-specific ini files for the Fujinet firmware.

## Naming convention

The files are named as follows:

```
platformio-<channel>-<transport>-<board>.ini
```

Where:

- `<channel>` is the channel name, e.g. `cdc`, `sio`, etc.
- `<transport>` is the transport name, e.g. `fujibus`, `legacy`, etc.
- `<board>` is the board name, e.g. `s3-wroom-1-n16r8`, etc.
