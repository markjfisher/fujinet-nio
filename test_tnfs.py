#!/usr/bin/env python3
import socket
import struct

TNFS_PORT = 16384
TNFS_HOST = 'localhost'

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(2.0)

    # TNFS mount command (version 1.2)
    mount_cmd = bytearray()
    mount_cmd.append(0x00)  # sessionIdL
    mount_cmd.append(0x00)  # sessionIdH
    mount_cmd.append(0x00)  # sequenceNum
    mount_cmd.append(0x00)  # command (CMD_MOUNT)
    mount_cmd.append(0x02)  # minor version
    mount_cmd.append(0x01)  # major version
    mount_cmd.append(0x2f)  # mount path '/'
    mount_cmd.append(0x00)  # null terminator
    mount_cmd.append(0x00)  # user null terminator
    mount_cmd.append(0x00)  # password null terminator

    print(f"Sending mount command to {TNFS_HOST}:{TNFS_PORT}")
    sock.sendto(mount_cmd, (TNFS_HOST, TNFS_PORT))

    try:
        data, addr = sock.recvfrom(532)  # TNFS packet size
        print(f"Received {len(data)} bytes from {addr}")
        print(f"Response: {data.hex()}")

        if len(data) > 0:
            result = data[4]
            if result == 0x00:
                print("Mount successful")
            else:
                print(f"Mount failed: result code {result}")
    except socket.timeout:
        print("Timeout waiting for response")
    except Exception as e:
        print(f"Error: {e}")
    finally:
        sock.close()

if __name__ == "__main__":
    main()
