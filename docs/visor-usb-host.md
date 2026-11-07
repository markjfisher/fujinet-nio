# Handspring Visor USB Host

The optional POSIX USB Host profile connects NIO directly to a Handspring Visor
USB cradle. It uses libusb on Linux and macOS and reuses the existing FujiBus
codecs, routing, NetworkService and ClockService. It is independent of the
RP2350 Springboard bridge: a USB cradle request does not pass through RP2350.

The supported device identity is **082d:0100**, tested on a Visor Prism. This is
a Handspring vendor-specific interface, not CDC or general Palm-branded USB
support. Linux has passed minimum normal-use hardware acceptance. The macOS
implementation uses the same source; compilation and runtime remain unverified
on both Intel and Apple Silicon.

## Build

Use the normal [POSIX development environment](developer_onboarding.md), C++20,
CMake/Ninja and pkg-config, plus **libusb-1.0 >= 1.0.25** development files.
On macOS use architecture-matched libusb/pkg-config and a C++20 compiler; no
Homebrew prefix is hardcoded.

```sh
cmake --preset fujibus-visor-usb-debug
cmake --build build/fujibus-visor-usb-debug -j6
ctest --test-dir build/fujibus-visor-usb-debug --output-on-failure
```

Initialize the project's submodules as described in the main README for a fresh
checkout. Python test dependencies are separate from libusb; an existing Python
environment can be selected with `-DPython3_EXECUTABLE=/path/to/venv/bin/python`.

`FN_BUILD_VISOR_USB_HOST` defaults OFF. Only this opt-in profile discovers and
links libusb or compiles Visor sources. Ordinary PTY, serial and PacketLink
builds need no libusb headers/library. The profile excludes other transport
profiles, embedded-library mode, Windows and shared-line HotSync arbitration.
See [build profiles](build_profiles.md) for the general selection model.

## Run

Install a USB-capable Palm NioLib from `fujinet-nio-lib`
(ABI 1.3 or later, USB capability `0x20`) and a client application. In NIO Config,
select **USB Cradle**, Save and return Home. End HotSync before running the host:

```sh
FN_CONSOLE=stdio FN_VISOR_USB_PORT=2 \
  ./build/fujibus-visor-usb-debug/fujinet-nio
```

The host waits for the Palm application to open its USB connection; pressing
HotSync is not required. Run NioLibTest's Clock + two HTTP test, or a compatible
application. The console commands `visor.status` and `net.sessions` show USB
state and network handles. Enter `kill` to exit through normal core shutdown.
For a local log, redirect stdout/stderr to a private file; URLs/location data
may occur in debug logs. Do not publish raw application traces.

### Selection settings and console

Device selection is loaded from `fujinet.yaml`. Environment variables override
individual fields after loading; invalid configured or overridden values stop
startup rather than silently choosing defaults or another device.

```yaml
visor:
  port: 2
  # bus: 1
  # path: "9.4"
```

| YAML setting | Environment override | Default | Valid values |
| --- | --- | --- | --- |
| `visor.port` | `FN_VISOR_USB_PORT` | `2` | Generic port `1` or `2` |
| `visor.bus` | `FN_VISOR_USB_BUS` | unset | Decimal `1..255`; omit or use YAML `null` to unset |
| `visor.path` | `FN_VISOR_USB_PATH` | unset | 1–7 dot-separated decimal components, each `1..255`; requires bus |

Omit `path` or use YAML `null` to unset it. Empty environment overrides are errors.
The path is a physical port chain, not the changing USB device address. Invalid
Visor settings are not replaced with backup/default settings or rewritten by the
YAML loader; unrelated configuration recovery behavior is unchanged.

The diagnostic console supports:

```text
visor.get
visor.set port 2
visor.set bus 1
visor.set path 9.4
visor.save
visor.status
```

Use `visor.set path unset`, then `visor.set bus unset` to remove a selector.
`visor.get` shows configured values. `visor.status` shows the effective startup
selection and the source of each value (`config` or `environment`), plus the
active USB session. `visor.set` validates before changing configuration, and
`visor.save` saves configured values and verifies them by reading back. Environment
overrides are never copied into the saved settings. **Restart the host to apply
selection changes**, even if it has not connected yet. An active session is not
retargeted by console edits.

