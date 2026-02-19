from __future__ import annotations

import datetime

from .fujibus import FujiBusSession
from . import fileproto as fp
from .common import open_serial, status_ok

# -----------------------
# clock commands
# -----------------------
CLOCK_DEVICE_ID = 0x45
CLOCK_CMD_GET = 0x01
CLOCK_CMD_SET = 0x02
CLOCK_CMD_GET_FORMAT = 0x03
CLOCK_CMD_GET_TIMEZONE = 0x04
CLOCK_CMD_SET_TIMEZONE = 0x05
CLOCK_CMD_SET_TIMEZONE_SAVE = 0x06
CLOCKPROTO_VERSION = 1

# Time format codes (must match TimeFormat enum in clock_commands.h)
TIME_FORMAT_SIMPLE = 0x00
TIME_FORMAT_PRODOS = 0x01
TIME_FORMAT_APETIME = 0x02
TIME_FORMAT_TZ_ISO = 0x03
TIME_FORMAT_UTC_ISO = 0x04
TIME_FORMAT_SOS = 0x05

# Format name to code mapping
FORMAT_MAP = {
    'simple': TIME_FORMAT_SIMPLE,
    'prodos': TIME_FORMAT_PRODOS,
    'apetime': TIME_FORMAT_APETIME,
    'iso-tz': TIME_FORMAT_TZ_ISO,
    'iso-utc': TIME_FORMAT_UTC_ISO,
    'sos': TIME_FORMAT_SOS,
}


def _build_clock_get_req() -> bytes:
    return bytes([CLOCKPROTO_VERSION])


def _build_clock_set_req(unix_seconds: int) -> bytes:
    if unix_seconds < 0:
        unix_seconds = 0
    unix_seconds &= (1 << 64) - 1
    b = bytearray()
    b.append(CLOCKPROTO_VERSION)
    for i in range(8):
        b.append((unix_seconds >> (8 * i)) & 0xFF)
    return bytes(b)


def _build_clock_get_format_req(format_code: int, timezone: str | None = None) -> bytes:
    """Build GetTimeFormat request.
    
    Payload:
        u8 version
        u8 format
        u8 tz_len (0 if no timezone override)
        char[] timezone (optional, only if tz_len > 0)
    """
    b = bytearray()
    b.append(CLOCKPROTO_VERSION)
    b.append(format_code & 0xFF)
    
    if timezone:
        tz_bytes = timezone.encode('utf-8')
        b.append(len(tz_bytes))
        b.extend(tz_bytes)
    else:
        b.append(0)  # no timezone override
    
    return bytes(b)


def _build_clock_set_timezone_req(timezone: str) -> bytes:
    """Build SetTimezone or SetTimezoneSave request.
    
    Payload:
        u8 version
        u8 tz_len
        char[] timezone
    """
    tz_bytes = timezone.encode('utf-8')
    b = bytearray()
    b.append(CLOCKPROTO_VERSION)
    b.append(len(tz_bytes))
    b.extend(tz_bytes)
    return bytes(b)


def _parse_clock_time_resp(payload: bytes) -> int:
    # u8 version, u8 flags, u16 reserved, u64 unixSeconds
    if len(payload) < 1 + 1 + 2 + 8:
        raise ValueError(f"clock response too short ({len(payload)} bytes)")
    ver = payload[0]
    if ver != CLOCKPROTO_VERSION:
        raise ValueError(f"clock version mismatch: got {ver}, want {CLOCKPROTO_VERSION}")
    # flags = payload[1]
    # reserved = payload[2:4]
    ts, _ = fp.read_u64le(payload, 4)
    return ts


def _parse_clock_format_resp(payload: bytes) -> tuple[int, bytes]:
    """Parse GetTimeFormat response.
    
    Returns: (format_code, formatted_data)
    """
    if len(payload) < 2:
        raise ValueError(f"clock format response too short ({len(payload)} bytes)")
    ver = payload[0]
    if ver != CLOCKPROTO_VERSION:
        raise ValueError(f"clock version mismatch: got {ver}, want {CLOCKPROTO_VERSION}")
    format_code = payload[1]
    data = payload[2:]
    return format_code, data


