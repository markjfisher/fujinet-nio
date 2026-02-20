#!/usr/bin/env bash
set -euo pipefail

HTTP_NAME="fujinet-httpbin"
HTTPS_NAME="fujinet-https-proxy"
TCP_NAME="fujinet-tcp-echo"
TLS_NAME="fujinet-tls-echo"
STREAM_NAME="fujinet-tcp-stream"

HTTP_PORT_HOST="${HTTP_PORT_HOST:-8080}"
HTTP_PORT_CONT="${HTTP_PORT_CONT:-80}"

HTTPS_PORT_HOST="${HTTPS_PORT_HOST:-8443}"
HTTPS_PORT_CONT="${HTTPS_PORT_CONT:-443}"

TCP_PORT_HOST="${TCP_PORT_HOST:-7777}"
TCP_PORT_CONT="${TCP_PORT_CONT:-7777}"

TLS_PORT_HOST="${TLS_PORT_HOST:-7778}"
TLS_PORT_CONT="${TLS_PORT_CONT:-7778}"

STREAM_PORT_HOST="${STREAM_PORT_HOST:-7779}"
STREAM_PORT_CONT="${STREAM_PORT_CONT:-7779}"

HTTP_IMAGE="${HTTP_IMAGE:-kennethreitz/httpbin}"
TCP_IMAGE="${TCP_IMAGE:-nicolaka/netshoot}"
NGINX_IMAGE="${NGINX_IMAGE:-nginx:alpine}"

usage() {
  cat <<EOF
Usage:
  $0 http            Start httpbin on localhost:${HTTP_PORT_HOST}
  $0 https           Start httpbin + nginx HTTPS reverse proxy on localhost:${HTTPS_PORT_HOST}
  $0 tcp             Start TCP echo on localhost:${TCP_PORT_HOST} (foreground with traffic logs)
  $0 tls             Start TLS echo on localhost:${TLS_PORT_HOST} (nginx stream proxy)
  $0 stream          Start TCP streaming server on localhost:${STREAM_PORT_HOST} (sends frames continuously)
  $0 both            Start httpbin (detached) + TCP echo (foreground)
  $0 all             Start httpbin + HTTPS proxy + TCP echo + TLS echo + stream (foreground)
  $0 stop            Stop all services
  $0 status          Show container status
  $0 logs http|https|tcp|tls|stream   Show logs (tcp logs are in-container stdout)
Options (env vars):
  HTTP_PORT_HOST=8080
  HTTPS_PORT_HOST=8443
  TCP_PORT_HOST=7777
  TLS_PORT_HOST=7778
  STREAM_PORT_HOST=7779
  HTTP_IMAGE=kennethreitz/httpbin
  TCP_IMAGE=nicolaka/netshoot
  NGINX_IMAGE=nginx:alpine

HTTPS Testing:
  After running '$0 https', test with:
    curl -k https://localhost:${HTTPS_PORT_HOST}/get
  Or with fujinet-nio-lib:
    make TEST_URL="\"https://192.168.1.xxx:${HTTPS_PORT_HOST}/get?testca=1\""

TLS Testing:
  After running '$0 tls', test with:
    openssl s_client -connect localhost:${TLS_PORT_HOST}
  Or with fujinet-nio-lib:
    Use URL: tls://192.168.1.xxx:${TLS_PORT_HOST}?testca=1

Stream Testing:
  After running '$0 stream', test with:
    nc localhost ${STREAM_PORT_HOST}
  Or with fujinet-nio-lib tcp_stream example:
    FN_TCP_HOST=127.0.0.1 FN_TCP_PORT=${STREAM_PORT_HOST} ./bin/linux/tcp_stream
EOF
}

is_running() {
  local name="$1"
  docker ps --format '{{.Names}}' | grep -qx "$name"
}

stop_one() {
  local name="$1"
  if docker ps -a --format '{{.Names}}' | grep -qx "$name"; then
    echo "Stopping $name..."
    docker rm -f "$name" >/dev/null 2>&1 || true
  fi
}

