#!/usr/bin/env sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
ENV_FILE=${DEVBOX_ENV_FILE:-"$ROOT_DIR/.env"}
MODE=${1:-auto}
RUN_DIR="$ROOT_DIR/run"
TOKEN_FILE="$RUN_DIR/host-cloudflared.tunnel-token.txt"
METRICS_URL=${CLOUDFLARED_METRICS_URL:-http://127.0.0.1:20241/metrics}
DOC_URL="https://github.com/adybag14-cyber/devbox/blob/main/docs/CLOUDFLARE_TUNNEL.md"

log() { printf '%s\n' "$*"; }
fail() { printf 'ERROR: %s\n' "$*" >&2; exit 2; }

read_env_value() {
  key=$1
  [ -f "$ENV_FILE" ] || return 0
  value=$(awk -v key="$key" '
    index($0, key "=") == 1 {
      value = substr($0, length(key) + 2)
    }
    END { print value }
  ' "$ENV_FILE")
  case "$value" in
    \"*\") value=${value#\"}; value=${value%\"} ;;
    \'*\') value=${value#\'}; value=${value%\'} ;;
  esac
  printf '%s' "$value"
}

is_termux() {
  [ -n "${TERMUX_VERSION:-}" ] || case "${PREFIX:-}" in *com.termux/files/usr*) return 0 ;; *) return 1 ;; esac
}

install_hint() {
  if is_termux; then
    cat <<'EOF'
Termux install:
  pkg update
  pkg install cloudflared termux-services
Then close/reopen Termux (or start the termux-services supervision environment) and rerun this script.
EOF
    return
  fi
  case "$(uname -s 2>/dev/null || true)" in
    Darwin)
      cat <<'EOF'
macOS install:
  brew install cloudflared
EOF
      ;;
    Linux)
      if command -v apt-get >/dev/null 2>&1; then
        cat <<'EOF'
Debian/Ubuntu install (Cloudflare package repository):
  sudo mkdir -p --mode=0755 /usr/share/keyrings
  curl -fsSL https://pkg.cloudflare.com/cloudflare-main.gpg | sudo tee /usr/share/keyrings/cloudflare-main.gpg >/dev/null
  echo "deb [signed-by=/usr/share/keyrings/cloudflare-main.gpg] https://pkg.cloudflare.com/cloudflared any main" | sudo tee /etc/apt/sources.list.d/cloudflared.list
  sudo apt-get update && sudo apt-get install cloudflared
EOF
      elif command -v dnf >/dev/null 2>&1 || command -v yum >/dev/null 2>&1; then
        cat <<'EOF'
Fedora/RHEL install (Cloudflare package repository):
  curl -fsSL https://pkg.cloudflare.com/cloudflared.repo | sudo tee /etc/yum.repos.d/cloudflared.repo
  sudo dnf install cloudflared
# On yum-based systems use: sudo yum install cloudflared
EOF
      elif command -v pacman >/dev/null 2>&1; then
        cat <<'EOF'
Arch Linux install:
  sudo pacman -Syu cloudflared
EOF
      elif command -v apk >/dev/null 2>&1; then
        cat <<EOF
Alpine Linux: Cloudflare does not document an apk repository for cloudflared.
Install the matching Linux binary from Cloudflare's official downloads page, place it on PATH, then rerun:
  $DOC_URL
EOF
      else
        cat <<EOF
Install the matching cloudflared Linux binary/package from Cloudflare's official downloads page, then rerun:
  $DOC_URL
EOF
      fi
      ;;
    *) log "See $DOC_URL" ;;
  esac
}

if ! command -v cloudflared >/dev/null 2>&1; then
  printf 'ERROR: cloudflared is not installed or is not on PATH.\n' >&2
  install_hint >&2
  exit 2
fi

TOKEN=${CLOUDFLARED_TUNNEL_TOKEN:-$(read_env_value CLOUDFLARED_TUNNEL_TOKEN)}
HOSTNAME=${CLOUDFLARED_PUBLIC_HOSTNAME:-$(read_env_value CLOUDFLARED_PUBLIC_HOSTNAME)}
PUBLIC_BASE_URL=${PUBLIC_BASE_URL:-$(read_env_value PUBLIC_BASE_URL)}
PORT=${PORT:-$(read_env_value PORT)}
PORT=${PORT:-8100}

