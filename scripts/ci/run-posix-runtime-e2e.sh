#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PORT=${DEVBOX_E2E_PORT:-18180}
EXPECT_PLATFORM=${DEVBOX_E2E_EXPECT_PLATFORM:-}
WORKSPACE=${DEVBOX_E2E_WORKSPACE:-"$ROOT_DIR/.ci-platform-workspace"}
HOST_SHELL_VALUE=${HOST_SHELL:-$(command -v bash 2>/dev/null || command -v sh)}

export HOST=127.0.0.1
export PORT
export MCP_AUTH_MODE=none
export PUBLIC_BASE_URL=
export DEVBOX_RUNTIME_MODE=host
export HOST_WORKSPACE_PATH="$WORKSPACE"
export HOST_DEFAULT_WORKDIR="$WORKSPACE"
export HOST_SHELL="$HOST_SHELL_VALUE"
export ENABLE_HOST_EXEC=true

cleanup() {
  node "$ROOT_DIR/bin/devbox.js" stop >/dev/null 2>&1 || true
  rm -rf "$WORKSPACE"
}
trap cleanup EXIT INT TERM

cd "$ROOT_DIR"
sh -n scripts/install-cloudflare-tunnel.sh
sh -n scripts/restart-cloudflare-tunnel.sh
sh scripts/ci/test-cloudflare-tunnel-errors.sh
rm -rf "$WORKSPACE"
mkdir -p "$WORKSPACE"
node bin/devbox.js stop >/dev/null 2>&1 || true

npm ci
node scripts/ci/screen-capture-platform-e2e.mjs
node bin/devbox.js start
node scripts/ci/platform-runtime-e2e.mjs \
  --url "http://127.0.0.1:$PORT/" \
  --workspace "$WORKSPACE" \
  --expect-platform "$EXPECT_PLATFORM"

node scripts/devbox-guardian.mjs --once --no-repair
node bin/devbox.js status
