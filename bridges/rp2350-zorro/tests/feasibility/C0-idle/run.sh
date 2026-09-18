#!/bin/sh

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# Help/dry-run stay read-only and don't need workspace environment setup.
case " $* " in
    *" --help "*|*" -h "*|*" --dry-run "*)
        exec python3 -B "$here/../experiment.py" \
            --manifest "$here/experiment.json" "$@"
        ;;
esac

workspace=$(CDPATH= cd -- "$here/../../../../../../.." && pwd)

if [ -f "$workspace/scripts/env.sh" ] &&
   [ -d "$workspace/repos/fujinet-nio" ]; then
    exec bash -c \
        'source "$1/scripts/env.sh"; shift; exec python3 "$@"' \
        bash "$workspace" \
        "$here/../experiment.py" \
        --manifest "$here/experiment.json" "$@"
fi

exec python3 "$here/../experiment.py" \
    --manifest "$here/experiment.json" "$@"