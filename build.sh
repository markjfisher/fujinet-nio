#!/usr/bin/env bash
#
# Unified build script dispatcher for FujiNet-NIO
# Routes to either POSIX (cmake) or ESP32 (PlatformIO) build systems

set -euo pipefail

function show_unified_help {
    cat <<EOF
FujiNet-NIO Build Script Dispatcher

This script routes to the appropriate build system based on arguments.

Usage: $0 [BUILD_TYPE] [OPTIONS]

BUILD_TYPE (choose one):
  -p, --posix PROFILE    Build for POSIX (Linux/macOS) using cmake
  -e, --esp32            Build for ESP32-S3 using PlatformIO (default if no type specified)
  -h, --help             Show this help message

Quick Examples:
  $0 -h                  # Show this unified help
  $0 -p PROFILE -h       # Show POSIX build help only
  $0 -e -h               # Show ESP32 build help only
  $0 -cb                 # Clean and build ESP32 (default)
  $0 -p fujibus-pty-debug -c  # Clean and build POSIX target

================================================================================
POSIX Build Help:
================================================================================

EOF
    # Extract POSIX help by running in subshell (scripts exit after help)
    (./scripts/build_posix.sh -h 2>&1) || true
    
    cat <<EOF

================================================================================
ESP32 Build Help:
================================================================================

EOF
    # Extract ESP32 help by running in subshell (scripts exit after help)
    (./scripts/build_pio.sh -h 2>&1) || true
    
    exit 0
}

# Parse arguments to determine build type and help request
HAS_POSIX_FLAG=0
HAS_ESP32_FLAG=0
HAS_HELP_FLAG=0

for arg in "$@"; do
    case "$arg" in
        -p|--posix)
            HAS_POSIX_FLAG=1
            ;;
        -e|--esp32)
            HAS_ESP32_FLAG=1
            ;;
        -h|--help)
            HAS_HELP_FLAG=1
            ;;
    esac
done

# Handle help requests
if [[ $HAS_HELP_FLAG -eq 1 ]]; then
    # If help requested with a build type, delegate to that script
    if [[ $HAS_POSIX_FLAG -eq 1 ]]; then
        ./scripts/build_posix.sh "$@"
        exit $?
    elif [[ $HAS_ESP32_FLAG -eq 1 ]]; then
        ./scripts/build_pio.sh "$@"
        exit $?
    else
        # No build type specified, show unified overview
        show_unified_help
    fi
fi

# Handle no arguments
if [[ $# -eq 0 ]]; then
    show_unified_help
fi

# Dispatch based on build type flags
if [[ $HAS_POSIX_FLAG -eq 1 ]]; then
    ./scripts/build_posix.sh "$@"
elif [[ $HAS_ESP32_FLAG -eq 1 ]]; then
    ./scripts/build_pio.sh "$@"
else
    # Default to ESP32 build for backward compatibility
    ./scripts/build_pio.sh "$@"
fi