def _parse_clock_timezone_resp(payload: bytes) -> str:
    """Parse GetTimezone/SetTimezone response.
    
    Returns: timezone string
    """
    if len(payload) < 2:
        raise ValueError(f"clock timezone response too short ({len(payload)} bytes)")
    ver = payload[0]
    if ver != CLOCKPROTO_VERSION:
        raise ValueError(f"clock version mismatch: got {ver}, want {CLOCKPROTO_VERSION}")
    tz_len = payload[1]
    if len(payload) < 2 + tz_len:
        raise ValueError(f"clock timezone response truncated (expected {tz_len} bytes)")
    return payload[2:2+tz_len].decode('utf-8').rstrip('\x00')


def _format_hex_bytes(data: bytes) -> str:
    """Format bytes as hex string like 'AA BB CC DD'."""
    return ' '.join(f'{b:02X}' for b in data)


def cmd_clock_get(args) -> int:
    req = _build_clock_get_req()

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=CLOCK_DEVICE_ID,
            command=CLOCK_CMD_GET,
            payload=req,
            expect_device=CLOCK_DEVICE_ID,
            expect_command=CLOCK_CMD_GET,
            timeout=args.timeout,
            cmd_txt="CLOCK_GET",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'}")
            return 1

        try:
            ts = _parse_clock_time_resp(pkt.payload)
        except Exception as e:
            print(f"Bad clock response: {e}")
            return 1

        print(f"device unix: {ts}")
        print(f"device utc : {fp.fmt_utc(ts):>20}")
    return 0


def cmd_clock_set(args) -> int:
    if args.unix is not None:
        ts = int(args.unix)
    else:
        # local machine time -> UTC epoch seconds
        ts = int(datetime.datetime.now(tz=datetime.timezone.utc).timestamp())

    req = _build_clock_set_req(ts)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=CLOCK_DEVICE_ID,
            command=CLOCK_CMD_SET,
            payload=req,
            expect_device=CLOCK_DEVICE_ID,
            expect_command=CLOCK_CMD_SET,
            timeout=args.timeout,
            cmd_txt="CLOCK_SET",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'}")
            return 1

        try:
            echoed = _parse_clock_time_resp(pkt.payload)
        except Exception as e:
            print(f"Bad clock response: {e}")
            return 1

        print(f"set unix : {ts}")
        print(f"echo unix: {echoed}")
        print(f"utc      : {fp.fmt_utc(echoed):>20}")
    return 0


def cmd_clock_get_format(args) -> int:
    """Get time in a specific format."""
    format_name = args.format.lower()
    if format_name not in FORMAT_MAP:
        print(f"Unknown format: {args.format}")
        print(f"Valid formats: {', '.join(FORMAT_MAP.keys())}")
        return 1
    
    format_code = FORMAT_MAP[format_name]
    req = _build_clock_get_format_req(format_code)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=CLOCK_DEVICE_ID,
            command=CLOCK_CMD_GET_FORMAT,
            payload=req,
            expect_device=CLOCK_DEVICE_ID,
            expect_command=CLOCK_CMD_GET_FORMAT,
            timeout=args.timeout,
            cmd_txt="CLOCK_GET_FORMAT",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'}")
            return 1

        try:
            resp_format, data = _parse_clock_format_resp(pkt.payload)
        except Exception as e:
            print(f"Bad clock format response: {e}")
            return 1

        # Format output based on format type
        if format_code in (TIME_FORMAT_SIMPLE, TIME_FORMAT_PRODOS, TIME_FORMAT_APETIME, TIME_FORMAT_SOS):
            # Binary formats - display as hex bytes
            print(f"{format_name}: {_format_hex_bytes(data)}")
        else:
            # String formats - display as string
            print(data.decode('utf-8').rstrip('\x00'))
    
    return 0


