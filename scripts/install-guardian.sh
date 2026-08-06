#!/usr/bin/env sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
MODE=${1:-auto}

case "$MODE" in
  auto|systemd|launchd|termux|foreground|background) ;;
  *) echo "usage: $0 [auto|systemd|launchd|termux|foreground|background]" >&2; exit 2 ;;
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
UNAME_S=$(uname -s 2>/dev/null || printf unknown)

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

xml_escape() {
  printf '%s' "$1" | sed \
    -e 's/&/\&amp;/g' \
    -e 's/</\&lt;/g' \
    -e 's/>/\&gt;/g' \
    -e 's/"/\&quot;/g' \
    -e "s/'/\&apos;/g"
}

if [ "$MODE" = "launchd" ] || { [ "$MODE" = "auto" ] && [ "$UNAME_S" = "Darwin" ]; }; then
  if [ "$UNAME_S" != "Darwin" ]; then
    echo "launchd mode is supported only on macOS." >&2
    exit 1
  fi
  NODE_EXE=$(command -v node || true)
  if [ -z "$NODE_EXE" ]; then
    echo "Node.js was not found in PATH." >&2
    exit 1
  fi
  LABEL="com.adybag14.devbox.guardian"
  AGENT_DIR="$HOME/Library/LaunchAgents"
  PLIST="$AGENT_DIR/$LABEL.plist"
  STDOUT_LOG="$PROJECT_ROOT/run/guardian/launchd.stdout.log"
  STDERR_LOG="$PROJECT_ROOT/run/guardian/launchd.stderr.log"
  UID_VALUE=$(id -u)
  mkdir -p "$AGENT_DIR"
  cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>$(xml_escape "$LABEL")</string>
  <key>ProgramArguments</key>
  <array>
    <string>$(xml_escape "$NODE_EXE")</string>
    <string>$(xml_escape "$PROJECT_ROOT/scripts/devbox-guardian.mjs")</string>
    <string>--project-root</string>
    <string>$(xml_escape "$PROJECT_ROOT")</string>
  </array>
  <key>WorkingDirectory</key><string>$(xml_escape "$PROJECT_ROOT")</string>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ProcessType</key><string>Background</string>
  <key>StandardOutPath</key><string>$(xml_escape "$STDOUT_LOG")</string>
  <key>StandardErrorPath</key><string>$(xml_escape "$STDERR_LOG")</string>
</dict>
</plist>
EOF
  chmod 600 "$PLIST"
  launchctl bootout "gui/$UID_VALUE" "$PLIST" >/dev/null 2>&1 || true
  if launchctl bootstrap "gui/$UID_VALUE" "$PLIST"; then
    launchctl enable "gui/$UID_VALUE/$LABEL" >/dev/null 2>&1 || true
    launchctl kickstart -k "gui/$UID_VALUE/$LABEL" >/dev/null 2>&1 || true
  else
    # Compatibility fallback for older macOS launchctl behavior.
    launchctl unload "$PLIST" >/dev/null 2>&1 || true
    launchctl load -w "$PLIST"
  fi
  echo "Guardian v2 installed as a macOS LaunchAgent: $PLIST"
  exit 0
fi

systemd_escape_exec_arg() {
  # systemd ExecStart accepts C-style quoted arguments. Escape backslashes and quotes.
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

if [ "$MODE" = "systemd" ] || { [ "$MODE" = "auto" ] && command -v systemctl >/dev/null 2>&1; }; then
  UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
  UNIT_FILE="$UNIT_DIR/devbox-guardian.service"
  NODE_EXE=$(command -v node || true)
  if [ -z "$NODE_EXE" ]; then
    echo "Node.js was not found in PATH." >&2
    exit 1
  fi
  mkdir -p "$UNIT_DIR"
  NODE_Q=$(systemd_escape_exec_arg "$NODE_EXE")
  SCRIPT_Q=$(systemd_escape_exec_arg "$PROJECT_ROOT/scripts/devbox-guardian.mjs")
  ROOT_Q=$(systemd_escape_exec_arg "$PROJECT_ROOT")
  {
    printf '%s\n' '[Unit]'
    printf '%s\n' 'Description=Devbox Guardian v2'
    printf '%s\n' 'After=network-online.target'
    printf '%s\n' 'Wants=network-online.target'
    printf '%s\n' ''
    printf '%s\n' '[Service]'
    printf 'WorkingDirectory=%s\n' "$PROJECT_ROOT"
    printf 'ExecStart="%s" "%s" --project-root "%s"\n' "$NODE_Q" "$SCRIPT_Q" "$ROOT_Q"
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

if [ "$MODE" = "background" ] || [ "$MODE" = "auto" ]; then
  LOG="$PROJECT_ROOT/run/guardian/background.log"
  nohup "$PROJECT_ROOT/scripts/run-guardian.sh" >>"$LOG" 2>&1 &
  echo "Guardian v2 started in the background (non-persistent fallback). Log: $LOG"
  echo "Install a platform service manager for automatic startup after reboot."
  exit 0
fi

echo "No supported service manager was detected. Run Guardian v2 in the foreground:"
echo "  $PROJECT_ROOT/scripts/run-guardian.sh"
