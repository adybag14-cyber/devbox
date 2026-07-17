# Host Compatibility

The repo now supports host mode on:

- Termux
- Linux
- macOS

## Status

### Termux
Supported and validated locally.

### Linux
Supported by the same host-mode path used on Termux:
- default runtime mode resolves to `host`
- launcher works through `npm link` + `devbox`
- host shell defaults to `/bin/sh`

### macOS
Supported by the same host-mode path used on Linux:
- default runtime mode resolves to `host`
- launcher works through `npm link` + `devbox`
- host shell defaults to `$SHELL` or `/bin/sh`

## Notes for Linux/macOS

Install:

```bash
git clone https://github.com/adybag14-cyber/devbox.git
cd devbox
cp .env.example .env
npm install
npm link
```

Start:

```bash
DEVBOX_RUNTIME_MODE=host devbox
```

Optional host-mode env overrides:

```bash
HOST_WORKSPACE_PATH=/path/to/workspace
HOST_DEFAULT_WORKDIR=/path/to/default/workdir
HOST_SHELL=/bin/bash
ENABLE_HOST_EXEC=true
```

## Docker on Linux/macOS

If you still want the old container-backed flow, force it explicitly:

```bash
DEVBOX_RUNTIME_MODE=docker
```

## Current limitation

Host mode does not sandbox read-only shell execution. `devbox_exec_readonly` is best-effort and intended for cooperative agents, not hard isolation.

## Environment loading

The CLI and `npm start` automatically load `<repo>/.env` before runtime selection and host-path resolution. Existing process environment variables take precedence over values in `.env`.

Node.js 18 or newer is required by the MCP SDK and Express dependencies.

## Linux validation

Run the complete suite and a host-mode smoke test:

```bash
npm ci
npm test
DEVBOX_RUNTIME_MODE=host node bin/devbox.js start
curl --fail http://127.0.0.1:8100/healthz
node bin/devbox.js stop
```

Host-runtime commands now use shell-specific argument conventions for PowerShell, `cmd.exe`, and POSIX shells, so an explicit `HOST_SHELL` override is handled correctly.
