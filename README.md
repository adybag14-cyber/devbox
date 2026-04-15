# Devbox MCP

This repo runs a ChatGPT-compatible remote MCP server with two runtime modes:

- Docker mode: the original Windows + Docker Desktop workflow.
- Host mode: direct host execution for Termux, Linux, and macOS without Docker.

Host mode is the default on Termux/Linux/macOS. Docker mode remains the default on Windows.

## What is included

- `src/server.js`: MCP server exposed over Streamable HTTP
- `src/runtime.js`: runtime selector for Docker vs host mode
- `src/docker-runtime.js`: original Docker-backed devbox runtime
- `src/host-runtime.js`: host-backed runtime for Termux/Linux/macOS
- `src/host-tools.js`: host shell + allowed-program execution helpers
- `src/launcher.js`: local service launcher helpers
- `bin/devbox.js`: installable `devbox` command
- `runtime.Dockerfile`: reproducible Linux devbox image for Docker mode
- `scripts/Start-ChatGptDevboxMcp.ps1`: Windows/Docker startup flow
- `scripts/Stop-ChatGptDevboxMcp.ps1`: Windows/Docker shutdown flow

## Runtime modes

### Host mode

Use host mode when Docker is unavailable or unnecessary.

Supported today:
- Termux on Android
- Linux
- macOS

Behavior:
- shell/file operations run directly on the host
- `devbox_exec_readonly` is best-effort in host mode; it is not container-sandboxed
- host tools are exposed through `host_*` MCP tools
- legacy `windows_host_*` tool names remain as compatibility aliases

### Docker mode

Use Docker mode when you want the original reproducible container workflow.

Behavior:
- devbox shell/file tools run inside the Docker runtime container
- read-only shell commands still use disposable read-only containers
- host tools remain explicit and separate from the devbox runtime

## Requirements

### Termux / Linux / macOS host mode

- Node.js
- npm
- Git
- optional but useful: `gh`, `python3`, `ripgrep`, `curl`

### Windows Docker mode

- Windows
- Docker Desktop
- Node.js
- PowerShell
- Git
- optional: GitHub CLI, Cloudflare Tunnel, Cloudflare Access

## Clone and bootstrap

```bash
git clone https://github.com/adybag14-cyber/devbox.git
cd devbox
cp .env.example .env
npm install
```

Important `.env` values:

- `DEVBOX_RUNTIME_MODE=auto|host|docker`
- `HOST_WORKSPACE_PATH`
- `HOST_DEFAULT_WORKDIR`
- `HOST_SHELL`
- `HOST_PROGRAM_ALLOWLIST`
- `PUBLIC_BASE_URL` when using OAuth modes
- `ENABLE_GATEWAY_BRIDGE=true|false` to allow ChatGPT Web / browser clients to call the local open server from `https://chatgpt.com`
- `GATEWAY_BRIDGE_ORIGINS=https://chatgpt.com,https://chat.openai.com`

## Termux quick start

Full notes: [docs/TERMUX.md](./docs/TERMUX.md)

```bash
pkg install nodejs git ripgrep python curl
cd ~/devbox
cp .env.example .env
npm install
npm link
devbox
```

That starts the MCP service in the background and prints the local URL plus log path.

Useful commands:

```bash
devbox status
devbox restart
devbox stop
devbox run
```

`devbox run` keeps the server in the foreground. Plain `devbox` behaves like `devbox start`.

## Linux / macOS host-mode quick start

Full notes: [docs/HOST_COMPATIBILITY.md](./docs/HOST_COMPATIBILITY.md)

```bash
cd /path/to/devbox
cp .env.example .env
npm install
npm link
DEVBOX_RUNTIME_MODE=host devbox
```

If you want the old container workflow on Linux/macOS, set:

```bash
DEVBOX_RUNTIME_MODE=docker
```

## Windows Docker-mode quick start

```powershell
git clone https://github.com/adybag14-cyber/devbox.git
cd .\devbox
Copy-Item .env.example .env
npm install
.\scripts\Start-ChatGptDevboxMcp.ps1
```

## ChatGPT connector values

After the service is up, inspect:

```bash
curl http://127.0.0.1:8100/
```

Typical local values:

- `Name`: `Devbox MCP`
- `MCP Server URL`: `http://127.0.0.1:8100/mcp` locally, or your public base URL when exposed
- `Authentication`: `none`, `demo-oauth`, or `cloudflare-access`
- `gateway_bridge.enabled`: `true` for local open-server requests when browser bridging is enabled

## Local ChatGPT Web bridge

When `MCP_AUTH_MODE=none`, local loopback requests automatically expose a browser bridge for ChatGPT Web origins.
That bridge:

- returns `mcp_url` for local requests even when `PUBLIC_BASE_URL` is blank
- answers CORS preflights for configured `GATEWAY_BRIDGE_ORIGINS`
- allows secure-page browser clients to call the local open MCP server on loopback

Default allowed origins:

- `https://chatgpt.com`
- `https://chat.openai.com`

You can disable or customize it with:

```bash
ENABLE_GATEWAY_BRIDGE=false
GATEWAY_BRIDGE_ORIGINS=https://chatgpt.com,https://chat.openai.com
```

## Public URL / OAuth

If you expose the service publicly, set:

- `PUBLIC_BASE_URL`
- `MCP_AUTH_MODE=demo-oauth` or `MCP_AUTH_MODE=cloudflare-access`

Cloudflare Access details are still documented by the existing helper scripts and Windows docs.

## Security notes

- `host_exec` is high risk because it provides direct host shell access.
- `host_run_program` is limited by `HOST_PROGRAM_ALLOWLIST`.
- `windows_host_*` tools remain for compatibility, but on non-Windows hosts they route to the generic host implementation.
- Do not commit `.env`, `run/`, `workspace/`, or other live runtime state.

## Validation

```bash
npm test
node bin/devbox.js start
curl http://127.0.0.1:8100/healthz
node bin/devbox.js stop
```
