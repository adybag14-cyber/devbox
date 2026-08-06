# Guardian v2

Guardian v2 keeps Devbox available, records machine-readable readiness, and repairs only the runtime that was selected. The same Node supervisor runs on Windows, Linux, macOS, and Termux; platform service managers only keep that foreground supervisor alive.

## Readiness model

`run/guardian/state.json` separates required readiness from optional degradation:

- `MCP healthy`: the owned MCP PID is alive and local `/healthz` succeeds
- `public tunnel healthy`: when public mode is enabled, the owned or externally managed tunnel and public `/healthz` succeed
- `selected runtime healthy`: host mode uses MCP/local readiness; Docker mode additionally requires the Docker engine and devbox container
- `optional components degraded`: a non-required component is unavailable or externally managed without local process evidence

Host mode never invokes Docker. Docker fields are `null` and `DockerProbePerformed` is `false`. Docker is probed only after `SelectedRuntime` resolves to `docker`.

Runtime selection is stored as both `RuntimeMode` (`auto`, `host`, or `docker`) and `SelectedRuntime` (`host` or `docker`). `auto` keeps a persisted selection; a healthy legacy Windows host deployment migrates to host mode instead of suddenly requiring Docker.

## Windows scheduled tasks

Run from an elevated PowerShell session:

```powershell
# Current host-native MCP and named cloudflared tunnel
.\scripts\Install-ChatGptDevboxGuardian.ps1 -Runtime host -Public -OAuth

# Docker-backed runtime
.\scripts\Install-ChatGptDevboxGuardian.ps1 -Runtime docker -Public -OAuth

# Preserve or automatically infer the current selection
.\scripts\Install-ChatGptDevboxGuardian.ps1 -Runtime auto
```

The installer creates:

- `ChatGptDevboxGuardian-Logon`
- `ChatGptDevboxGuardian-KeepAlive`
- `ChatGptDevboxMcp-ElevatedStart` (on-demand host-mode MCP start at RunLevel Highest)

On Windows host mode, MCP must stay elevated so `host_exec` inherits admin privileges and never calls `Start-Process -Verb RunAs` (that path pops a full UAC secure-desktop prompt). Guardian treats a healthy but medium-integrity MCP process as unhealthy and restarts it via the elevated repair path.

Windows launchers prefer `POWERSHELL_EXE` when configured, otherwise `C:\Program Files\PowerShell\7\pwsh.exe` when installed, and finally Windows PowerShell 5.1. `POWERSHELL_FALLBACK_EXE` can override the fallback. The scheduled-task VBS launchers resolve this policy at execution time, so removing or breaking PowerShell 7 does not strand Guardian; it can still start through the legacy 5.1 executable.

The scheduled tasks run `Ensure-ChatGptDevboxGuardian.ps1`, which checks the heartbeat and safely restarts only the verified Guardian wrapper/supervisor PIDs when stale. Guardian writes an independent heartbeat every five seconds so slow health probes do not look like supervisor death. KeepAlive uses a 60-second stale threshold and requires two consecutive stale observations before killing the verified Guardian process. `Watch-ChatGptDevboxGuardian.ps1` is a thin Windows wrapper around the portable foreground supervisor.

Windows MCP elevation inspection is tri-state. A definitive medium-integrity result is unhealthy, but a PowerShell/token-query timeout is `unknown` rather than falsely treated as unelevated. A confirmed elevation result is cached for the lifetime of that MCP PID.

Inspect current status:

```powershell
.\scripts\Get-ChatGptDevboxGuardianStatus.ps1
```

## Linux systemd user service

The installer creates and starts `~/.config/systemd/user/devbox-guardian.service`:

```bash
./scripts/install-guardian.sh systemd
systemctl --user status devbox-guardian.service
journalctl --user -u devbox-guardian.service -f
```

The unit uses `Restart=on-failure` and `KillMode=control-group`; Guardian itself owns endpoint checks, runtime classification, repair thresholds, and backoff.

## macOS launchd LaunchAgent

Install the per-user LaunchAgent with:

```bash
./scripts/install-guardian.sh launchd
launchctl print "gui/$(id -u)/com.adybag14.devbox.guardian"
```

The generated plist lives at `~/Library/LaunchAgents/com.adybag14.devbox.guardian.plist`, starts Guardian at login, keeps it alive, and writes stdout/stderr under `run/guardian/`.

## Termux:Boot

Install the Termux:Boot app, then run:

```bash
./scripts/install-guardian.sh termux
```

This creates `~/.termux/boot/devbox-guardian`, takes a Termux wake lock when available, and starts the same foreground supervisor after boot. Its startup output is appended to `run/guardian/termux-boot.log`.

## Foreground operation

For containers, minimal Linux systems, or interactive diagnosis:

```bash
./scripts/run-guardian.sh
# or
npm run guardian
```

A read-only single probe is available for deployment checks:

```bash
npm run guardian:check
```

## Repair safety and circuit breaking

Guardian waits for repeated failures before repair. A failed localhost MCP health probe uses a faster two-observation threshold; public-tunnel-only failures retain the configured threshold so normal QUIC/edge reconnect churn does not restart the MCP. Cloudflared Prometheus metrics (HA connections, request errors, total requests, and QUIC closed connections) are recorded in Guardian state when available. Docker repairs use exponential backoff and open a one-hour circuit after three consecutive failed attempts by default. The persistent decision state is in `run/guardian/repair-policy.json` and survives Guardian restarts.

Container repair first inspects the exact configured name. A stopped container is started. If it cannot start, it is replaced. A create race is re-inspected and started. An ambiguous Docker/inspect error never falls through to a conflicting `docker run`.

All repair output and state remain local under:

- `run/guardian/guardian.log`
- `run/guardian/ensure.log`
- `run/guardian/last-repair.json`
- `run/guardian/repair-policy.json`
- `run/guardian/repairs/`

`guardian.desired-state.json` continues to prevent resurrection after an intentional stop.

## Remove Windows tasks

```powershell
Unregister-ScheduledTask -TaskName 'ChatGptDevboxGuardian-Logon' -Confirm:$false
Unregister-ScheduledTask -TaskName 'ChatGptDevboxGuardian-KeepAlive' -Confirm:$false
```

For systemd, run `systemctl --user disable --now devbox-guardian.service` and remove the generated user unit. For Termux, remove only `~/.termux/boot/devbox-guardian`.
