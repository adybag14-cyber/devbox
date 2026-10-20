#!/bin/sh
set -eu
umask 077
export DISPLAY=:99
export XAUTHORITY=/tmp/devbox-desktop/authority
export XDG_RUNTIME_DIR=/tmp/devbox-desktop/runtime
export DEVBOX_COMPUTER_USE_X11=1
unset WAYLAND_DISPLAY SESSION_MANAGER
case "${DEVBOX_DESKTOP_SUPERVISOR:-native}" in
  native)
    "${DEVBOX_DESKTOP_BINARY:-/opt/devbox/bin/native/devbox-mcp}" \
      --desktop-supervisor --project-root /opt/devbox --workspace /workspace &
    ;;
  node)
    node /desktop/desktop-supervisor.mjs &
    ;;
  *)
    echo "Unsupported desktop supervisor mode" >&2
    exit 1
    ;;
esac
desktop=$!
birth() {
  [ -r "/proc/$1/stat" ] || return 1
  value=$(cat "/proc/$1/stat") || return 1
  fields=${value##*) }
  set -- $fields
  [ "$#" -ge 20 ] || return 1
  shift 19
  printf '%s' "$1"
}
desktop_birth=$(birth "$desktop") || exit 1
cleanup() {
  current_birth=$(birth "$desktop" 2>/dev/null || true)
  if [ -n "$desktop_birth" ] && [ "$current_birth" = "$desktop_birth" ]; then
    kill "$desktop" 2>/dev/null || true
  fi
  wait "$desktop" || true
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
node bin/devbox.js start
node scripts/devbox-guardian.mjs --project-root /opt/devbox
