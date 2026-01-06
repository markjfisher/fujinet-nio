# Diagnostics & Console

FujiNet-NIO includes a small diagnostic framework in the **core library** plus an optional, shell-like console UI in the **apps**.

Design goals:
- Keep diagnostics **platform-agnostic** and reusable by embedders.
- Keep the console UI **app-only** and platform I/O in `src/platform/*`.
- Avoid `#ifdef` inside core services/devices.

---

## Diagnostic framework (core library)

Public headers live under `include/fujinet/diag/`:
- `diagnostic_types.h`: `DiagResult`, `DiagStatus`, `DiagCommandSpec`, `DiagArgsView`
- `diagnostic_provider.h`: `IDiagnosticProvider` (and the built-in core provider factory)
- `diagnostic_registry.h`: `DiagnosticRegistry`

### Provider model

Providers publish a stable command set and execute commands:
- A provider has a stable `provider_id()` (e.g. `core`, `net`, `fs`).
- Commands are plain strings with a convention of `provider.command` (e.g. `core.stats`).
- Results are returned as a stable `DiagResult`:
  - `text`: human-readable, line-oriented text
  - `kv`: optional key/value pairs for tooling

### Registry model

`DiagnosticRegistry` is a simple aggregator/dispatcher:
- The registry stores pointers to providers (providers own their own state).
- `list_all_commands(out)` gathers `DiagCommandSpec` from all providers.
- `dispatch(args)` tries providers in registration order; providers return `NotFound` when they don’t handle a command.

### Built-in provider: `core`

The built-in provider is created with:
- `fujinet::diag::create_core_diagnostic_provider(core::FujinetCore&)`

It currently exposes:
- `core.info` — version + build profile
- `core.stats` — tick count + registered device count

---

## Console engine (app-only)

The console engine is pure parsing/dispatch logic:
- Header: `include/fujinet/console/console_engine.h`
- Implementation: `src/app/console_engine.cpp`

It depends on:
- a `diag::DiagnosticRegistry&`
- an `IConsoleTransport&` that provides line I/O with timeouts

### Console commands

- `help`
- `exit` / `quit`
- `diag list`
- `diag get <provider> [<key_or_subpath>]`
  - Example: `diag get core stats` → dispatches `core.stats`
- `diag dump`

### Prompt + echo

Many PTY/serial clients do not echo locally. The console prints a simple prompt:

```
> 
```

The console does **not** currently echo your input characters; use a client with local echo if you want to see what you type.

---

## Platform transports (platform glue)

Platform I/O implementations live in `src/platform/<posix|esp32>/` and are selected by `create_default_console_transport()`.

### POSIX

POSIX defaults to a **dedicated console PTY**, separate from the FujiBus PTY used for normal traffic.

At startup you should see two different PTYs printed:
- Console:
  - `[Console] PTY created. Connect diagnostic console to: /dev/pts/<N>`
- FujiBus (normal traffic):
  - `[PtyChannel] Created PTY. Connect to slave: /dev/pts/<M>`

Important: **Do not connect your FujiBus tooling to the console PTY** (and vice-versa).

#### POSIX: choosing console transport (PTY vs stdio)

- Default: PTY console
- Force stdio console (stdin/stdout of the process):

```bash
FN_CONSOLE=stdio ./fujinet-nio
```

#### POSIX: connecting with picocom (recommended)

Example (replace with appropriate port from the app startup):

```bash
picocom -q --echo --omap crlf --imap lfcrlf /dev/pts/8
```

Notes:
- Replace `/dev/pts/8` with the **console PTY** printed by `[Console] ...`, not the FujiBus PTY.
- `--omap crlf` / `--imap lfcrlf` help keep output aligned and make Enter behave naturally.

#### POSIX: connecting with screen (works, but less friendly)

```bash
screen /dev/pts/<N>
```

To exit:
- Detach: `Ctrl-A` then `d`
- Quit: `Ctrl-A` then `\` then `y`

### ESP32

On ESP32 builds, FujiBus commonly uses **USB CDC** for normal traffic (BuildProfile: `UsbCdcDevice`).

Console transport note:
- The console must **not share** the same USB CDC stream as FujiBus, or it will corrupt the wire protocol.
- The console transport is configurable via **sdkconfig/Kconfig** (see below).

Typical usage:
- Use PlatformIO/ESP-IDF serial monitor (UART0) to interact with the console.
- The console prompt and commands are the same as POSIX.

Future direction:
- Use **two TinyUSB CDC ACM ports** (multi-CDC) so the host sees two separate `/dev/ttyACM*` devices:
  - FujiBus on one port (default: ACM0)
  - Console on a different port (default: ACM1)

#### ESP32: configuration (sdkconfig)

FujiNet-NIO provides Kconfig options under `menuconfig` → **FujiNet-NIO**:

- `CONFIG_FN_CONSOLE_ENABLE`
  - Enable/disable the diagnostic console in the ESP32 app.
- `CONFIG_FN_CONSOLE_TRANSPORT_UART0` / `CONFIG_FN_CONSOLE_TRANSPORT_USB_CDC`
  - Select whether console runs on UART0 or TinyUSB CDC ACM.
- `CONFIG_FN_FUJIBUS_USB_CDC_PORT`
  - Which TinyUSB CDC ACM port FujiBus uses (default: `0`).
- `CONFIG_FN_CONSOLE_USB_CDC_PORT`
  - Which TinyUSB CDC ACM port the console uses (default: `1`).

To expose **two** CDC ACM ports from TinyUSB, set:
- `CONFIG_TINYUSB_CDC_ENABLED=y`
- `CONFIG_TINYUSB_CDC_COUNT=2`

Notes:
- If `CONFIG_TINYUSB_CDC_COUNT<2`, a USB-CDC console cannot be dedicated; FujiNet-NIO will fall back to UART0 when configured for USB CDC.
- The exact host node numbering (`/dev/ttyACM0`, `/dev/ttyACM1`, `/dev/ttyACM2`) is not guaranteed to be stable across replug/boot; consider udev rules for stable symlinks if needed.


