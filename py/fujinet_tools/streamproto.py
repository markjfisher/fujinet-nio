"""Experimental Atari small-stream payload codec (not FujiBus framing).

>>> request(1, 2, 3, b'ab').hex()
'01030100000002000000026162'
>>> query(bytes.fromhex('01780100000002000000'))
{'version': 1, 'maximum': 120, 'token': 1, 'sequence': 2}
"""
import struct

OPERATIONS = {'open': 1, 'read': 2, 'write': 3, 'close': 4, 'appkey': 5}

def request(token: int, sequence: int, operation: int, data: bytes = b'', length: int = 0) -> bytes:
    if not 0 < token <= 0xffffffff or not 0 <= sequence <= 0xffffffff:
        raise ValueError('token/sequence out of range')
    if operation not in OPERATIONS.values():
        raise ValueError('unknown operation')
    if operation in (1, 3):
        length = len(data)
        if not 1 <= length <= 120:
            raise ValueError('payload must be 1..120 bytes')
        if operation == 1 and (not data.startswith(b'tcp://') or b'\0' in data):
            raise ValueError('open requires a tcp:// URL without NUL')
    elif data:
        raise ValueError('operation has no data payload')
    elif operation == 2 and not 1 <= length <= 120:
        raise ValueError('read capacity must be 1..120')
    elif operation == 4 and length:
        raise ValueError('close length must be zero')
    elif operation == 5 and not 0 <= length <= 255:
        raise ValueError('key must be 0..255')
    return struct.pack('<BBIIB', 1, operation, token, sequence, length) + data

def query(data: bytes) -> dict:
    if len(data) != 10:
        raise ValueError('query must contain 10 bytes')
    version, maximum, token, sequence = struct.unpack('<BBII', data)
    if version != 1 or maximum != 120 or not token:
        raise ValueError('unsupported capabilities')
    return dict(version=version, maximum=maximum, token=token, sequence=sequence)

def response(data: bytes, token: int, sequence: int, operation: int, capacity: int) -> dict:
    if len(data) < 11:
        raise ValueError('truncated response')
    status, flags, actual, echoed_token, echoed_sequence = struct.unpack('<BBBII', data[:11])
    if echoed_token != token or echoed_sequence != sequence:
        raise ValueError('response belongs to another transaction')
    if status > 8 or actual > capacity or (status and actual):
        raise ValueError('invalid result metadata')
    count = actual if operation in (2, 5) else 0
    if len(data) < 11 + count:
        raise ValueError('truncated data')
    return dict(status=status, flags=flags, actual=actual, data=data[11:11+count].hex())

def register_subcommands(sub):
    p = sub.add_parser('stream-wire', help='encode/decode experimental Atari stream payloads (hex; no I/O)')
    commands = p.add_subparsers(dest='action', required=True)
    q = commands.add_parser('query', help='decode ten-byte capability reply')
    q.add_argument('hex')
    q.set_defaults(fn=lambda a: print(query(bytes.fromhex(a.hex))))
    r = commands.add_parser('request')
    r.add_argument('operation', choices=OPERATIONS)
    r.add_argument('--token', type=int, required=True)
    r.add_argument('--sequence', type=int, required=True)
    r.add_argument('--data-hex', default='')
    r.add_argument('--length', type=int, default=0)
    r.set_defaults(fn=lambda a: print(request(a.token, a.sequence, OPERATIONS[a.operation],
                                             bytes.fromhex(a.data_hex), a.length).hex()))
    r = commands.add_parser('response')
    r.add_argument('hex')
    r.add_argument('operation', choices=OPERATIONS)
    r.add_argument('--token', type=int, required=True)
    r.add_argument('--sequence', type=int, required=True)
    r.add_argument('--capacity', type=int, default=120)
    r.set_defaults(fn=lambda a: print(response(bytes.fromhex(a.hex), a.token, a.sequence,
                                              OPERATIONS[a.operation], a.capacity)))