def cmd_clock_get_tz(args) -> int:
    """Get time in a specific format with a specific timezone."""
    format_name = args.format.lower()
    if format_name not in FORMAT_MAP:
        print(f"Unknown format: {args.format}")
        print(f"Valid formats: {', '.join(FORMAT_MAP.keys())}")
        return 1
    
    format_code = FORMAT_MAP[format_name]
    req = _build_clock_get_format_req(format_code, args.timezone)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=CLOCK_DEVICE_ID,
            command=CLOCK_CMD_GET_FORMAT,
            payload=req,
            expect_device=CLOCK_DEVICE_ID,
            expect_command=CLOCK_CMD_GET_FORMAT,
            timeout=args.timeout,
            cmd_txt="CLOCK_GET_FORMAT",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'}")
            return 1

        try:
            resp_format, data = _parse_clock_format_resp(pkt.payload)
        except Exception as e:
            print(f"Bad clock format response: {e}")
            return 1

        # Format output based on format type
        if format_code in (TIME_FORMAT_SIMPLE, TIME_FORMAT_PRODOS, TIME_FORMAT_APETIME, TIME_FORMAT_SOS):
            # Binary formats - display as hex bytes
            print(f"{format_name}: {_format_hex_bytes(data)}")
        else:
            # String formats - display as string
            print(data.decode('utf-8').rstrip('\x00'))
    
    return 0


def cmd_clock_get_timezone(args) -> int:
    """Get the current timezone."""
    req = bytes([CLOCKPROTO_VERSION])

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=CLOCK_DEVICE_ID,
            command=CLOCK_CMD_GET_TIMEZONE,
            payload=req,
            expect_device=CLOCK_DEVICE_ID,
            expect_command=CLOCK_CMD_GET_TIMEZONE,
            timeout=args.timeout,
            cmd_txt="CLOCK_GET_TIMEZONE",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'}")
            return 1

        try:
            tz = _parse_clock_timezone_resp(pkt.payload)
        except Exception as e:
            print(f"Bad clock timezone response: {e}")
            return 1

        print(f"timezone: {tz}")
    
    return 0


def cmd_clock_set_timezone(args) -> int:
    """Set the timezone (non-persistent)."""
    req = _build_clock_set_timezone_req(args.timezone)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=CLOCK_DEVICE_ID,
            command=CLOCK_CMD_SET_TIMEZONE,
            payload=req,
            expect_device=CLOCK_DEVICE_ID,
            expect_command=CLOCK_CMD_SET_TIMEZONE,
            timeout=args.timeout,
            cmd_txt="CLOCK_SET_TIMEZONE",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'}")
            return 1

        print("ok")
    
    return 0


def cmd_clock_set_timezone_save(args) -> int:
    """Set the timezone and persist to config."""
    req = _build_clock_set_timezone_req(args.timezone)

    with open_serial(args.port, args.baud, timeout_s=0.01) as ser:
        bus = FujiBusSession().attach(ser, debug=args.debug)

        pkt = bus.send_command_expect(
            device=CLOCK_DEVICE_ID,
            command=CLOCK_CMD_SET_TIMEZONE_SAVE,
            payload=req,
            expect_device=CLOCK_DEVICE_ID,
            expect_command=CLOCK_CMD_SET_TIMEZONE_SAVE,
            timeout=args.timeout,
            cmd_txt="CLOCK_SET_TIMEZONE_SAVE",
        )
        if pkt is None:
            print("No response")
            return 2
        if not status_ok(pkt):
            print(f"Device status={pkt.params[0] if pkt.params else '??'}")
            return 1

        print("ok (saved)")
    
    return 0


