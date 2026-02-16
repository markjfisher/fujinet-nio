#!/bin/bash
#
# Generate self-signed CA and server certificates for FujiNet HTTPS testing.
#
# This creates:
# - CA certificate (ca.crt) - embedded in firmware for verification
# - Server certificate (server.crt) - used by nginx for HTTPS
# - Private keys for both
#
# The certificates are valid for 10 years and support:
# - localhost
# - 127.0.0.1
# - 192.168.1.x (common home network range)
#
# Usage: ./generate_certs.sh [output_dir]
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${1:-$SCRIPT_DIR}"

echo "Generating FujiNet test certificates in: $OUTPUT_DIR"
cd "$OUTPUT_DIR"

# Generate CA private key
echo "1. Generating CA private key..."
openssl genrsa -out ca.key 2048

# Generate CA certificate (self-signed, valid 10 years)
echo "2. Generating CA certificate..."
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
    -out ca.crt \
    -subj "/CN=FujiNet Test CA/O=FujiNet Test/C=US"

# Generate server private key
echo "3. Generating server private key..."
openssl genrsa -out server.key 2048

# Create a certificate signing request (CSR)
echo "4. Generating server CSR..."
openssl req -new -key server.key -out server.csr \
    -subj "/CN=localhost"

# Create extensions file for SAN (Subject Alternative Names)
cat > server.ext << 'EOF'
authorityKeyIdentifier=keyid,issuer
basicConstraints=CA:FALSE
keyUsage = digitalSignature, nonRepudiation, keyEncipherment, dataEncipherment
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
DNS.2 = *.local
IP.1 = 127.0.0.1
IP.2 = 192.168.1.1
IP.3 = 192.168.1.101
IP.4 = 192.168.1.102
IP.5 = 192.168.1.103
IP.6 = 10.0.0.1
IP.7 = 172.17.0.1
EOF

# Sign the server certificate with our CA
echo "5. Signing server certificate with CA..."
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key \
    -CAcreateserial -out server.crt -days 3650 -sha256 \
    -extfile server.ext

# Clean up temporary files
rm -f server.csr server.ext ca.srl

# Set permissions
chmod 644 ca.crt server.crt
chmod 600 ca.key server.key

echo ""
echo "=== Certificate Generation Complete ==="
echo ""
echo "Files created:"
echo "  ca.crt      - CA certificate (embed in firmware)"
echo "  ca.key      - CA private key (keep secure)"
echo "  server.crt  - Server certificate (for nginx)"
echo "  server.key  - Server private key (for nginx)"
echo ""
echo "To embed CA in firmware, use the contents of ca.crt"
echo "To use with nginx:"
echo "  ssl_certificate /path/to/server.crt;"
echo "  ssl_certificate_key /path/to/server.key;"
echo ""
echo "To verify:"
echo "  openssl x509 -in ca.crt -text -noout"
echo "  openssl x509 -in server.crt -text -noout"
