# UART Turnaround Pacing

FujiBus over RS-232 is a half-duplex transaction pattern carried over a
full-duplex UART:

1. The host sends one SLIP-framed FujiBus request.
2. FujiNet decodes and handles that request.
3. FujiNet sends one SLIP-framed FujiBus response.
4. The host receives and validates that response.

On a fast machine this can happen many times per second. The transport normally
does not need extra delay. However, some host drivers need a small turnaround
window after finishing their transmit path before they are reliably polling the
UART receive path.

## `tx_gap_us`

`channel.uart.tx_gap_us` is an optional ESP32-side delay, in microseconds, before
FujiNet writes a response frame to the UART.

Default:

```yaml
channel:
  uart:
    tx_gap_us: 0
```

`0` means no extra pacing.

The value can also be changed live through diagnostics:

```text
uart.status
uart.set tx_gap_us 250
uart.save
```

The setting is part of the generic UART config, but it is primarily useful for
ESP32 FujiBus GPIO/RS-232 builds.

## What Problem It Solves

The problematic timing looks like this:

```text
host                       FujiNet
----                       -------
send request  ---------->
finish TX
switch to RX polling
                         decode request
                         send response immediately
receive response <-------
```

If FujiNet begins the response before the host receive path is ready, the host
can miss the first byte or first few bytes of the response frame.

For SLIP-framed FujiBus packets, the first byte is normally `C0`, the SLIP frame
delimiter. Losing that byte can be enough for a strict receiver to discard the
rest of an otherwise valid response while trying to synchronize to the next
frame. On the MS-DOS driver this can appear as a timeout with no decoded bytes,
for example:

```text
DRV err=10 st=7 rx=0 exp=6 lsr=00
```

That means the driver timed out waiting for the 6-byte FujiBus response header,
decoded zero bytes, and saw no UART line-status error.

`tx_gap_us` delays FujiNet's response very slightly so the host has time to
finish transmitting, return from its transmit path, and enter receive polling.

## What It Does Not Solve

`tx_gap_us` does not fix arbitrary byte loss.

If the host UART or driver loses bytes in the middle of a response, the packet
should still fail length, checksum, device, or command validation. In that case
the correct fix is in UART reliability, receive-loop performance, FIFO handling,
flow control, or response-size pacing.

This setting is specifically for turnaround timing: the boundary between the
host request and the FujiNet response.

## Typical Values

Start with no pacing:

```text
uart.set tx_gap_us 0
```

If logs show valid FujiNet responses being sent but the host reports receive
timeouts with zero decoded bytes, try:

```text
uart.set tx_gap_us 250
```

Then test progressively:

```text
uart.set tx_gap_us 500
uart.set tx_gap_us 1000
```

Values above 1000 microseconds should be treated as diagnostic. They may still
be useful for proving a timing problem, but they add visible latency to every
FujiBus response.

## Baud Rate Considerations

UART byte time depends on baud rate. With normal 8N1 framing, each byte takes
10 bit times:

```text
byte_time_us = 10,000,000 / baud_rate
```

Approximate byte times:

| Baud rate | One byte | 250 us | 500 us | 1000 us |
| --- | ---: | ---: | ---: | ---: |
| 115200 | 87 us | 2.9 bytes | 5.8 bytes | 11.5 bytes |
| 57600 | 174 us | 1.4 bytes | 2.9 bytes | 5.8 bytes |
| 38400 | 260 us | 1.0 bytes | 1.9 bytes | 3.8 bytes |
| 19200 | 521 us | 0.5 bytes | 1.0 bytes | 1.9 bytes |
| 9600 | 1042 us | 0.2 bytes | 0.5 bytes | 1.0 bytes |

At 115200 baud, a 250-500 us gap is only a few character times. That is often
enough to cover a host-side transmit-to-receive transition without materially
changing throughput.

At lower baud rates, the same microsecond gap represents fewer byte times. The
host is also receiving data more slowly, so the receive loop has more time once
the frame begins. For turnaround issues, lower baud rates may need less pacing,
not more.

## How To Evaluate It

Use host logs and FujiNet logs together.

Evidence that `tx_gap_us` may help:

- FujiNet logs show it received the request.
- FujiNet logs show it sent a valid response immediately.
- The host reports a timeout with `rx=0`, or repeated duplicate requests.
- There is no UART line-status error such as overrun.

Evidence that another fix is needed:

- The host reports UART overrun, framing, or parity errors.
- The host decodes a short or checksum-failing frame.
- Failures happen in the middle of large responses.
- Raising `tx_gap_us` does not change the failure rate.

For stress testing, keep the application workload constant, change only
`tx_gap_us`, and compare run duration plus the exact driver error. A useful test
sequence is:

```text
0 us -> 250 us -> 500 us -> 1000 us
```

If a small value removes `rx=0` response-header timeouts, keep the smallest value
that is stable. If only very large values help, treat that as evidence of a
deeper receive-side problem rather than a final tuning answer.
