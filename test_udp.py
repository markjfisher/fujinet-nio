#!/usr/bin/env python3
import socket
import time

def test_udp_connection():
    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server_address = ('localhost', 16384)
    
    # TNFS Mount command (version 1.2)
    # Format: conn_id(2) retry(1) cmd(1) ver_min(1) ver_maj(1) mount_path(0-terminated) user(0-terminated) password(0-terminated)
    mount_cmd = b'\x00\x00\x00\x00\x02\x01/' + b'\x00' + b'\x00' + b'\x00'
    
    try:
        print(f"Sending mount command to {server_address}")
        sent = sock.sendto(mount_cmd, server_address)
        print(f"Sent {sent} bytes")
        
        # Set timeout for receiving response
        sock.settimeout(5.0)
        
        try:
            print("Waiting for response...")
            data, addr = sock.recvfrom(1024)
            print(f"Received {len(data)} bytes from {addr}")
            print(f"Response: {data.hex()}")
            
            # Parse response
            if len(data) >= 5:
                conn_id = int.from_bytes(data[0:2], 'little')
                retry = data[2]
                cmd = data[3]
                status = data[4]
                
                print(f"conn_id: 0x{conn_id:04x}, retry: {retry}, cmd: 0x{cmd:02x}, status: 0x{status:02x}")
                
                if status == 0:
                    print("Mount successful!")
                    
                    # If we have more data, check version and retry delay
                    if len(data) >= 7:
                        ver_min = data[5]
                        ver_maj = data[6]
                        retry_delay = int.from_bytes(data[7:9], 'little') if len(data) >=9 else 0
                        
                        print(f"Version: {ver_maj}.{ver_min}, Retry delay: {retry_delay}ms")
                else:
                    print(f"Mount failed with status: {status}")
            else:
                print("Response too short")
                
        except socket.timeout:
            print("Timeout waiting for response")
        except Exception as e:
            print(f"Error receiving data: {e}")
            
    except Exception as e:
        print(f"Error: {e}")
    finally:
        sock.close()

if __name__ == "__main__":
    test_udp_connection()