start_http() {
  if is_running "$HTTP_NAME"; then
    echo "httpbin already running: http://localhost:${HTTP_PORT_HOST}"
    return 0
  fi

  # Get host IP for network access
  local host_ip
  host_ip=$(ip addr show | grep 'inet ' | grep -v '127.0.0.1' | head -1 | awk '{print $2}' | cut -d'/' -f1)
  
  echo "Starting httpbin:"
  echo "  Local:   http://localhost:${HTTP_PORT_HOST}"
  echo "  Network: http://${host_ip}:${HTTP_PORT_HOST}"
  
  # Detached so the script can continue / exit cleanly
  # Bind to all interfaces (0.0.0.0) so it's accessible from network
  docker run -d --rm \
    --name "$HTTP_NAME" \
    -p "0.0.0.0:${HTTP_PORT_HOST}:${HTTP_PORT_CONT}" \
    "$HTTP_IMAGE" >/dev/null

  echo "httpbin started."
}

# Generate self-signed certificate for HTTPS testing
generate_cert() {
  local cert_dir="/tmp/fujinet-https-certs"
  mkdir -p "$cert_dir"
  
  # Use pre-generated test certificates from the repo if available
  # These are signed by the FujiNet Test CA which is embedded in firmware
  local repo_certs="$(dirname "$0")/../integration-tests/certs"
  if [ -f "$repo_certs/server.crt" ] && [ -f "$repo_certs/server.key" ]; then
    echo "Using FujiNet Test CA certificates from repo"
    cp "$repo_certs/server.crt" "$cert_dir/"
    cp "$repo_certs/server.key" "$cert_dir/"
    return 0
  fi
  
  if [ -f "$cert_dir/server.crt" ] && [ -f "$cert_dir/server.key" ]; then
    echo "Using existing certificates in $cert_dir"
    return 0
  fi
  
  echo "Generating self-signed certificate for HTTPS testing..."
  echo "NOTE: For proper TLS verification, use integration-tests/certs/generate_certs.sh"
  openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
    -keyout "$cert_dir/server.key" \
    -out "$cert_dir/server.crt" \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    >/dev/null 2>&1
  
  echo "Certificates generated in $cert_dir"
}

start_https() {
  # Start httpbin first if not running
  if ! is_running "$HTTP_NAME"; then
    start_http
    sleep 2  # Wait for httpbin to be ready
  fi

  if is_running "$HTTPS_NAME"; then
    echo "HTTPS proxy already running: https://localhost:${HTTPS_PORT_HOST}"
    return 0
  fi

  generate_cert

  # Get host IP for certificate
  local host_ip
  host_ip=$(ip addr show | grep 'inet ' | grep -v '127.0.0.1' | head -1 | awk '{print $2}' | cut -d'/' -f1)
  
  echo "Starting nginx HTTPS reverse proxy:"
  echo "  Local:   https://localhost:${HTTPS_PORT_HOST}"
  echo "  Network: https://${host_ip}:${HTTPS_PORT_HOST}"
  
  # Create nginx config
  local config_dir="/tmp/fujinet-https-config"
  mkdir -p "$config_dir"
  
  cat > "$config_dir/nginx.conf" <<'NGINX_EOF'
events {
    worker_connections 1024;
}

http {
    server {
        listen 443 ssl;
        server_name _;  # Accept any hostname

        ssl_certificate /etc/nginx/ssl/server.crt;
        ssl_certificate_key /etc/nginx/ssl/server.key;

        location / {
            proxy_pass http://fujinet-httpbin:80;
            proxy_set_header Host $host;
            proxy_set_header X-Real-IP $remote_addr;
            proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
            proxy_set_header X-Forwarded-Proto $scheme;
        }
    }
}
NGINX_EOF

  # Run nginx with SSL termination
  # Bind to all interfaces (0.0.0.0) so it's accessible from network
  docker run -d --rm \
    --name "$HTTPS_NAME" \
    -p "0.0.0.0:${HTTPS_PORT_HOST}:${HTTPS_PORT_CONT}" \
    -v /tmp/fujinet-https-certs:/etc/nginx/ssl:ro \
    -v "$config_dir/nginx.conf:/etc/nginx/nginx.conf:ro" \
    --link "$HTTP_NAME:fujinet-httpbin" \
    "$NGINX_IMAGE" >/dev/null

  echo "HTTPS proxy started."
  echo "Test with: curl -k https://localhost:${HTTPS_PORT_HOST}/get"
  echo "       or: curl -k https://${host_ip}:${HTTPS_PORT_HOST}/get"
}

