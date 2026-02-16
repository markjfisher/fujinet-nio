#!/usr/bin/env bash
set -euo pipefail

HTTP_NAME="fujinet-httpbin"
HTTPS_NAME="fujinet-https-proxy"
TCP_NAME="fujinet-tcp-echo"

HTTP_PORT_HOST="${HTTP_PORT_HOST:-8080}"
HTTP_PORT_CONT="${HTTP_PORT_CONT:-80}"

HTTPS_PORT_HOST="${HTTPS_PORT_HOST:-8443}"
HTTPS_PORT_CONT="${HTTPS_PORT_CONT:-443}"

TCP_PORT_HOST="${TCP_PORT_HOST:-7777}"
TCP_PORT_CONT="${TCP_PORT_CONT:-7777}"

HTTP_IMAGE="${HTTP_IMAGE:-kennethreitz/httpbin}"
TCP_IMAGE="${TCP_IMAGE:-nicolaka/netshoot}"
NGINX_IMAGE="${NGINX_IMAGE:-nginx:alpine}"

usage() {
  cat <<EOF
Usage:
  $0 http            Start httpbin on localhost:${HTTP_PORT_HOST}
  $0 https           Start httpbin + nginx HTTPS reverse proxy on localhost:${HTTPS_PORT_HOST}
  $0 tcp             Start TCP echo on localhost:${TCP_PORT_HOST} (foreground with traffic logs)
  $0 both            Start httpbin (detached) + TCP echo (foreground)
  $0 all             Start httpbin + HTTPS proxy + TCP echo (foreground)
  $0 stop            Stop all services
  $0 status          Show container status
  $0 logs http|https|tcp   Show logs (tcp logs are in-container stdout)
Options (env vars):
  HTTP_PORT_HOST=8080
  HTTPS_PORT_HOST=8443
  TCP_PORT_HOST=7777
  HTTP_IMAGE=kennethreitz/httpbin
  TCP_IMAGE=nicolaka/netshoot
  NGINX_IMAGE=nginx:alpine

HTTPS Testing:
  After running '$0 https', test with:
    curl -k https://localhost:${HTTPS_PORT_HOST}/get
  Or with fujinet-nio-lib:
    make TEST_URL="\"https://192.168.1.xxx:${HTTPS_PORT_HOST}/get\""
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

  echo "Starting httpbin: http://localhost:${HTTP_PORT_HOST}"
  # Detached so the script can continue / exit cleanly
  docker run -d --rm \
    --name "$HTTP_NAME" \
    -p "${HTTP_PORT_HOST}:${HTTP_PORT_CONT}" \
    "$HTTP_IMAGE" >/dev/null

  echo "httpbin started."
}

# Generate self-signed certificate for HTTPS testing
generate_cert() {
  local cert_dir="/tmp/fujinet-https-certs"
  mkdir -p "$cert_dir"
  
  if [ -f "$cert_dir/server.crt" ] && [ -f "$cert_dir/server.key" ]; then
    echo "Using existing certificates in $cert_dir"
    return 0
  fi
  
  echo "Generating self-signed certificate for HTTPS testing..."
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

  echo "Starting nginx HTTPS reverse proxy: https://localhost:${HTTPS_PORT_HOST}"
  
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
        server_name localhost;

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
  docker run -d --rm \
    --name "$HTTPS_NAME" \
    -p "${HTTPS_PORT_HOST}:${HTTPS_PORT_CONT}" \
    -v /tmp/fujinet-https-certs:/etc/nginx/ssl:ro \
    -v "$config_dir/nginx.conf:/etc/nginx/nginx.conf:ro" \
    --link "$HTTP_NAME:fujinet-httpbin" \
    "$NGINX_IMAGE" >/dev/null

  echo "HTTPS proxy started."
  echo "Test with: curl -k https://localhost:${HTTPS_PORT_HOST}/get"
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

status() {
  echo "Containers:"
  docker ps --format '  {{.Names}}\t{{.Status}}\t{{.Ports}}' | grep -E "(${HTTP_NAME}|${HTTPS_NAME}|${TCP_NAME})" || true
  echo
  echo "Endpoints:"
  echo "  http:  http://localhost:${HTTP_PORT_HOST}"
  echo "  https: https://localhost:${HTTPS_PORT_HOST}"
  echo "  tcp:   tcp://127.0.0.1:${TCP_PORT_HOST}"
}

logs_cmd() {
  local which="${1:-}"
  case "$which" in
    http)  docker logs "$HTTP_NAME" ;;
    https) docker logs "$HTTPS_NAME" ;;
    tcp)   docker logs "$TCP_NAME" ;;
    *) echo "logs requires: http|https|tcp" ; exit 2 ;;
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
  both)
    start_http
    start_tcp
    ;;
  all)
    start_https
    start_tcp
    ;;
  stop)
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
