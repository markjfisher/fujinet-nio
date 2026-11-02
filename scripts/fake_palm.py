#!/usr/bin/env python3
"""A stand-in Palm that HotSyncs with FujiNet NIO over network HotSync (NetSync).

It answers the DLP requests a sync makes, keeps its user and databases in a
JSON file between runs, and prints what happened. Use it to exercise the
HotSync service (docs/hotsync.md) without a Palm.

    scripts/fake_palm.py localhost                 # sync once
    scripts/fake_palm.py 192.168.1.252 --state palm.json --verbose

A new state file holds one database, "FujiNet Notes", with its backup bit
set, so a sync with `hotsync.backup: flagged` backs it up. Apps FujiNet
installs are stored in the state file and listed afterwards.
"""

import argparse
import json
import os
import socket
import struct
import sys

# DLP function codes (Palm OS SDK DLCommon.h).
F = {
    "ReadUserInfo": 0x10, "WriteUserInfo": 0x11, "ReadSysInfo": 0x12,
    "GetSysDateTime": 0x13, "SetSysDateTime": 0x14, "ReadDBList": 0x16,
    "OpenDB": 0x17, "CreateDB": 0x18, "CloseDB": 0x19, "DeleteDB": 0x1A,
    "ReadAppBlock": 0x1B, "WriteAppBlock": 0x1C, "ReadSortBlock": 0x1D,
    "WriteSortBlock": 0x1E, "ReadRecord": 0x20, "WriteRecord": 0x21,
    "DeleteRecord": 0x22, "ReadResource": 0x23, "WriteResource": 0x24,
    "ResetSystem": 0x29, "AddSyncLogEntry": 0x2A, "ReadOpenDBInfo": 0x2B,
    "OpenConduit": 0x2E, "EndOfSync": 0x2F,
}
NAMES = {v: k for k, v in F.items()}
ERR_NOT_FOUND = 5
DB_RESOURCE = 0x0001
DB_BACKUP = 0x0008


class Link:
    """NetSync framing: type 1, xid, u32 length, payload."""

    def __init__(self, sock):
        self.sock = sock
        self.xid = 0

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("FujiNet closed the connection")
            buf += chunk
        return buf

    def recv(self):
        _type, xid, length = struct.unpack(">BBI", self.recv_exact(6))
        self.xid = xid
        return self.recv_exact(length)

    def send(self, payload):
        self.sock.sendall(struct.pack(">BBI", 1, self.xid, len(payload)) + payload)


def parse_args(body):
    """DLP arguments by index (argument ID minus 0x20)."""
    argc = body[1]
    pos, args = 2, {}
    for _ in range(argc):
        aid = body[pos]
        if aid & 0x80:      # short form
            length = struct.unpack(">H", body[pos + 2:pos + 4])[0]
            pos += 4
        elif aid & 0x40:    # long form
            length = struct.unpack(">I", body[pos + 2:pos + 6])[0]
            pos += 6
        else:
            length = body[pos + 1]
            pos += 2
        args[(aid & 0x3F) - 0x20] = body[pos:pos + length]
        pos += length
    return args


def reply(func, error=0, arg=None):
    out = bytes([func | 0x80, 1 if arg is not None else 0]) + struct.pack(">H", error)
    if arg is not None:
        if len(arg) <= 0xFF:
            out += bytes([0x20, len(arg)])
        elif len(arg) <= 0xFFFF:
            out += bytes([0xA0, 0]) + struct.pack(">H", len(arg))
        else:
            out += bytes([0x60, 0]) + struct.pack(">I", len(arg))
        out += arg
    return out


def cstr(data):
    return data.split(b"\0")[0].decode("latin-1")


DATE = struct.pack(">HBBBBBB", 2026, 10, 18, 12, 0, 0, 0)


def new_state():
    note = {"id": 0x100001, "attr": 0, "cat": 0, "data": b"Hello from a fake Palm\0".hex()}
    return {
        "user": {"name": "", "id": 0},
        "databases": {
            "FujiNet Notes": {"type": "DATA", "creator": "FjNt", "flags": DB_BACKUP,
                              "version": 0, "appinfo": "", "records": [note], "resources": []},
        },
    }


