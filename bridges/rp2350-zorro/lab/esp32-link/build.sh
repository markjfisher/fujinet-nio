#!/usr/bin/env bash
# Dedicated Story 2.3 lab build. It does not call or alter the product build.sh.
set -euo pipefail
lab_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
scenario="${1:-L0}"
shift || true
case "$scenario" in L[0-9]) ;; *) echo "usage: $0 L0..L9" >&2; exit 2;; esac
upload_port=""
if [[ "${1:-}" == "--upload" ]]; then
    upload_port="${2:-}"
    [[ -n "$upload_port" && $# -eq 2 ]] || { echo "usage: $0 L0..L9 [--upload /dev/serial/by-id/PORT]" >&2; exit 2; }
else
    [[ $# -eq 0 ]] || { echo "usage: $0 L0..L9 [--upload /dev/serial/by-id/PORT]" >&2; exit 2; }
fi
if [[ -x "${PIO:-}" ]]; then pio_bin="$PIO";
elif [[ -x "$HOME/.platformio/penv/bin/pio" ]]; then pio_bin="$HOME/.platformio/penv/bin/pio";
elif command -v pio >/dev/null; then pio_bin="$(command -v pio)";
else echo "PlatformIO not found; install it through the normal FujiNet ESP32 setup." >&2; exit 127; fi
export LINK_DEFAULT_SCENARIO="${scenario#L}"
export PLATFORMIO_CORE_DIR="${LAB_PLATFORMIO_CORE_DIR:-$lab_dir/.pio-core}"
cd "$lab_dir"
if [[ -n "$upload_port" ]]; then
    exec "$pio_bin" run -t upload --upload-port "$upload_port"
fi
exec "$pio_bin" run
