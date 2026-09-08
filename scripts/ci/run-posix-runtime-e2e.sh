#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PORT=${DEVBOX_E2E_PORT:-18180}
EXPECT_PLATFORM=${DEVBOX_E2E_EXPECT_PLATFORM:-}
if [ "${GITHUB_ACTIONS:-}" != true ] && [ "${DEVBOX_E2E_ISOLATED_CHECKOUT:-}" != 1 ]; then
  echo 'Run managed lifecycle certification only in an explicitly isolated checkout.' >&2
  exit 2
fi
mkdir -p "$ROOT_DIR/run"
WORKSPACE=$(mktemp -d "$ROOT_DIR/run/ci-workspace.XXXXXXXX")
HOST_SHELL_VALUE=${HOST_SHELL:-$(command -v bash 2>/dev/null || command -v sh)}
export CPP_MCP_EXE="${CPP_MCP_EXE:-${DEVBOX_MCP_TEST_BINARY:?Supply the verified C++ candidate}}"
export DEVBOX_MCP_IMPLEMENTATION=cpp

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
node bin/devbox.js stop >/dev/null 2>&1 || true

npm ci
node bin/devbox.js start
STATUS_OUTPUT=$(node bin/devbox.js status)
printf '%s\n' "$STATUS_OUTPUT"
printf '%s\n' "$STATUS_OUTPUT" | grep -q '^implementation: cpp$'
node scripts/ci/platform-runtime-e2e.mjs \
  --url "http://127.0.0.1:$PORT/" \
  --workspace "$WORKSPACE" \
  --expect-platform "$EXPECT_PLATFORM"

node scripts/devbox-guardian.mjs --once --no-repair

echo '=== JavaScript rollback smoke ==='
node bin/devbox.js stop
DEVBOX_MCP_IMPLEMENTATION=js node bin/devbox.js start
ROLLBACK_STATUS=$(node bin/devbox.js status)
printf '%s\n' "$ROLLBACK_STATUS"
printf '%s\n' "$ROLLBACK_STATUS" | grep -q '^implementation: js$'
curl -fsS "http://127.0.0.1:$PORT/healthz" | grep -q 'ok'

echo 'Return to the verified C++ runtime'
node bin/devbox.js stop
DEVBOX_MCP_IMPLEMENTATION=cpp node bin/devbox.js start
FINAL_STATUS=$(node bin/devbox.js status)
printf '%s\n' "$FINAL_STATUS"
printf '%s\n' "$FINAL_STATUS" | grep -q '^implementation: cpp$'
