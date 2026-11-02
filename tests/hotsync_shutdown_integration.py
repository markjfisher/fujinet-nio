"""Stop the POSIX service while a real NetSync peer stalls mid-handshake."""

import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        (root / "fujinet-data").mkdir()
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        (root / "fujinet-data/fujinet.yaml").write_text(
            f"hotsync:\n  enabled: true\n  port: {port}\n  cradle: false\n"
        )
        env = dict(os.environ, FN_CONSOLE="stdio")
        with (root / "log").open("w+") as log:
            process = subprocess.Popen(
                [binary], cwd=root, env=env, stdin=subprocess.PIPE,
                stdout=log, stderr=subprocess.STDOUT,
            )
            peer = None
            try:
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    try:
                        peer = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                        break
                    except OSError:
                        time.sleep(0.05)
                assert peer is not None, "NetSync listener did not start"
                peer.settimeout(3)
                # Complete the first handshake to prove the service accepted us.
                peer.sendall(b"\x01\x01\x00\x00\x00\x16" + bytes(22))
                assert peer.recv(512), "no NetSync handshake response"
                peer.sendall(b"\x01\x02\x00")  # Half of the next header.
                time.sleep(0.05)
                start = time.monotonic()
                process.stdin.write(b"reboot\n")
                process.stdin.flush()
                result = process.wait(timeout=4)
                assert result == 75, (result, (root / "log").read_text()[-3000:])
                print(f"stalled NetSync interrupted; reboot exited in {time.monotonic() - start:.3f}s")
            finally:
                if peer is not None:
                    peer.close()
                if process.poll() is None:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    main()
