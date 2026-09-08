#!/data/data/com.termux/files/usr/bin/bash
set -euo pipefail
case "${PREFIX:-}" in
  */com.termux/files/usr) ;;
  *) echo 'The Android runtime gate requires real Termux userspace' >&2; exit 2 ;;
esac
cd /input
sha256sum -c SHA256SUMS
# The Termux image enters Android through a login shell that clears Docker env.
export DEVBOX_EXPECTED_SOURCE=$(cat /input/source-id)
[[ "$DEVBOX_EXPECTED_SOURCE" =~ ^[a-f0-9]{40}$ ]]
printf 'deb https://packages.termux.dev/apt/termux-main stable main\n' > "$PREFIX/etc/apt/sources.list"
if ! apt-get update; then
  printf 'deb https://packages-cf.termux.dev/apt/termux-main stable main\n' > "$PREFIX/etc/apt/sources.list"
  apt-get update
fi
apt-get install -y nodejs git python ripgrep curl ca-certificates
node --version
npm --version
git --version
fixture_root=$(mktemp -d "$PREFIX/tmp/devbox-cpp-native.XXXXXXXX")
export GIT_CONFIG_GLOBAL="$fixture_root/gitconfig"
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_COUNT=0
export GH_CONFIG_DIR="$fixture_root/gh"
unset GH_TOKEN GITHUB_TOKEN GH_ENTERPRISE_TOKEN GITHUB_ENTERPRISE_TOKEN
export HOST_SHELL="$PREFIX/bin/bash"
export NODE_EXE="$PREFIX/bin/node"
git clone --quiet /input/source.bundle "$fixture_root/source"
cd "$fixture_root/source"
mkdir -p .cpp-build/package/bin
cp /input/bin/* .cpp-build/package/bin/
chmod 0755 .cpp-build/package/bin/*
export DEVBOX_MCP_TEST_BINARY="$PWD/.cpp-build/package/bin/devbox-mcp"
export DEVBOX_CPP_BINARY="$DEVBOX_MCP_TEST_BINARY"
export DEVBOX_SETUP_TEST_BINARY="$PWD/.cpp-build/package/bin/devbox-setup"
export DEVBOX_TUI_TEST_BINARY="$PWD/.cpp-build/package/bin/devbox-tui"
"$DEVBOX_MCP_TEST_BINARY" --build-info > .cpp-build/build-info.json
node --input-type=module -e '
  import fs from "node:fs";
  import assert from "node:assert/strict";
  const info=JSON.parse(fs.readFileSync(".cpp-build/build-info.json","utf8"));
  assert.equal(info.implementation,"cpp");
  assert.equal(info.gitSha,process.env.DEVBOX_EXPECTED_SOURCE);
  assert.equal(info.sourceDirty,false);
  console.log(JSON.stringify(info));
'
npm ci --ignore-scripts --no-audit --no-fund
node cpp-bootstrap/scripts/fresh-install.mjs
"$DEVBOX_TUI_TEST_BINARY" --cloudflare-help --no-color > .cpp-build/cloudflare-help.txt
grep -F 'pkg update && pkg install cloudflared termux-services' .cpp-build/cloudflare-help.txt
node cpp-mcp/scripts/engine-sdk-smoke.mjs
node rust-mcp/scripts/agent-reliability-smoke.mjs
node rust-mcp/scripts/config-parity-smoke-runner.mjs
node rust-mcp/scripts/gateway-smoke-runner.mjs
node rust-mcp/scripts/oauth-smoke-runner.mjs
node rust-mcp/scripts/cloudflare-oauth-smoke-runner.mjs
node rust-mcp/scripts/disconnect-smoke-runner.mjs
node rust-mcp/scripts/smoke-runner.mjs
printf 'Real Termux C++ installer, MCP and persistent-state gates passed.\n'
