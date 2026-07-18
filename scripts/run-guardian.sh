#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

exec node "$PROJECT_ROOT/scripts/devbox-guardian.mjs" --project-root "$PROJECT_ROOT" "$@"
