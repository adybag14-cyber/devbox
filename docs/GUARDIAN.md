# Guardian Setup

The guardian keeps the MCP stack available on a Windows workstation by installing two scheduled tasks and a background watcher. It is designed to restart the stack when Docker, the MCP process, or the optional public tunnel becomes unhealthy, while staying idle after an intentional stop.

## Install

Bootstrap the repo first and make sure `.env` reflects the mode you want to keep alive.

Local-only setup:

```powershell
.\scripts\Install-ChatGptDevboxGuardian.ps1
```

Public ChatGPT connector with OAuth:

```powershell
.\scripts\Install-ChatGptDevboxGuardian.ps1 -Public -OAuth
```

The install script creates these scheduled tasks:

- `ChatGptDevboxGuardian-Logon`
- `ChatGptDevboxGuardian-KeepAlive`

It also writes guardian state under `run\guardian\` plus `run\guardian.settings.json` and `run\guardian.desired-state.json`. Those files stay local and are already ignored by Git.

## Verify

Use the status helper after install:

```powershell
.\scripts\Get-ChatGptDevboxGuardianStatus.ps1
```

Check for these signals:

- both scheduled tasks exist and show `LastTaskResult` `0`
- `guardian.pid` and `heartbeat.json` exist
- `state.json` reports `IsHealthy` as `true`
- `guardian.log` does not show repeated repair failures

## How It Behaves

- `Start-ChatGptDevboxMcp.ps1` writes `ShouldRun=true`, updates guardian settings, and starts the stack normally
- `Stop-ChatGptDevboxMcp.ps1` writes `ShouldRun=false`, so the guardian stays installed but does not immediately revive an intentionally stopped stack
- when the desired state is `true`, the watcher checks Docker, the devbox container, the MCP process, local `/healthz`, and public `/healthz` when `-Public` is enabled
- after repeated unhealthy checks, it reruns `Start-ChatGptDevboxMcp.ps1` with the stored `Public` and `OAuth` mode

Repair output is written to `run\guardian\guardian.log`, `run\guardian\ensure.log`, and timestamped files under `run\guardian\repairs\`.

## Update Or Remove

Re-run the install command any time you want to refresh the scheduled tasks or switch between local and public/OAuth mode.

If you want to remove the guardian completely, unregister both scheduled tasks:

```powershell
Unregister-ScheduledTask -TaskName 'ChatGptDevboxGuardian-Logon' -Confirm:$false
Unregister-ScheduledTask -TaskName 'ChatGptDevboxGuardian-KeepAlive' -Confirm:$false
```

You can leave `run\guardian\` in place for logs, or delete it manually after removing the tasks if you no longer need the repair history.
