# Cloudflare Tunnel setup

Devbox authentication and Cloudflare Tunnel are separate layers:

- **Authentication** decides who may call the MCP server (`none`, the built-in connector/test OAuth flow, or Cloudflare Access).
- **Cloudflare Tunnel** is an optional transport that publishes the local Devbox origin without opening an inbound port.

For a public deployment, Cloudflare Access plus Cloudflare Tunnel is the recommended Cloudflare-backed combination. The built-in `oauth` option is useful for connector/protocol testing but does **not** perform an external human identity check.

## Recommended layout

```text
ChatGPT / MCP client
        |
        v
https://mcp.example.com
        |
Cloudflare Access (optional identity policy)
        |
Cloudflare Tunnel
        |
http://127.0.0.1:8100
        |
Devbox MCP
```

The commands below assume Devbox listens on `127.0.0.1:8100`. Change the port if your `.env` uses a different `PORT`.

## 1. Create the tunnel in Cloudflare

The simplest Devbox setup is a **remotely-managed tunnel** created in the Cloudflare Zero Trust dashboard.

1. Put your domain on Cloudflare.
2. Open **Cloudflare Zero Trust -> Networks -> Tunnels**.
3. Create a Cloudflared tunnel.
4. Add a **Public Hostname**, for example `mcp.example.com`.
5. Set its service/origin to `http://127.0.0.1:8100`.
6. Copy the tunnel token Cloudflare shows for the connector.
7. Add these values to Devbox `.env`:

```env
CLOUDFLARED_TUNNEL_TOKEN=<secret tunnel token>
CLOUDFLARED_PUBLIC_HOSTNAME=mcp.example.com
PUBLIC_BASE_URL=https://mcp.example.com
```

Never commit the tunnel token. Devbox copies it to `run/host-cloudflared.tunnel-token.txt` with user-only permissions when installing a persistent POSIX service.

If you selected Cloudflare Access authentication in the TUI, also configure:

```env
MCP_AUTH_MODE=cloudflare-access
CLOUDFLARE_ACCESS_TEAM_DOMAIN=https://<team>.cloudflareaccess.com
CLOUDFLARE_ACCESS_AUD=<Access application AUD>
CLOUDFLARE_ACCESS_JWKS_URL=https://<team>.cloudflareaccess.com/cdn-cgi/access/certs
```

## 2. Install cloudflared

### Windows

```powershell
winget install --id Cloudflare.cloudflared --exact
cloudflared --version
```

Windows Devbox host mode can launch the configured named tunnel itself when `CLOUDFLARED_TUNNEL_TOKEN` and `CLOUDFLARED_PUBLIC_HOSTNAME` are set. Restart Devbox after editing `.env`.

Windows installations of `cloudflared` do not self-update; use `winget upgrade --id Cloudflare.cloudflared --exact` periodically.

### macOS

```bash
brew install cloudflared
cloudflared --version
```

Then from the Devbox checkout:

```bash
sh scripts/install-cloudflare-tunnel.sh auto
```

Devbox installs a per-user LaunchAgent named `com.adybag14.devbox.cloudflared`. Inspect it with:

```bash
launchctl print "gui/$(id -u)/com.adybag14.devbox.cloudflared"
tail -n 100 run/cloudflared-launchd.stderr.log
```

### Ubuntu / Debian

Add Cloudflare's package repository and install the package:

```bash
sudo mkdir -p --mode=0755 /usr/share/keyrings
curl -fsSL https://pkg.cloudflare.com/cloudflare-main.gpg | sudo tee /usr/share/keyrings/cloudflare-main.gpg >/dev/null
echo "deb [signed-by=/usr/share/keyrings/cloudflare-main.gpg] https://pkg.cloudflare.com/cloudflared any main" | sudo tee /etc/apt/sources.list.d/cloudflared.list
sudo apt-get update
sudo apt-get install cloudflared
```

Then install the Devbox user service:

```bash
sh scripts/install-cloudflare-tunnel.sh auto
systemctl --user status devbox-cloudflared.service
```

Logs:

