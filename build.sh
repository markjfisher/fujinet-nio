#!/usr/bin/env bash
#
# Unified build script dispatcher for FujiNet-NIO
# Routes to either POSIX (cmake) or ESP32 (PlatformIO) build systems
set -euo pipefail

function show_unified_help {
    cat <<EOF
FujiNet-NIO Build Script Dispatcher

Usage: $0 [BUILD_TYPE] [OPTIONS]

BUILD_TYPE (choose one):
  -p, --posix            Build for POSIX (Linux/macOS) using cmake
  -e, --esp32            Build for ESP32-S3 using PlatformIO (default)
  -h, --help             Show this help message

Examples:
  $0 -h                  # Show this unified help
  $0 -p PROFILE -h       # Show POSIX build help
  $0 -cp PROFILE         # Clean and build POSIX target
  $0 -cb                 # Clean and build ESP32 (default)

================================================================================
POSIX Build Help:
================================================================================

EOF
    (./scripts/build_posix.sh -h 2>&1) || true
    cat <<EOF

================================================================================
ESP32 Build Help:
================================================================================

EOF
    (./scripts/build_pio.sh -h 2>&1) || true
    exit 0
}

# Detect build type flags (including in combined flags like -cp)
BUILD_TYPE=""
for arg in "$@"; do
    if [[ "$arg" =~ p ]] && [[ "$arg" =~ ^- ]]; then
        BUILD_TYPE="posix"
    elif [[ "$arg" =~ e ]] && [[ "$arg" =~ ^- ]]; then
        BUILD_TYPE="esp32"
    fi
done

# Handle help
if [[ "$*" =~ -h ]] || [[ "$*" =~ --help ]]; then
    if [[ "$BUILD_TYPE" == "posix" ]]; then
        ./scripts/build_posix.sh "$@"
    elif [[ "$BUILD_TYPE" == "esp32" ]]; then
        ./scripts/build_pio.sh "$@"
    else
        show_unified_help
    fi
    exit $?
fi

# Dispatch
if [[ "$BUILD_TYPE" == "posix" ]]; then
    ./scripts/build_posix.sh "$@"
elif [[ "$BUILD_TYPE" == "esp32" ]]; then
    ./scripts/build_pio.sh "$@"
else
    # Default to ESP32
    ./scripts/build_pio.sh "$@"
fi
