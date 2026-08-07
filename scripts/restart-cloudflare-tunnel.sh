#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MODE=${1:-auto}
SERVICE_NAME=devbox-cloudflared
LAUNCHD_LABEL=com.adybag14.devbox.cloudflared

log() { printf '%s
' "$*"; }
fail() { printf 'ERROR: %s
' "$*" >&2; exit 3; }

is_termux() {
  [ -n "${TERMUX_VERSION:-}" ] || case "${PREFIX:-}" in *com.termux/files/usr*) return 0 ;; *) return 1 ;; esac
}

restart_termux() {
  command -v sv >/dev/null 2>&1 || return 1
  sv status "$SERVICE_NAME" >/dev/null 2>&1 || return 1
  sv restart "$SERVICE_NAME"
  log "Restarted Termux Cloudflare service: $SERVICE_NAME"
}

restart_launchd() {
  command -v launchctl >/dev/null 2>&1 || return 1
  domain="gui/$(id -u)"
  launchctl print "$domain/$LAUNCHD_LABEL" >/dev/null 2>&1 || return 1
  launchctl kickstart -k "$domain/$LAUNCHD_LABEL"
  log "Restarted macOS Cloudflare LaunchAgent: $LAUNCHD_LABEL"
}

restart_systemd() {
  command -v systemctl >/dev/null 2>&1 || return 1
  systemctl --user status "$SERVICE_NAME.service" >/dev/null 2>&1 || return 1
  systemctl --user restart "$SERVICE_NAME.service"
  log "Restarted Linux Cloudflare user service: $SERVICE_NAME.service"
}

case "$MODE" in
  auto)
    if is_termux && restart_termux; then exit 0; fi
    case "$(uname -s 2>/dev/null || true)" in
      Darwin) if restart_launchd; then exit 0; fi ;;
      Linux) if restart_systemd; then exit 0; fi ;;
    esac
    ;;
  termux) restart_termux && exit 0 ;;
  launchd) restart_launchd && exit 0 ;;
  systemd) restart_systemd && exit 0 ;;
  *) fail "unknown mode '$MODE'; expected auto, termux, launchd, or systemd" ;;
esac

fail "no managed Devbox Cloudflare tunnel service was found. Re-run scripts/install-cloudflare-tunnel.sh auto or inspect $ROOT_DIR/docs/CLOUDFLARE_TUNNEL.md"
