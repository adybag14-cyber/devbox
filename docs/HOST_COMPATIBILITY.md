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
