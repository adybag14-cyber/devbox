#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
IMAGE=${TERMUX_DOCKER_IMAGE:-termux/termux-docker:x86_64}
PORT=${DEVBOX_E2E_PORT:-18182}

printf 'Pulling official Termux userspace image: %s\n' "$IMAGE"
docker pull "$IMAGE"
docker image inspect "$IMAGE" --format 'image={{index .RepoDigests 0}} created={{.Created}}'

docker run --rm \
  -v "$ROOT_DIR:/source:ro" \
  "$IMAGE" \
  bash -lc "set -eu
    echo '=== Termux environment ==='
    echo PREFIX=\"\$PREFIX\"
    echo TERMUX_VERSION=\"\${TERMUX_VERSION:-unset}\"
    uname -a
    test -n \"\${PREFIX:-}\"
    case \"\$PREFIX\" in *com.termux/files/usr*) ;; *) echo 'Not a Termux PREFIX' >&2; exit 1 ;; esac

    pkg update -y
    pkg install -y nodejs git python ripgrep curl ca-certificates rust clang

    echo '=== Termux toolchain ==='
    node --version
    npm --version
    git --version
    rustc --version
    clang++ --version | head -n 1

    rm -rf \"\$HOME/devbox-e2e\" \"\$HOME/devbox-e2e-workspace\"
    mkdir -p \"\$HOME/devbox-e2e\"
    cp -a /source/. \"\$HOME/devbox-e2e/\"
    cd \"\$HOME/devbox-e2e\"

    echo '=== Rust bootstrap native Termux tests ==='
    cargo test --manifest-path bootstrap/Cargo.toml
    cargo build --release --manifest-path bootstrap/Cargo.toml
    ./bootstrap/target/release/devbox-setup --version

    echo '=== Native C++ TUI in Termux ==='
    clang++ -std=c++17 -O2 -DNDEBUG setup-tui/src/main.cpp -o devbox-tui-termux-ci
    ./devbox-tui-termux-ci --version
    ./devbox-tui-termux-ci --diagnostics --no-color
    ./devbox-tui-termux-ci --cloudflare-help --no-color | tee cloudflare-help-termux.txt
    grep -F 'pkg update && pkg install cloudflared termux-services' cloudflare-help-termux.txt
    sh -n scripts/install-cloudflare-tunnel.sh
    sh -n scripts/restart-cloudflare-tunnel.sh
    sh scripts/ci/test-cloudflare-tunnel-errors.sh

    echo '=== Full bootstrap -> launcher -> MCP workflow ==='
    ./bootstrap/target/release/devbox-setup \
      --repo . \
      --runtime host \
      --skip-system-packages \
      --no-link \
      --host 127.0.0.1 \
      --port $PORT \
      --workspace \"\$HOME/devbox-e2e-workspace\"

    node scripts/ci/platform-runtime-e2e.mjs \
      --url http://127.0.0.1:$PORT/ \
      --workspace \"\$HOME/devbox-e2e-workspace\" \
      --expect-platform termux

    npm run guardian:check
    node bin/devbox.js status
    node bin/devbox.js stop
  "
