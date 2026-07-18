#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MODE=${1:-auto}

case "$MODE" in
  auto|systemd|termux|foreground) ;;
  *) echo "usage: $0 [auto|systemd|termux|foreground]" >&2; exit 2 ;;
esac

mkdir -p "$PROJECT_ROOT/run/guardian"

# Starting through the launcher records ShouldRun=true. A later `devbox stop`
# records ShouldRun=false and leaves the installed guardian intentionally idle.
(cd "$PROJECT_ROOT" && node ./bin/devbox.js start >/dev/null)

if [ "$MODE" = "foreground" ]; then
  exec "$PROJECT_ROOT/scripts/run-guardian.sh"
fi

IS_TERMUX=false
case "${PREFIX:-}" in
  *com.termux*) IS_TERMUX=true ;;
esac

if [ "$MODE" = "termux" ] || { [ "$MODE" = "auto" ] && [ "$IS_TERMUX" = true ]; }; then
  BOOT_DIR="$HOME/.termux/boot"
  BOOT_SCRIPT="$BOOT_DIR/devbox-guardian"
  mkdir -p "$BOOT_DIR"
  {
    printf '%s\n' '#!/data/data/com.termux/files/usr/bin/sh'
    printf '%s\n' 'termux-wake-lock 2>/dev/null || true'
    printf 'nohup %s >>%s 2>&1 &\n' "$(printf "'%s'" "$PROJECT_ROOT/scripts/run-guardian.sh")" "$(printf "'%s'" "$PROJECT_ROOT/run/guardian/termux-boot.log")"
  } > "$BOOT_SCRIPT"
  chmod 700 "$BOOT_SCRIPT" "$PROJECT_ROOT/scripts/run-guardian.sh"
  "$BOOT_SCRIPT"
  echo "Guardian v2 installed for Termux:Boot: $BOOT_SCRIPT"
  exit 0
fi

if [ "$MODE" = "systemd" ] || { [ "$MODE" = "auto" ] && command -v systemctl >/dev/null 2>&1; }; then
  UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
  UNIT_FILE="$UNIT_DIR/devbox-guardian.service"
  NODE_EXE=$(command -v node)
  mkdir -p "$UNIT_DIR"
  {
    printf '%s\n' '[Unit]'
    printf '%s\n' 'Description=Devbox Guardian v2'
    printf '%s\n' 'After=network-online.target'
    printf '%s\n' 'Wants=network-online.target'
    printf '%s\n' ''
    printf '%s\n' '[Service]'
    printf 'WorkingDirectory=%s\n' "$PROJECT_ROOT"
    printf 'ExecStart=%s %s --project-root %s\n' "$NODE_EXE" "$PROJECT_ROOT/scripts/devbox-guardian.mjs" "$PROJECT_ROOT"
    printf '%s\n' 'Restart=on-failure'
    printf '%s\n' 'RestartSec=10'
    printf '%s\n' 'KillMode=control-group'
    printf '%s\n' ''
    printf '%s\n' '[Install]'
    printf '%s\n' 'WantedBy=default.target'
  } > "$UNIT_FILE"
  chmod 700 "$PROJECT_ROOT/scripts/run-guardian.sh"
  systemctl --user daemon-reload
  systemctl --user enable --now devbox-guardian.service
  echo "Guardian v2 installed as a systemd user service: $UNIT_FILE"
  exit 0
fi

echo "No supported service manager was detected. Run Guardian v2 in the foreground:"
echo "  $PROJECT_ROOT/scripts/run-guardian.sh"