The standard POSIX PTY console is the default, or select it with `FN_CONSOLE=pty`:

```sh
FN_CONSOLE=pty ./build/fujibus-visor-usb-debug/fujinet-nio
# In another terminal, use the console path printed at startup:
picocom /dev/pts/N
```

`FN_CONSOLE=stdio` remains supported. The console PTY is separate from the USB
cradle and does not claim it. A Linux automated test exercises the real PTY and console engine while the
mock USB backend routes a Clock request. PTY use with an attached Prism still
needs physical verification; recorded USB acceptance used stdio.

Multiple matching devices require an unambiguous selector. An absent device is
not an error requiring restart; discovery is limited to once per second.

### Interface ownership

The backend never detaches kernel drivers, changes permissions, installs udev
rules, resets a USB device or initiates HotSync. It reports ACCESS/BUSY failures.
On Linux, the `visor` kernel driver may own the interface after enumeration.
Stop conflicting HotSync programs and have the operator explicitly arrange
access/release for the verified cradle interface. Do not unload modules or
change global permissions as an automatic startup step. On macOS, exclusive
access failures are reported without forced detachment or legacy kernel drivers.
Do not run two backends against the same interface.

Pre-session setup failures (including kernel ownership, a busy claim or missing/
invalid Generic connection information) release acquired resources and retry at
most once per second. They do not quarantine the device: releasing the driver or
opening the Palm port can allow the same enumerated device to connect. Every
retry repeats descriptor/function validation; it never sends or replays a NIO
command. This recovery is mock-tested, not yet physically qualified on the Prism.

For initial use, finish HotSync and arrange interface access, start the host,
then open a Palm NIO client. The previous acceptance sessions used coordinated
access release and stdio; they did not qualify recovery from an initial setup
failure. If the Palm opening causes fresh enumeration, operator-controlled access
may need to be arranged for the new instance.

## Discovery and packet handling

Discovery validates active configuration 1, alternate 0, a unique vendor-class
bulk IN/OUT pair with valid full-speed MPS, and the selected Generic function.
GET_INTERFACE verifies the active alternate after claim; the host does not change
configuration or alternate settings. Old connection-info request `0x03` is
required and validated. Extended request `0x04` is optional; its stall on the
Prism is accepted. HotSync is never substituted for a missing Generic function.
Request `0x01` is not a packet-length query.

On the tested Prism, Generic 2 is OUT `0x02` / IN `0x82`, MPS 64; Generic 1 is
OUT `0x01` / IN `0x81`, MPS 16. Endpoint descriptors and function information,
not those constants alone, determine the selected pair. Each IN transfer requests
**one actual descriptor MPS**. Larger reads can wait on exact packet-size
multiples. USB short packets and ZLPs do not delimit application records.

Packets use the existing canonical format:

```text
u8 device | u8 command | u16 total_length_le | u8 checksum | u8 descriptor | ...
```

The **1024-byte limit includes the complete packet**, including status and
service metadata. No SLIP, PacketLink envelope or session field is added.
`RawPackets` preserves fragmented and aggregated input in a bounded queue of
four complete packets plus one partial packet. The existing `FujiBusPacket`
codec validates complete records. Invalid lengths, checksums, descriptors,
overflow or a five-second partial-record deadline retire the session. Corrupted
input is never scanned for a plausible replacement header.

## Scheduling, ownership and failures

POSIX Ingress composes `IPacketIO`, `NativeFramer` and `FujiBusTransport`; common
services remain transport-independent. libusb callbacks update private state
only. The cooperative core thread performs routing and owner-scoped
`resetClient(NetworkService, owner)` cleanup. Other clients' handles are retained.
See [architecture](architecture.md) and [driver boundaries](driver_architecture.md).

There is one asynchronous IN and one OUT transfer, with 250 ms transfer limits,
a four-packet owned OUT queue and a five-second total OUT deadline. Successful
short OUT completions send only the unsent suffix. Ambiguous transfer errors
retire the session and never replay executed requests. Buffers remain owned
until cancellation callbacks complete. Shutdown drains callbacks for up to two
seconds; an exceptional OS cancellation failure deliberately retains USB-owned
storage until process exit instead of freeing live buffers.