start_tcp() {
  if is_running "$TCP_NAME"; then
    echo "tcp echo already running on localhost:${TCP_PORT_HOST}"
    echo "Run: $0 stop   (then restart)"
    return 0
  fi

  echo "Starting TCP echo (foreground, with traffic logs) on localhost:${TCP_PORT_HOST}"
  echo "Press Ctrl-C to stop (or run: $0 stop from another shell)"
  # Foreground so you see the -v -v traffic logs from socat
  docker run --rm -it \
    --name "$TCP_NAME" \
    -p "${TCP_PORT_HOST}:${TCP_PORT_CONT}" \
    "$TCP_IMAGE" \
    socat -v -v "tcp-listen:${TCP_PORT_CONT},reuseaddr,fork" exec:cat
}

start_tls() {
  # Start TCP echo first if not running
  if ! is_running "$TCP_NAME"; then
    echo "Starting TCP echo (detached) for TLS proxy..."
    docker run -d --rm \
      --name "$TCP_NAME" \
      -p "${TCP_PORT_HOST}:${TCP_PORT_CONT}" \
      "$TCP_IMAGE" \
      socat -v -v "tcp-listen:${TCP_PORT_CONT},reuseaddr,fork" exec:cat
    sleep 1
  fi

  if is_running "$TLS_NAME"; then
    echo "TLS echo already running on localhost:${TLS_PORT_HOST}"
    return 0
  fi

  generate_cert

  # Get host IP for display
  local host_ip
  host_ip=$(ip addr show | grep 'inet ' | grep -v '127.0.0.1' | head -1 | awk '{print $2}' | cut -d'/' -f1)
  
  echo "Starting TLS echo (nginx stream proxy):"
  echo "  Local:   tls://localhost:${TLS_PORT_HOST}"
  echo "  Network: tls://${host_ip}:${TLS_PORT_HOST}"
  
  # Create nginx stream config for TLS echo
  local config_dir="/tmp/fujinet-tls-config"
  mkdir -p "$config_dir"
  
  cat > "$config_dir/nginx.conf" <<'NGINX_EOF'
events {
    worker_connections 1024;
}

stream {
    server {
        listen 7778 ssl;
        
        ssl_certificate /etc/nginx/ssl/server.crt;
        ssl_certificate_key /etc/nginx/ssl/server.key;
        
        # Proxy to TCP echo service
        proxy_pass fujinet-tcp-echo:7777;
    }
}
NGINX_EOF

  # Run nginx as TLS stream proxy
  # Bind to all interfaces (0.0.0.0) so it's accessible from network
  docker run -d --rm \
    --name "$TLS_NAME" \
    -p "0.0.0.0:${TLS_PORT_HOST}:${TLS_PORT_CONT}" \
    -v /tmp/fujinet-https-certs:/etc/nginx/ssl:ro \
    -v "$config_dir/nginx.conf:/etc/nginx/nginx.conf:ro" \
    --link "$TCP_NAME:fujinet-tcp-echo" \
    "$NGINX_IMAGE" >/dev/null

  echo "TLS echo started."
  echo "Test with: openssl s_client -connect localhost:${TLS_PORT_HOST}"
  echo "       or: use tls://${host_ip}:${TLS_PORT_HOST}?testca=1 in fujinet-nio"
}

