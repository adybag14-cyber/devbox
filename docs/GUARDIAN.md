# Guardian v2

Guardian v2 keeps Devbox available, records machine-readable readiness, and repairs only the runtime that was selected. The same Node supervisor runs on Windows, Linux, and Termux; platform service managers only keep that foreground supervisor alive.

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

The scheduled tasks run `Ensure-ChatGptDevboxGuardian.ps1`, which checks the heartbeat and safely restarts only the verified Guardian wrapper/supervisor PIDs when stale. `Watch-ChatGptDevboxGuardian.ps1` is a thin Windows wrapper around the portable foreground supervisor.

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

Guardian waits for repeated failures before repair. Docker repairs use exponential backoff and open a one-hour circuit after three consecutive failed attempts by default. The persistent decision state is in `run/guardian/repair-policy.json` and survives Guardian restarts.

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
