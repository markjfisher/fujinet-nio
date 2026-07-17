# POSIX TCP Serial Channel

This document explains the POSIX TCP serial channel used by emulator and QEMU
workflows.

The short version:

- `fujibus-tcp-debug` and `fujibus-tcp-release` build FujiNet-NIO as a POSIX app.
- The app listens as a TCP server.
- The emulator or QEMU process connects as a TCP client.
- The bytes on that socket are FujiBus packets framed with SLIP.
- Disk, file, network, clock, and config behavior still lives in normal
  `VirtualDevice` implementations.

## Layering

The TCP channel sits at the same layer as PTY, USB CDC, and UART channels:

```text
QEMU / emulator TCP client
        |
        v
TcpServerChannel             src/platform/posix/channel_factory.cpp
        |
        v
FujiBusTransport             src/lib/fujibus_transport.cpp
        |
        v
IOService / IODeviceManager
        |
        v
VirtualDevice instances      DiskDevice, FileDevice, NetworkDevice, FujiDevice
```

`TcpServerChannel` implements only the raw `Channel` API:

```cpp
bool available();
std::size_t read(std::uint8_t* buffer, std::size_t maxLen);
void write(const std::uint8_t* buffer, std::size_t len);
```

It does not parse FujiBus packets, does not know about devices, and does not
special-case MS-DOS. `FujiBusTransport` owns SLIP framing and FujiBus packet
conversion to `IORequest` / `IOResponse`.

## Build profile

The TCP serial workflow uses:

```text
FN_BUILD_FUJIBUS_TCP
machine          = Machine::Generic
primaryTransport = TransportKind::FujiBus
primaryChannel   = ChannelKind::TcpSocket
name             = "POSIX + FujiBus over TCP serial"
```

Build with:

```bash
cmake --preset fujibus-tcp-debug
cmake --build --preset fujibus-tcp-debug-build
```

Run:

```bash
./build/fujibus-tcp-debug/fujinet-nio
```

Expected startup includes:

```text
Build profile: POSIX + FujiBus over TCP serial
[ChannelFactory] Using TCP server channel (TcpSocket) on 127.0.0.1:65504
[TcpServerChannel] Listening on 127.0.0.1:65504
```

## Configuration

The channel reads `channel.tcp_host` and `channel.tcp_port` from
`fujinet.yaml`:

```yaml
channel:
  tcp_host: "127.0.0.1"
  tcp_port: 65504
```

Defaults:

- `tcp_host`: `127.0.0.1`
- `tcp_port`: `65504`

Bind to `127.0.0.1` for local QEMU/emulator testing. Binding to `0.0.0.0`
allows remote clients to connect, but the TCP channel is a raw serial byte
stream and has no authentication or encryption.

## Runtime behavior

`TcpServerChannel`:

- resolves and binds the configured host/port
- listens with backlog 1
- keeps the listening socket nonblocking
- accepts one active client
- uses nonblocking reads and writes
- closes the active client on EOF, hangup, or socket error
- returns to listening after client disconnect

If no client is connected, `available()` is false and writes are dropped. This
matches the rest of the `Channel` contract: transports should not block waiting
for a physical link.

## QEMU serial usage

QEMU should connect as the TCP client. A typical serial chardev shape is:

```bash
qemu-system-i386 \
  -chardev socket,id=fujinet,host=127.0.0.1,port=65504,reconnect-ms=1000 \
  -serial chardev:fujinet
```

Start `fujinet-nio` first so the TCP server is listening before QEMU connects.
With `reconnect-ms`, QEMU can also survive a FujiNet-NIO restart.

## MS-DOS disk image workflow

The QEMU boot disk and the NIO-exposed disk are separate concerns:

- The QEMU boot disk can be `qcow2` and contains MS-DOS plus `FUJINET.SYS`.
- FujiNet-NIO does not mount the qcow image.
- DiskService mounts a separate raw/FAT image through `StorageManager`.
- The MS-DOS driver talks to `DiskDevice` over FujiBus and sees that raw image
  as a sector device.

Example NIO config for a DOS/FAT raw image:

```yaml
mounts:
  - slot: 1
    uri: "host:/dos/fn-dos.img"
    mode: "rw"
    enabled: true
channel:
  tcp_host: "127.0.0.1"
  tcp_port: 65504
```

Raw DOS/FAT images with a valid FAT BPB can be mounted without a sector-size
hint. Use `sector_size_hint` only for headerless raw images or ambiguous files
whose geometry cannot be detected from content.

## Why this is not legacy FujiNet transport

The TCP serial channel carries clean NIO FujiBus traffic. It is not the legacy
Atari SIO-style FujiNet protocol and does not install the legacy network
adapter. Legacy routing is only enabled for legacy transports such as SIO/IWM.

For MS-DOS integration, the driver should speak NIO FujiBus+SLIP commands to
NIO service device IDs such as `DiskService`, not the legacy FujiNet protocol.