def register_subcommands(subparsers) -> None:
    """Register ClockDevice commands under `clock`."""
    pc = subparsers.add_parser("clock", help="Clock device commands")
    csub = pc.add_subparsers(dest="clock_cmd", required=True)

    # get: Get raw Unix time
    pcg = csub.add_parser("get", help="Get device time as Unix timestamp",
                          description="Get the current time from the FujiNet device as a Unix timestamp (seconds since 1970-01-01)")
    pcg.set_defaults(fn=cmd_clock_get)

    # set: Set device time
    pcs = csub.add_parser("set", help="Set device time from this machine (UTC now)",
                          description="Set the FujiNet device's real-time clock. Defaults to current UTC time from this machine.")
    pcs.add_argument("--unix", type=int, help="Override: set explicit unix seconds")
    pcs.set_defaults(fn=cmd_clock_set)

    # get-format: Get time in a specific format
    pcgf = csub.add_parser("get-format", help="Get time in a specific format using device's current timezone",
                           description="""Get the current time in a specific format, using the device's current timezone setting.

Formats:
  simple   - 7 bytes: [century, year, month, day, hour, min, sec] (hex)
  prodos   - 4 bytes: Apple ProDOS format (hex)
  apetime  - 6 bytes: Atari ApeTime format [day, month, year, hour, min, sec] (hex)
  iso-tz   - ISO 8601 string with timezone offset: YYYY-MM-DDTHH:MM:SS+HHMM
  iso-utc  - ISO 8601 string always in UTC: YYYY-MM-DDTHH:MM:SS+0000
  sos      - 16 bytes: Apple III SOS format (hex)""")
    pcgf.add_argument("format", help="Format name: simple, prodos, apetime, iso-tz, iso-utc, sos")
    pcgf.set_defaults(fn=cmd_clock_get_format)

    # get-tz: Get time with a specific timezone
    pcgt = csub.add_parser("get-tz", help="Get time in a specific timezone and format",
                           description="""Get the current time converted to a specific timezone and format.

The timezone parameter is a POSIX timezone string like:
  UTC              - UTC (no offset)
  UTC+3            - UTC minus 3 hours (note: sign is inverted in POSIX)
  EST5EDT,M3.2.0,M11.1.0 - US Eastern with DST

Formats:
  simple   - 7 bytes: [century, year, month, day, hour, min, sec] (hex)
  prodos   - 4 bytes: Apple ProDOS format (hex)
  apetime  - 6 bytes: Atari ApeTime format [day, month, year, hour, min, sec] (hex)
  iso-tz   - ISO 8601 string with the specified timezone offset
  iso-utc  - ISO 8601 string always in UTC (timezone param only affects time value, not offset)
  sos      - 16 bytes: Apple III SOS format (hex)

Note: 'iso-utc' always shows +0000 offset but the time value is converted to the specified timezone.
      'iso-tz' shows the actual timezone offset in the string.""")
    pcgt.add_argument("timezone", help="POSIX timezone string (e.g., 'UTC', 'EST5EDT,M3.2.0,M11.1.0', 'UTC+3')")
    pcgt.add_argument("format", help="Format name: simple, prodos, apetime, iso-tz, iso-utc, sos")
    pcgt.set_defaults(fn=cmd_clock_get_tz)

    # get-timezone: Get current timezone
    pcgtz = csub.add_parser("get-timezone", help="Get the device's current timezone setting",
                            description="Get the current timezone string from the FujiNet device. This is the timezone used for 'iso-tz' format output.")
    pcgtz.set_defaults(fn=cmd_clock_get_timezone)

    # set-timezone: Set timezone (non-persistent)
    pcst = csub.add_parser("set-timezone", help="Set timezone for this session only (not saved)",
                           description="""Set the device's timezone without persisting to config.

The timezone will be used for subsequent 'get-format iso-tz' calls but will be lost on reboot.

POSIX timezone examples:
  UTC              - UTC (no offset)
  UTC+3            - UTC minus 3 hours (note: sign is inverted in POSIX)
  EST5EDT,M3.2.0,M11.1.0 - US Eastern with DST (March 2nd Sunday, November 1st Sunday)
  CET-1CEST,M3.5.0,M10.5.0/3 - Central European with DST""")
    pcst.add_argument("timezone", help="POSIX timezone string")
    pcst.set_defaults(fn=cmd_clock_set_timezone)

    # set-timezone-save: Set timezone and persist to config
    pcsts = csub.add_parser("set-timezone-save", help="Set timezone and save to config (persistent)",
                            description="""Set the device's timezone and persist it to the config file.

The timezone will survive reboots and be loaded on next startup.

POSIX timezone examples:
  UTC              - UTC (no offset)
  UTC+3            - UTC minus 3 hours (note: sign is inverted in POSIX)
  EST5EDT,M3.2.0,M11.1.0 - US Eastern with DST (March 2nd Sunday, November 1st Sunday)
  CET-1CEST,M3.5.0,M10.5.0/3 - Central European with DST""")
    pcsts.add_argument("timezone", help="POSIX timezone string")
    pcsts.set_defaults(fn=cmd_clock_set_timezone_save)
