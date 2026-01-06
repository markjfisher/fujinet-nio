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
- `<provider> <command> [args...]`
  - Example: `core stats` → dispatches `core.stats`
  - Example: `net close 0x0102` → dispatches `net.close 0x0102`
- `<provider>.<command> [args...]`
  - Example: `core.stats`
- `list`
- `dump`

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

#### Linux: stable `/dev/` names with udev (recommended)

When you expose multiple serial interfaces (e.g. USB-Serial-JTAG + TinyUSB CDC ACM0 + TinyUSB CDC ACM1),
Linux may assign `/dev/ttyACM*` numbers differently across boots or re-plugs.

To make tooling reliable, create stable symlinks like:
- `/dev/fujinet-fujibus`
- `/dev/fujinet-console`

##### 1) Inspect the attributes for each port

Plug the device in, then for each candidate port (example: `/dev/ttyACM1`, `/dev/ttyACM2`):

```bash
❯ udevadm info -n /dev/ttyACM1 -q property | grep -E 'ID_VENDOR_ID|ID_MODEL_ID|ID_USB_INTERFACE_NUM'
ID_MODEL_ID=4002
ID_VENDOR_ID=303a
ID_USB_INTERFACE_NUM=00

❯ udevadm info -n /dev/ttyACM2 -q property | grep -E 'ID_VENDOR_ID|ID_MODEL_ID|ID_USB_INTERFACE_NUM'
ID_MODEL_ID=4002
ID_VENDOR_ID=303a
ID_USB_INTERFACE_NUM=02
```

Tip: quick view via sysfs for the ID_USB_INTERFACE_NUM, it equates to the following path:

```bash
cat /sys/class/tty/ttyACM1/device/bInterfaceNumber
```

For multi-CDC, a common pattern is:
- CDC ACM0 → interface number `00`
- CDC ACM1 → interface number `02`

…but **verify on your system** using the commands above.

Alternate way to check the values is via journalctl logs:
```
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: new full-speed USB device number 49 using xhci_hcd
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: New USB device found, idVendor=1a86, idProduct=55d3, bcdDevice= 4.45
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: New USB device strings: Mfr=0, Product=2, SerialNumber=3
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: Product: USB Single Serial
Jan 06 19:03:29 archy kernel: usb 7-1.3.3: SerialNumber: 5875011611
Jan 06 19:03:29 archy kernel: cdc_acm 7-1.3.3:1.0: ttyACM0: USB ACM device
```

##### 2) Create udev rules

Create a rules file:

```bash
sudoedit /etc/udev/rules.d/99-fujinet-nio.rules
```

Template (replace VID/PID and interface numbers with what you observed):

```udev
# FujiNet-NIO (TinyUSB CDC ACM)
# Replace ID_VENDOR_ID/ID_MODEL_ID and ID_USB_INTERFACE_NUM to match your device.

ACTION=="add", SUBSYSTEM=="tty", KERNEL=="ttyACM*", \
  ENV{ID_VENDOR_ID}=="303a", ENV{ID_MODEL_ID}=="4002", ENV{ID_USB_INTERFACE_NUM}=="00", \
  SYMLINK+="fujinet-fujibus"

ACTION=="add", SUBSYSTEM=="tty", KERNEL=="ttyACM*", \
  ENV{ID_VENDOR_ID}=="303a", ENV{ID_MODEL_ID}=="4002", ENV{ID_USB_INTERFACE_NUM}=="02", \
  SYMLINK+="fujinet-console"
```

Notes:
- The example `303a` VID is Espressif; your PID may differ depending on descriptor settings.
- `ID_USB_INTERFACE_NUM` matching is usually the most reliable way to separate ACM0 vs ACM1 on the same USB device.

##### 3) Reload rules

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger --subsytem-match=tty
```

Then unplug/replug the device, and verify:

```bash
ls -l /dev/fujinet-*
```