```bash
journalctl --user -u devbox-cloudflared.service -n 100 --no-pager
```

### Fedora / RHEL / yum-based Linux

```bash
curl -fsSL https://pkg.cloudflare.com/cloudflared.repo | sudo tee /etc/yum.repos.d/cloudflared.repo
sudo dnf install cloudflared
# On older yum-based distributions: sudo yum install cloudflared
```

Then:

```bash
sh scripts/install-cloudflare-tunnel.sh auto
systemctl --user status devbox-cloudflared.service
```

### Arch Linux

```bash
sudo pacman -Syu cloudflared
sh scripts/install-cloudflare-tunnel.sh auto
systemctl --user status devbox-cloudflared.service
```

### openSUSE and other Linux distributions

Cloudflare's current package-repository instructions explicitly cover apt/yum families and Arch. On openSUSE or another distribution without a documented Cloudflare repository, install the matching official Linux binary from Cloudflare's downloads page and place it on `PATH`. Then run:

```bash
cloudflared --version
sh scripts/install-cloudflare-tunnel.sh auto
```

If your system does not run a user systemd session, use `foreground` mode and supervise it with the distribution's normal service manager.

### Alpine Linux

Cloudflare's current installation documentation does not provide an Alpine `apk` repository. Download the matching Linux `cloudflared` binary from Cloudflare's official downloads page, mark it executable, and put it on `PATH`, for example `/usr/local/bin/cloudflared`. Then run:

```bash
cloudflared --version
sh scripts/install-cloudflare-tunnel.sh auto
```

If the environment does not have a usable user systemd session, run:

```bash
sh scripts/install-cloudflare-tunnel.sh foreground
```

and supervise that command with the service manager used by your Alpine installation.

### Termux / Android

Install both the tunnel daemon and Termux's runit service integration:

```bash
pkg update
pkg install cloudflared termux-services
```

After installing `termux-services`, **close and reopen Termux** so its runit supervision environment starts. Then, from the Devbox checkout:

```bash
sh scripts/install-cloudflare-tunnel.sh termux
sv status devbox-cloudflared
```

The service definition is stored under:

```text
$PREFIX/var/service/devbox-cloudflared/
```

Useful recovery commands:

```bash
sv restart devbox-cloudflared
sv status devbox-cloudflared
```

For startup after an Android reboot, also install/configure Termux:Boot as described in `docs/TERMUX.md`; Devbox Guardian and the runit-managed tunnel can then be restored when the Termux environment starts.

## 3. Check the tunnel

First verify Devbox locally:

```bash
curl -f http://127.0.0.1:8100/healthz
```

Then verify DNS/public routing:

```bash
curl -i https://mcp.example.com/healthz
```

If Cloudflare Access protects the hostname, an unauthenticated public `curl` may receive an Access login/authorization response instead of the origin response. That is not the same as the tunnel being down.

The Cloudflare Tunnel metrics endpoint used by Devbox Guardian defaults to:

```text
http://127.0.0.1:20241/metrics
```

`devbox_status` surfaces HA connection count, tunnel request errors, total requests, and QUIC closed-connection information when that endpoint is reachable.

## Common errors

### `cloudflared: command not found`

Install `cloudflared` using the platform section above. You can also run:

```bash
devbox-tui --cloudflare-help
```

for a platform-specific install hint.

### `CLOUDFLARED_TUNNEL_TOKEN is missing`

Copy the connector token from **Zero Trust -> Networks -> Tunnels -> your tunnel** and place it in `.env`. Do not commit it.

### Public hostname is missing

Set both:

```env
CLOUDFLARED_PUBLIC_HOSTNAME=mcp.example.com
PUBLIC_BASE_URL=https://mcp.example.com
```

and ensure the public-hostname route in Cloudflare points to `http://127.0.0.1:8100` (or your configured Devbox port).

### Tunnel runs but the hostname returns `502 Bad Gateway`

Check the origin first:

```bash
curl -f http://127.0.0.1:8100/healthz
```

If that fails, Devbox itself is not ready. Run `devbox status` and inspect `run/devbox.log`.