class Palm:
    def __init__(self, state):
        self.state = state
        self.user = state["user"]
        self.dbs = state["databases"]
        self.open = {}      # handle -> database name
        self.log = []
        self.installed = []

    def db(self, handle):
        return self.dbs[self.open[handle]]

    def handle(self, body):
        func = body[0]
        args = parse_args(body)
        a = args.get(0, b"")

        if func == F["ReadUserInfo"]:
            u = self.user["name"].encode("latin-1")
            zero = b"\0" * 8
            return reply(func, 0, struct.pack(">III", self.user["id"], 0, 0) + zero + zero +
                         bytes([len(u) + 1 if u else 0, 0]) + (u + b"\0" if u else b""))
        if func == F["WriteUserInfo"]:
            user_id = struct.unpack(">I", a[0:4])[0]
            flags = a[20]
            if flags & 0x80:
                self.user["id"] = user_id
            if flags & 0x10:
                self.user["name"] = cstr(a[22:])
            return reply(func)
        if func == F["ReadSysInfo"]:
            return reply(func, 0, struct.pack(">IIBB", 0x03303000, 0, 0, 0))
        if func == F["GetSysDateTime"]:
            return reply(func, 0, DATE)
        if func == F["ReadDBList"]:
            start = struct.unpack(">H", a[2:4])[0]
            names = sorted(self.dbs)
            if start >= len(names):
                return reply(func, ERR_NOT_FOUND)
            out = struct.pack(">HBB", len(names) - 1, 0, len(names) - start)
            for index in range(start, len(names)):
                d = self.dbs[names[index]]
                entry = struct.pack(">BH4s4sHI", 0, d["flags"], d["type"].encode(), d["creator"].encode(),
                                    d["version"], 0) + DATE * 3 + struct.pack(">H", index)
                entry += names[index].encode("latin-1") + b"\0"
                out += bytes([len(entry) + 1]) + entry
            return reply(func, 0, out)
        if func == F["OpenDB"]:
            name = cstr(a[2:])
            if name not in self.dbs:
                return reply(func, ERR_NOT_FOUND)
            handle = len(self.open) + 1
            self.open[handle] = name
            return reply(func, 0, bytes([handle]))
        if func == F["CreateDB"]:
            creator, dbtype = a[0:4].decode("latin-1"), a[4:8].decode("latin-1")
            flags, version = struct.unpack(">HH", a[10:14])
            name = cstr(a[14:])
            self.dbs[name] = {"type": dbtype, "creator": creator, "flags": flags, "version": version,
                              "appinfo": "", "records": [], "resources": []}
            self.installed.append(name)
            handle = len(self.open) + 1
            self.open[handle] = name
            return reply(func, 0, bytes([handle]))
        if func == F["DeleteDB"]:
            return reply(func, 0 if self.dbs.pop(cstr(a[2:]), None) else ERR_NOT_FOUND)
        if func == F["ReadOpenDBInfo"]:
            d = self.db(a[0])
            count = len(d["resources"]) if d["flags"] & DB_RESOURCE else len(d["records"])
            return reply(func, 0, struct.pack(">H", count))
        if func == F["ReadAppBlock"]:
            data = bytes.fromhex(self.db(a[0])["appinfo"])
            if not data:
                return reply(func, ERR_NOT_FOUND)
            return reply(func, 0, struct.pack(">H", len(data)) + data)
        if func == F["WriteAppBlock"]:
            self.db(a[0])["appinfo"] = a[4:].hex()
            return reply(func)
        if func in (F["ReadSortBlock"],):
            return reply(func, ERR_NOT_FOUND)
        if func == F["ReadRecord"]:
            if 1 not in args:       # by ID; FujiNet's backup reads by index
                return reply(func, ERR_NOT_FOUND)
            r1 = args[1]
            d = self.db(r1[0])
            index = struct.unpack(">H", r1[2:4])[0]
            if index >= len(d["records"]):
                return reply(func, ERR_NOT_FOUND)
            rec = d["records"][index]
            data = bytes.fromhex(rec["data"])
            return reply(func, 0, struct.pack(">IHHBB", rec["id"], index, len(data), rec["attr"], rec["cat"]) + data)
        if func == F["WriteRecord"]:
            d = self.db(a[0])
            rid, attr, cat = struct.unpack(">IBB", a[2:8])
            if rid == 0:
                rid = 0x200000 + len(d["records"])
            d["records"].append({"id": rid, "attr": attr, "cat": cat, "data": a[8:].hex()})
            return reply(func, 0, struct.pack(">I", rid))
        if func == F["ReadResource"]:
            d = self.db(a[0])
            index = struct.unpack(">H", a[2:4])[0]
            if index >= len(d["resources"]):
                return reply(func, ERR_NOT_FOUND)
            res = d["resources"][index]
            data = bytes.fromhex(res["data"])
            return reply(func, 0, res["type"].encode() + struct.pack(">HHH", res["id"], index, len(data)) + data)
        if func == F["WriteResource"]:
            rtype = a[2:6].decode("latin-1")
            rid, size = struct.unpack(">HH", a[6:10])
            self.db(a[0])["resources"].append({"type": rtype, "id": rid, "data": a[10:10 + size].hex()})
            return reply(func)
        if func == F["AddSyncLogEntry"]:
            self.log.append(cstr(a).strip())
            return reply(func)
        # OpenConduit, CloseDB, ResetSystem, EndOfSync and anything else.
        return reply(func)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host")
    p.add_argument("--port", type=int, default=14238)
    p.add_argument("--state", default="fake-palm.json",
                   help="JSON file holding this Palm's user and databases (default: %(default)s)")
    p.add_argument("--verbose", action="store_true", help="print each DLP request")
    opts = p.parse_args()

    state = new_state()
    if os.path.exists(opts.state):
        with open(opts.state) as f:
            state = json.load(f)
    palm = Palm(state)

    sock = socket.create_connection((opts.host, opts.port), timeout=60)
    link = Link(sock)
    # The handshake payloads are opaque; FujiNet checks only their lengths.
    for size in (22, 50):
        link.send(bytes(size))
        link.recv()
    link.send(bytes(8))

    while True:
        body = link.recv()
        if opts.verbose:
            print("  <", NAMES.get(body[0], hex(body[0])), file=sys.stderr)
        link.send(palm.handle(body))
        if body[0] == F["EndOfSync"]:
            break
    sock.close()

    with open(opts.state, "w") as f:
        json.dump(state, f, indent=1)

    print("User: %s (id %08x)" % (palm.user["name"], palm.user["id"]))
    for line in palm.log:
        print("Log:  " + line)
    for name in palm.installed:
        print("Installed: " + name)
    print("Databases: " + ", ".join(sorted(palm.dbs)))


if __name__ == "__main__":
    main()
