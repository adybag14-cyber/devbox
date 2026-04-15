# Termux Host Mode

This repo can run directly on Termux without Docker.

## Install prerequisites

```bash
pkg update
pkg install nodejs git ripgrep python curl
```

## Install devbox

```bash
git clone https://github.com/adybag14-cyber/devbox.git ~/devbox
cd ~/devbox
cp .env.example .env
npm install
```

Recommended `.env` settings for Termux:

```bash
DEVBOX_RUNTIME_MODE=host
HOST_WORKSPACE_PATH=$HOME/devbox/workspace
HOST_DEFAULT_WORKDIR=$HOME
HOST_SHELL=/data/data/com.termux/files/usr/bin/bash
ENABLE_HOST_EXEC=true
```

If you want the `devbox` command in `$PREFIX/bin`:

```bash
cd ~/devbox
npm link
```

That installs the package bin entry so you can start it from anywhere in Termux.

## Start the service

Background service:

```bash
devbox
```

Foreground service:

```bash
devbox run
```

Check status:

```bash
devbox status
```

Stop the background service:

```bash
devbox stop
```

## Verify locally

```bash
curl http://127.0.0.1:8100/healthz
curl http://127.0.0.1:8100/
```

## Notes

- Host mode runs directly on the Termux host.
- `devbox_exec_readonly` is advisory only in host mode; it is not sandboxed like Docker mode.
- `host_exec` and `host_run_program` operate on the Termux host tools available in your PATH.
- `windows_host_*` MCP tool names still exist as compatibility aliases, but they use the same host implementation on Termux.
- If you want public OAuth flows, set `PUBLIC_BASE_URL` and the appropriate auth env vars before starting the service.