if [ -z "$TOKEN" ]; then
  cat >&2 <<EOF
ERROR: CLOUDFLARED_TUNNEL_TOKEN is missing.

Create a remotely-managed tunnel in Cloudflare Zero Trust -> Networks -> Tunnels,
add a public hostname that points to http://127.0.0.1:$PORT, copy the tunnel token,
and add it to $ENV_FILE:

  CLOUDFLARED_TUNNEL_TOKEN=<token>
  CLOUDFLARED_PUBLIC_HOSTNAME=mcp.example.com
  PUBLIC_BASE_URL=https://mcp.example.com

Do not commit the token. Full guide:
  $DOC_URL
EOF
  exit 2
fi

if [ -z "$HOSTNAME" ] && [ -z "$PUBLIC_BASE_URL" ]; then
  cat >&2 <<EOF
ERROR: the public hostname is missing.
Set CLOUDFLARED_PUBLIC_HOSTNAME and PUBLIC_BASE_URL in $ENV_FILE after creating the hostname in Cloudflare:

  CLOUDFLARED_PUBLIC_HOSTNAME=mcp.example.com
  PUBLIC_BASE_URL=https://mcp.example.com

The Cloudflare public hostname must route to http://127.0.0.1:$PORT.
Guide: $DOC_URL
EOF
  exit 2
fi

mkdir -p "$RUN_DIR"
umask 077
printf '%s' "$TOKEN" > "$TOKEN_FILE"
chmod 600 "$TOKEN_FILE" 2>/dev/null || true

