#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$ROOT_DIR"

if [ "$#" -eq 0 ]; then
    if [ -f tools/examples/default_patterns.json ]; then
        exec python3 -m tools.chatchat_patterns gui tools/examples/default_patterns.json
    fi
    exec python3 -m tools.chatchat_patterns gui
fi

exec python3 -m tools.chatchat_patterns "$@"
