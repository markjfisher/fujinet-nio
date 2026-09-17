#!/usr/bin/env bash
set -euo pipefail
experiment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$experiment_dir/../experiment.py" --manifest "$experiment_dir/experiment.json" "$@"