CLOUDFLARED=$(command -v cloudflared)
METRICS_ADDRESS=${METRICS_URL#http://}
METRICS_ADDRESS=${METRICS_ADDRESS%%/*}

if [ "$MODE" = auto ]; then
  if is_termux; then MODE=termux
  elif [ "$(uname -s)" = Darwin ]; then MODE=launchd
  elif command -v systemctl >/dev/null 2>&1; then MODE=systemd
  else MODE=foreground
  fi
fi

case "$MODE" in
  termux)
    is_termux || fail "termux mode requested outside Termux"
    if ! command -v sv >/dev/null 2>&1 || ! command -v sv-enable >/dev/null 2>&1; then
      cat >&2 <<'EOF'
ERROR: termux-services is required for persistent tunnel supervision.
Install it with:
  pkg install termux-services
Then close and reopen Termux so runit supervision starts, and rerun this command.
EOF
      exit 2
    fi
    SERVICE_DIR="$PREFIX/var/service/devbox-cloudflared"
    mkdir -p "$SERVICE_DIR/log"
    cat > "$SERVICE_DIR/run" <<EOF
#!/data/data/com.termux/files/usr/bin/sh
exec "$CLOUDFLARED" tunnel --no-autoupdate --metrics "$METRICS_ADDRESS" run --token-file "$TOKEN_FILE"
EOF
    chmod 700 "$SERVICE_DIR/run"
    if [ -x "$PREFIX/share/termux-services/svlogger" ]; then
      ln -sf "$PREFIX/share/termux-services/svlogger" "$SERVICE_DIR/log/run"
    fi
    sv-enable devbox-cloudflared >/dev/null 2>&1 || true
    if ! sv up devbox-cloudflared; then
      cat >&2 <<'EOF'
ERROR: runit could not start devbox-cloudflared.
If termux-services was just installed, close/reopen Termux and rerun this script.
Inspect with:
  sv status devbox-cloudflared
  logcat | tail
EOF
      exit 2
    fi
    log "Cloudflare Tunnel installed as Termux service: devbox-cloudflared"
    log "Status: sv status devbox-cloudflared"
    ;;

  systemd)
    command -v systemctl >/dev/null 2>&1 || fail "systemctl is unavailable; use '$0 foreground' or see $DOC_URL"
    UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
    UNIT_FILE="$UNIT_DIR/devbox-cloudflared.service"
    mkdir -p "$UNIT_DIR"
    cat > "$UNIT_FILE" <<EOF
[Unit]
Description=Devbox Cloudflare Tunnel
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart="$CLOUDFLARED" tunnel --no-autoupdate --metrics "$METRICS_ADDRESS" run --token-file "$TOKEN_FILE"
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload
    systemctl --user enable --now devbox-cloudflared.service
    if ! systemctl --user is-active --quiet devbox-cloudflared.service; then
      cat >&2 <<'EOF'
ERROR: devbox-cloudflared.service did not become active.
Inspect it with:
  systemctl --user status devbox-cloudflared.service
  journalctl --user -u devbox-cloudflared.service -n 100 --no-pager
If your distro has no user systemd session, use foreground mode or another service manager.
EOF
      exit 2
    fi
    log "Cloudflare Tunnel installed as systemd user service: devbox-cloudflared.service"
    log "Status: systemctl --user status devbox-cloudflared.service"
    ;;

  launchd)
    [ "$(uname -s)" = Darwin ] || fail "launchd mode is only available on macOS"
    LABEL="com.adybag14.devbox.cloudflared"
    PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
    mkdir -p "$HOME/Library/LaunchAgents" "$RUN_DIR"
    xml_escape() { printf '%s' "$1" | sed 's/&/\&amp;/g; s/</\&lt;/g; s/>/\&gt;/g; s/"/\&quot;/g'; }
    CF_XML=$(xml_escape "$CLOUDFLARED")
    TOKEN_XML=$(xml_escape "$TOKEN_FILE")
    METRICS_XML=$(xml_escape "$METRICS_ADDRESS")
    OUT_XML=$(xml_escape "$RUN_DIR/cloudflared-launchd.stdout.log")
    ERR_XML=$(xml_escape "$RUN_DIR/cloudflared-launchd.stderr.log")
    cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array>
    <string>$CF_XML</string><string>tunnel</string><string>--no-autoupdate</string>
    <string>--metrics</string><string>$METRICS_XML</string>
    <string>run</string><string>--token-file</string><string>$TOKEN_XML</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>$OUT_XML</string>
  <key>StandardErrorPath</key><string>$ERR_XML</string>
</dict></plist>
EOF
    DOMAIN="gui/$(id -u)"
    launchctl bootout "$DOMAIN/$LABEL" >/dev/null 2>&1 || true
    if ! launchctl bootstrap "$DOMAIN" "$PLIST"; then
      cat >&2 <<EOF
ERROR: launchd could not bootstrap $PLIST.
Inspect the plist with:
  plutil -lint "$PLIST"
Then inspect launchd with:
  launchctl print "$DOMAIN/$LABEL"
Guide: $DOC_URL
EOF
      exit 2
    fi
    launchctl enable "$DOMAIN/$LABEL" >/dev/null 2>&1 || true
    launchctl kickstart -k "$DOMAIN/$LABEL" >/dev/null 2>&1 || true
    log "Cloudflare Tunnel installed as macOS LaunchAgent: $LABEL"
    log "Status: launchctl print '$DOMAIN/$LABEL'"
    ;;

  foreground)
    log "Starting cloudflared in the foreground. Press Ctrl-C to stop it."
    log "For persistent setup see: $DOC_URL"
    exec "$CLOUDFLARED" tunnel --no-autoupdate --metrics "$METRICS_ADDRESS" run --token-file "$TOKEN_FILE"
    ;;

  *) fail "unknown mode '$MODE'; expected auto, systemd, launchd, termux, or foreground" ;;
esac

log "Tunnel token is stored with user-only permissions at: $TOKEN_FILE"
log "Expected origin: http://127.0.0.1:$PORT"
[ -n "$PUBLIC_BASE_URL" ] && log "Configured public MCP base URL: $PUBLIC_BASE_URL"
log "If the hostname does not resolve/reach Devbox, confirm the Cloudflare Tunnel public-hostname route targets http://127.0.0.1:$PORT."
log "Full troubleshooting guide: $DOC_URL"