start_stream() {
  if is_running "$STREAM_NAME"; then
    echo "TCP stream already running on localhost:${STREAM_PORT_HOST}"
    echo "Run: $0 stop   (then restart)"
    return 0
  fi

  # Get host IP for display
  local host_ip
  host_ip=$(ip addr show | grep 'inet ' | grep -v '127.0.0.1' | head -1 | awk '{print $2}' | cut -d'/' -f1)
  
  echo "Starting TCP streaming server (foreground) on localhost:${STREAM_PORT_HOST}"
  echo "This server continuously sends frames of data for testing non-blocking reads."
  echo "Press Ctrl-C to stop (or run: $0 stop from another shell)"
  echo ""
  echo "Test with: nc localhost ${STREAM_PORT_HOST}"
  echo "       or: FN_TCP_HOST=127.0.0.1 FN_TCP_PORT=${STREAM_PORT_HOST} ./bin/linux/tcp_stream"
  
  # Create a streaming server using socat
  # The SYSTEM option runs a command with stdin/stdout connected to the socket
  # Each frame is: "FRAME <number>: <timestamp>\n"
  # Note: Using a heredoc to avoid quoting issues with bash -c
  docker run --rm -it \
    --name "$STREAM_NAME" \
    -p "${STREAM_PORT_HOST}:${STREAM_PORT_CONT}" \
    "$TCP_IMAGE" \
    bash -c 'socat -v TCP-LISTEN:7779,reuseaddr,fork SYSTEM:"bash -c '\''frame=0; while true; do printf \"FRAME %d: %s\n\" \$frame \"\$(date +%H:%M:%S.%3N)\"; frame=\$((frame + 1)); sleep 0.1; done'\''"'
}

status() {
  echo "Containers:"
  docker ps --format '  {{.Names}}\t{{.Status}}\t{{.Ports}}' | grep -E "(${HTTP_NAME}|${HTTPS_NAME}|${TCP_NAME}|${TLS_NAME}|${STREAM_NAME})" || true
  echo
  echo "Endpoints:"
  echo "  http:   http://localhost:${HTTP_PORT_HOST}"
  echo "  https:  https://localhost:${HTTPS_PORT_HOST}"
  echo "  tcp:    tcp://127.0.0.1:${TCP_PORT_HOST}"
  echo "  tls:    tls://127.0.0.1:${TLS_PORT_HOST}"
  echo "  stream: tcp://127.0.0.1:${STREAM_PORT_HOST}"
}

logs_cmd() {
  local which="${1:-}"
  case "$which" in
    http)   docker logs "$HTTP_NAME" ;;
    https)  docker logs "$HTTPS_NAME" ;;
    tcp)    docker logs "$TCP_NAME" ;;
    tls)    docker logs "$TLS_NAME" ;;
    stream) docker logs "$STREAM_NAME" ;;
    *) echo "logs requires: http|https|tcp|tls|stream" ; exit 2 ;;
  esac
}

cmd="${1:-}"
case "$cmd" in
  http)
    start_http
    ;;
  https)
    start_https
    ;;
  tcp)
    start_tcp
    ;;
  tls)
    start_tls
    ;;
  stream)
    start_stream
    ;;
  both)
    start_http
    start_tcp
    ;;
  all)
    start_https
    start_tls
    start_stream
    ;;
  stop)
    stop_one "$STREAM_NAME"
    stop_one "$TLS_NAME"
    stop_one "$TCP_NAME"
    stop_one "$HTTPS_NAME"
    stop_one "$HTTP_NAME"
    echo "Stopped."
    ;;
  status)
    status
    ;;
  logs)
    logs_cmd "${2:-}"
    ;;
  -h|--help|help|"")
    usage
    ;;
  *)
    echo "Unknown command: $cmd"
    usage
    exit 2
    ;;
esac
