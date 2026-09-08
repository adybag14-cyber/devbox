#!/bin/sh
set -eu
case "$DEVBOX_DISTRO_FAMILY" in
  apt)
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends nodejs npm git python3 ripgrep curl ca-certificates bash
    ;;
  dnf) dnf -y install nodejs npm git python3 ripgrep curl ca-certificates bash ;;
  pacman) pacman -Syu --noconfirm --needed nodejs npm git python ripgrep curl ca-certificates bash ;;
  apk)
    apk add --no-cache nodejs npm git python3 ripgrep curl ca-certificates bash \
      g++ linux-headers cmake ninja zip unzip tar pkgconf autoconf automake libtool perl
    ;;
  *) echo 'Unknown isolated distro fixture family' >&2; exit 2 ;;
esac
cat /etc/os-release
node --version
git --version
cd /input
sha256sum -c SHA256SUMS
fixture_root=$(mktemp -d /tmp/devbox-cpp-distro.XXXXXXXX)
export GIT_CONFIG_GLOBAL="$fixture_root/gitconfig"
export GIT_CONFIG_NOSYSTEM=1
export GIT_CONFIG_COUNT=0
export GH_CONFIG_DIR="$fixture_root/gh"
unset GH_TOKEN GITHUB_TOKEN GH_ENTERPRISE_TOKEN GITHUB_ENTERPRISE_TOKEN
export HOST_SHELL=/bin/bash
git clone --quiet /input/source.bundle "$fixture_root/source"
cd "$fixture_root/source"
npm ci --ignore-scripts --no-audit --no-fund
if [ "$DEVBOX_DISTRO_FAMILY" = apk ]; then
  baseline=$(node -p 'JSON.parse(require("fs").readFileSync("vcpkg.json"))["builtin-baseline"]')
  git init -q .cpp-build/vcpkg
  git -C .cpp-build/vcpkg fetch --depth 1 https://github.com/microsoft/vcpkg.git "$baseline"
  git -C .cpp-build/vcpkg checkout -q --detach FETCH_HEAD
  export VCPKG_FORCE_SYSTEM_BINARIES=1
  export VCPKG_DEFAULT_BINARY_CACHE=/cache
  export VCPKG_ROOT="$PWD/.cpp-build/vcpkg"
  sh .cpp-build/vcpkg/bootstrap-vcpkg.sh -musl -disableMetrics
  node cpp-mcp/scripts/ci-native.mjs
else
  mkdir -p .cpp-build/package/bin
  cp /input/bin/* .cpp-build/package/bin/
  chmod 0755 .cpp-build/package/bin/*
fi
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
node cpp-bootstrap/scripts/fresh-install.mjs
node cpp-bootstrap/scripts/download-install.mjs
node cpp-mcp/scripts/engine-sdk-smoke.mjs
node rust-mcp/scripts/agent-reliability-smoke.mjs
node rust-mcp/scripts/config-parity-smoke-runner.mjs
node rust-mcp/scripts/gateway-smoke-runner.mjs
node rust-mcp/scripts/oauth-smoke-runner.mjs
node rust-mcp/scripts/cloudflare-oauth-smoke-runner.mjs
node rust-mcp/scripts/disconnect-smoke-runner.mjs
node rust-mcp/scripts/smoke-runner.mjs
printf 'Native C++ distro installer and MCP checks passed.\n'
