#!/usr/bin/env bash
set -euo pipefail
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Help remains read-only, including when workspace env setup would create caches.
case " $* " in
  *" --help "*|*" -h "*) exec python3 -B "$here/bridge_setup.py" install-toolchain "$@" ;;
esac
workspace=$(CDPATH= cd -- "$here/../../../../.." && pwd)
if [[ -f "$workspace/scripts/env.sh" && -d "$workspace/repos/fujinet-nio" ]]; then
  set +e
  source "$workspace/scripts/env.sh"
  env_status=$?
  set -e
  [[ "$env_status" == 0 ]] || exit "$env_status"
fi
exec python3 "$here/bridge_setup.py" install-toolchain "$@"
