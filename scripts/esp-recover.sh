#!/usr/bin/env bash
set -euo pipefail

###############################################################################
# Config
###############################################################################

PIO_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}"
ESPTOOL_PY="$HOME/.platformio/packages/tool-esptoolpy/esptool.py"
PIO_PYTHON="$HOME/.platformio/penv/bin/python"

BAUD=460800
PORT=""

###############################################################################
# Helpers
###############################################################################

function find_port() {
    if [ -n "${PORT}" ]; then
        echo "$PORT"
        return
    fi

    ports=(/dev/ttyACM* /dev/ttyUSB*)
    if [ ${#ports[@]} -eq 1 ]; then
        echo "${ports[0]}"
    else
        echo "Multiple or no serial ports found:"
        ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || true
        echo "Specify port with -p"
        exit 1
    fi
}

function esptool() {
    "$PIO_PYTHON" "$ESPTOOL_PY" \
        --chip esp32s3 \
        --baud "$BAUD" \
        --port "$(find_port)" \
        "$@"
}

###############################################################################
# Usage
###############################################################################

function usage() {
    echo ""
    echo "ESP32-S3 Recovery Utility"
    echo ""
    echo "Usage: $0 [-p PORT] command"
    echo ""
    echo "Commands:"
    echo "  chip-id        # Verify communication"
    echo "  flash-id       # Read flash info"
    echo "  erase          # Full chip erase"
    echo "  monitor        # Open serial monitor"
    echo "  write FILE     # Write raw firmware at 0x0"
    echo ""
    exit 1
}

###############################################################################
# Args
###############################################################################

while getopts "p:" flag; do
    case "$flag" in
        p) PORT=${OPTARG} ;;
        *) usage ;;
    esac
done
shift $((OPTIND -1))

[ $# -lt 1 ] && usage

CMD="$1"
shift || true

###############################################################################
# Commands
###############################################################################

case "$CMD" in

    chip-id)
        esptool chip_id
        ;;

    flash-id)
        esptool flash_id
        ;;

    erase)
        echo "Erasing entire flash..."
        esptool erase_flash
        ;;

    write)
        [ $# -ne 1 ] && { echo "Specify firmware file"; exit 1; }
        FILE="$1"
        esptool write_flash 0x0 "$FILE"
        ;;

    monitor)
        PORT=$(find_port)
        echo "Opening monitor on $PORT"
        screen "$PORT" 115200
        ;;

    *)
        usage
        ;;

esac
