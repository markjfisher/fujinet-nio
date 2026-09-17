#!/bin/sh
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Environment setup belongs to the workspace; help and dry-run stay read-only.
# env.sh is Bash, so use a separate Bash entry only for actual operations.
case " $* " in
  *" --help "*|*" -h "*|*" --dry-run "*) exec python3 -B "$here/../experiment.py" --manifest "$here/experiment.json" "$@" ;;
esac
workspace=$(CDPATH= cd -- "$here/../../../../../../.." && pwd)
if [ -f "$workspace/scripts/env.sh" ] && [ -d "$workspace/repos/fujinet-nio" ]; then
  exec bash -c 'source "$1/scripts/env.sh"; shift; exec python3 "$@"' bash "$workspace" "$here/../experiment.py" --manifest "$here/experiment.json" "$@"
fi
exec python3 "$here/../experiment.py" --manifest "$here/experiment.json" "$@"
