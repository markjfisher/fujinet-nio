#!/usr/bin/env python3
import argparse
import json
import fuji_send


def list_dir(port: str, fs_name: str, path: str):
    # Temporary example: device 0x70, command 0xF0
    device = 0xFE
    command = 0x01

    payload_obj = {"fs": fs_name, "path": path}
    payload = json.dumps(payload_obj).encode("utf-8")

    resp = fuji_send.send_fuji_command(
        port=port,
        device=device,
        command=command,
        payload=payload,
        baud=115200,
        timeout=1.0,
        debug=False,
    )

    if resp is None:
        print("No or invalid response")
        return

    # assuming the device puts JSON in the payload
    try:
        entries = json.loads(resp["payload"])
    except Exception as e:
        print("Response payload is not JSON:", e)
        print(resp)
        return

    print(f"Directory listing for {fs_name}:{path}")
    print(json.dumps(entries, indent=2))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", "-p", required=True)
    parser.add_argument("fs")
    parser.add_argument("path")
    args = parser.parse_args()

    list_dir(args.port, args.fs, args.path)


if __name__ == "__main__":
    main()
