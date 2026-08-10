#!/usr/bin/env sh
set -eu

IMAGE=${1:?usage: run-linux-distro-container-e2e.sh IMAGE FAMILY}
FAMILY=${2:?usage: run-linux-distro-container-e2e.sh IMAGE FAMILY}
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

case "$FAMILY" in
  apt)
    INSTALL='apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends nodejs npm git python3 ripgrep curl ca-certificates bash rustc cargo && rm -rf /var/lib/apt/lists/*'
    ;;
  dnf)
    INSTALL='dnf -y install nodejs npm git python3 ripgrep curl ca-certificates bash rust cargo && dnf clean all'
    ;;
  apk)
    INSTALL='apk add --no-cache nodejs npm git python3 ripgrep curl ca-certificates bash rust cargo'
    ;;
  pacman)
    INSTALL='pacman -Syu --noconfirm --needed nodejs npm git python ripgrep curl ca-certificates bash rust cargo'
    ;;
  *)
    echo "Unsupported package family: $FAMILY" >&2
    exit 2
    ;;
esac

echo "Pulling runtime image: $IMAGE"
docker pull "$IMAGE"
docker image inspect "$IMAGE" --format 'image={{index .RepoDigests 0}} created={{.Created}}'

docker run --rm \
  -e DEVBOX_E2E_EXPECT_PLATFORM=linux \
  -e DEVBOX_E2E_PORT=18180 \
  -v "$ROOT_DIR:/source:ro" \
  "$IMAGE" \
  sh -lc "set -eu
    $INSTALL
    rm -rf /tmp/devbox-e2e
    mkdir -p /tmp/devbox-e2e
    cp -a /source/. /tmp/devbox-e2e/
    cd /tmp/devbox-e2e
    echo '=== distribution ==='
    cat /etc/os-release || true
    echo '=== toolchain ==='
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o /tmp/rustup-init.sh
    sh /tmp/rustup-init.sh -y --profile minimal --default-toolchain 1.97.1 --no-modify-path
    rm -f /tmp/rustup-init.sh
    export PATH="\$HOME/.cargo/bin:\$PATH"
    node --version
    npm --version
    git --version
    rustc --version
    cargo --version
    DEVBOX_E2E_EXPECT_PLATFORM=linux DEVBOX_E2E_PORT=18180 sh scripts/ci/run-posix-runtime-e2e.sh
  "
