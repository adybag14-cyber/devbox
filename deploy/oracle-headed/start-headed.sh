#!/bin/sh
set -eu
umask 077
export DISPLAY=:99
export XAUTHORITY=/tmp/devbox-desktop/authority
export XDG_RUNTIME_DIR=/tmp/devbox-desktop/runtime
export DEVBOX_COMPUTER_USE_X11=1
node /desktop/desktop-supervisor.mjs &
desktop=$!
trap 'kill "$desktop" 2>/dev/null || true; wait "$desktop" || true' EXIT
node bin/devbox.js start
node scripts/devbox-guardian.mjs --project-root /opt/devbox
