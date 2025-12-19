#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<EOF
Usage:
  $0 --posix-pty /dev/pts/N
  $0 --serial /dev/ttyACM1

Options:
  --http-url  (default: http://localhost:8080)
  --tcp-url   (default: tcp://127.0.0.1:7777)
  --no-docker (do not start docker test services)
EOF
}

HTTP_URL="http://localhost:8080"
TCP_URL="tcp://127.0.0.1:7777"
NO_DOCKER=0
TARGET=""
PORT=""
FS_PATH="host"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --posix-pty)
      TARGET="posix"
      PORT="$2"
      shift 2
      ;;
    --serial)
      TARGET="serial"
      PORT="$2"
      shift 2
      ;;
    --http-url)
      HTTP_URL="$2"
      shift 2
      ;;
    --tcp-url)
      TCP_URL="$2"
      shift 2
      ;;
    --fs)
      FS_PATH="$2"
      shift 2
      ;;
    --no-docker)
      NO_DOCKER=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown arg: $1"
      usage
      exit 2
      ;;
  esac
done

if [[ -z "$TARGET" || -z "$PORT" ]]; then
  usage
  exit 2
fi

if [[ "$NO_DOCKER" -eq 0 ]]; then
  ./scripts/start_test_services.sh both
fi

# Run the python-driven integration flow using the actual scripts/fujinet CLI.
python3 ./scripts/integration_test_runner.py \
  --port "$PORT" \
  --http-url "$HTTP_URL" \
  --tcp-url "$TCP_URL" \
  --fs "$FS_PATH"