Each physical connection gets a private generation and owner. Retired callbacks,
packets and responses cannot cross into a later generation. A device faulted after session activation is
quarantined until discovery confirms its absence; a local buffer reset cannot
establish a fresh physical session. Close/Reopen the Palm connection only as
part of diagnosed recovery, not an automatic retry loop. Generations do not
prove that already delivered Palm bytes were flushed.

Idle readiness is false when no packet or actionable lifecycle work is pending;
bounded waits pump libusb events for at most 10 ms. A staged packet or lifecycle
change wakes core processing. Outgoing NativeFramer rejection, including an
oversized response, retires the physical generation even if no USB send occurred.
Requests are never replayed or replies truncated to hide a send failure.
Synchronous service work can still block the core; this does not make Palm
applications asynchronous. Setup control queries have 250 ms limits each;
OS enumeration/claim time is outside that USB timeout budget.

## Tests and qualification

`tests/visor_usb/` compiles the real driver, ingress and protocol code against
libusb mocks, with no live USB access. Coverage includes descriptor/function
selection, partial/combined records, corruption, deadlines, cancellation, stale
generations, idle scheduling, response-framing rejection, real Clock routing,
two network handles and owner isolation. Discovery and record/output deadline tests
inject a monotonic clock into the real host/ingress code rather than sleeping;
production uses `steady_clock`. Tests cover the instant before and at the
five-second deadlines. Cancellation shutdown retains its real two-second bound,
independent of the injected operational clock. Existing POSIX codec/service/serial
and PacketLink regressions remain applicable.

For dependency isolation, also build an ordinary profile and inspect both:

```sh
cmake --preset fujibus-link-serial-debug
cmake --build build/fujibus-link-serial-debug -j6
ctest --test-dir build/fujibus-link-serial-debug --output-on-failure
python3 tests/visor_usb/audit_builds.py \
  build/fujibus-visor-usb-debug build/fujibus-link-serial-debug
python3 tests/visor_usb/test_generation.py
```

Linux enabled/disabled builds, mock tests and relevant POSIX regressions have
passed. Physical Prism acceptance passed USB Clock + two HTTP requests,
ISS → Weather → ISS, then Springboard Clock + two HTTP after switching transport.
Post-audit fixes passed the shorter USB/CS1 regression; logs ended with no active
network handles. The later cached-JSON NioLib/apps were separately reported
working with faster app loading, without a new complete transport qualification
sequence or timing benchmark. No RP2350 firmware/ROM change was required for USB.
The later setup-retry and YAML/console changes are automatically verified only.
For their next short physical check, use the PTY console, start the host before
opening a Palm NIO client, inspect `visor.status`, and run Clock + two HTTP once.
If checking kernel-driver release recovery, release only the verified interface
with explicit operator approval; the same device should connect on a subsequent
discovery without restarting the host. No PRC or Springboard firmware update is
needed for these host-only changes.
macOS, serial hardware, arbitrary physical failure recovery and long-duration
heap/stress qualification remain unverified.

## Source boundaries and future embedded reuse

- Portable: `include/fujinet/io/visor_usb/protocol.h` and
  `src/lib/visor_usb/protocol.cpp` provide `connection_info()`,
  `select_generic_pair()` and `RawPackets`, using the existing packet codec.
  They contain no libusb, POSIX or ESP-IDF types and are selected only where used.
- POSIX: `src/platform/posix/visor_usb/{host,ingress}.cpp` owns discovery,
  event pumping, transfers, generations, routing ownership and diagnostics.
- Build source of truth: `build-artifacts/CMakeLists_posix.cmake` and
  `scripts/update_cmake_sources.py --posix-only`; do not edit generated source
  lists directly or generate unrelated platform/diagram outputs.

A future ESP32-S3 USB Host client may reuse the portable helpers and FujiBus
codec, replacing libusb/event/buffer handling with ESP-IDF code and platform-local
owner cleanup. It must supply VBUS, host-role/PHY and transfer ownership support.
Springboard-to-RP2350 and direct-cradle Host are **mutually exclusive hardware
configurations**, not simultaneous links. ESP32-S3 USB Host is not implemented
or qualified; no hub or automatic role switching is required.