If local health succeeds, inspect the tunnel service/logs and confirm the Cloudflare public-hostname origin uses the same port as Devbox.

### Linux: `systemctl --user` fails

Some containers/minimal distros do not provide a user systemd session. Use:

```bash
sh scripts/install-cloudflare-tunnel.sh foreground
```

and supervise it with the platform's normal process manager, or install/configure a user systemd session.

### Termux: `sv: command not found` or service will not start

```bash
pkg install termux-services
```

Then close and reopen Termux before running the installer again. `termux-services` needs its runit supervision environment active.

### macOS: LaunchAgent fails to load

```bash
plutil -lint ~/Library/LaunchAgents/com.adybag14.devbox.cloudflared.plist
launchctl print "gui/$(id -u)/com.adybag14.devbox.cloudflared"
tail -n 100 run/cloudflared-launchd.stderr.log
```

## Advanced: locally-managed tunnels

Cloudflare also supports locally-managed tunnels created with `cloudflared tunnel login`, `cloudflared tunnel create`, a credentials JSON file, and a local `config.yml`. Devbox's helper above intentionally uses the dashboard/token flow because it is easier to reproduce across Windows, Linux, macOS, and Termux. If you prefer local management, follow Cloudflare's locally-managed tunnel documentation and point the ingress service at the Devbox loopback URL.

## Official Cloudflare references

- Cloudflare Tunnel downloads: <https://developers.cloudflare.com/tunnel/downloads/>
- Create a locally-managed tunnel: <https://developers.cloudflare.com/tunnel/advanced/local-management/create-local-tunnel/>
- Run as a Linux service: <https://developers.cloudflare.com/tunnel/advanced/local-management/as-a-service/linux/>
- Run as a macOS service: <https://developers.cloudflare.com/tunnel/advanced/local-management/as-a-service/macos/>

## Transport resilience and IPv4/IPv6 selection

Devbox leaves Cloudflare transport/protocol selection under `cloudflared` control, but it now monitors the live HA-connection gauge. If all HA connections collapse while the local MCP remains healthy, Guardian classifies the incident as a tunnel transport failure and performs a tunnel-only recovery instead of restarting the MCP or disturbing unrelated host/WSL workloads.

The edge IP family is configurable on every supported platform:

```env
CLOUDFLARED_EDGE_IP_VERSION=auto
```

Allowed values are `auto`, `4`, and `6`. Keep `auto` unless diagnostics show a persistently unreliable IPv6 or IPv4 path. To temporarily force IPv4 while diagnosing a router/ISP IPv6 problem, set:

```env
CLOUDFLARED_EDGE_IP_VERSION=4

On Windows named tunnels you can also make the edge transport explicit. This is useful when virtualization or local network filters make UDP/QUIC unreliable:

```env
CLOUDFLARED_TRANSPORT_PROTOCOL=http2
CLOUDFLARED_EDGE_BIND_ADDRESS=192.0.2.10
```

`CLOUDFLARED_EDGE_BIND_ADDRESS` is optional and must be a local IP. If that address is no longer assigned (for example after DHCP changes), the Windows launcher deliberately falls back to normal route selection instead of leaving the tunnel permanently offline.

Tunnel-only repair does not restart the MCP server. `-TunnelOnly` implies the configured public tunnel, and Guardian performs a fresh health probe immediately before repair so a recovered MCP is not destroyed because of stale failure observations.
```

Then restart only the Cloudflare tunnel/service. Devbox writes the active Windows host selection to `run/host-cloudflared.transport.json`; POSIX service installers print the selected edge IP mode after setup.

Guardian records these fields in `run/guardian/state.json` when metrics are available:

- `CloudflaredMetrics.HaConnections`
- `CloudflaredMetrics.RequestErrors`
- `CloudflaredMetrics.TotalRequests`
- `CloudflaredMetrics.QuicClosedConnections`
- `CloudflaredMetricsDelta`
- `TunnelTransportHealthy`
- `TunnelTransportDegraded`
- `TunnelTransportReasons`

A rising QUIC-close counter is useful evidence but is not alone considered an outage. HA connections reaching zero is the stronger failure signal.
