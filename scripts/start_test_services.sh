#!/usr/bin/env bash
set -euo pipefail

HTTP_NAME="fujinet-httpbin"
TCP_NAME="fujinet-tcp-echo"

HTTP_PORT_HOST="${HTTP_PORT_HOST:-8080}"
HTTP_PORT_CONT="${HTTP_PORT_CONT:-80}"

TCP_PORT_HOST="${TCP_PORT_HOST:-7777}"
TCP_PORT_CONT="${TCP_PORT_CONT:-7777}"

HTTP_IMAGE="${HTTP_IMAGE:-kennethreitz/httpbin}"
TCP_IMAGE="${TCP_IMAGE:-nicolaka/netshoot}"

usage() {
  cat <<EOF
Usage:
  $0 http            Start httpbin on localhost:${HTTP_PORT_HOST}
  $0 tcp             Start TCP echo on localhost:${TCP_PORT_HOST} (foreground with traffic logs)
  $0 both            Start httpbin (detached) + TCP echo (foreground)
  $0 stop            Stop both services
  $0 status          Show container status
  $0 logs http|tcp   Show logs (tcp logs are in-container stdout)
Options (env vars):
  HTTP_PORT_HOST=8080
  TCP_PORT_HOST=7777
  HTTP_IMAGE=kennethreitz/httpbin
  TCP_IMAGE=nicolaka/netshoot
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
  docker ps --format '  {{.Names}}\t{{.Status}}\t{{.Ports}}' | grep -E "(${HTTP_NAME}|${TCP_NAME})" || true
  echo
  echo "Endpoints:"
  echo "  httpbin: http://localhost:${HTTP_PORT_HOST}"
  echo "  tcp:     tcp://127.0.0.1:${TCP_PORT_HOST}"
}

logs_cmd() {
  local which="${1:-}"
  case "$which" in
    http) docker logs "$HTTP_NAME" ;;
    tcp)  docker logs "$TCP_NAME" ;;
    *) echo "logs requires: http|tcp" ; exit 2 ;;
  esac
}

cmd="${1:-}"
case "$cmd" in
  http)
    start_http
    ;;
  tcp)
    start_tcp
    ;;
  both)
    start_http
    start_tcp
    ;;
  stop)
    stop_one "$TCP_NAME"
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
