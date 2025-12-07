#!/bin/bash

# dispatch to either build_pio.sh or build_posix.sh
# by detecting "-p" arg in the command line. If it's there, call build_posix.sh
# otherwise call build_pio.sh

if [[ "$*" == *"-p"* ]]; then
    ./tools/build_posix.sh "$@"
else
    ./tools/build_pio.sh "$@"
fi
