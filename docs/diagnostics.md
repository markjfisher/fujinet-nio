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
- The current implementation uses **UART0** for the diagnostic console (via ESP-IDF UART driver), because a dedicated second CDC interface/port has not been added yet.

Typical usage:
- Use PlatformIO/ESP-IDF serial monitor (UART0) to interact with the console.
- The console prompt and commands are the same as POSIX.

Future direction:
- Add a second CDC ACM interface (separate endpoint) for the console, then switch the ESP32 console transport to USB CDC safely.


