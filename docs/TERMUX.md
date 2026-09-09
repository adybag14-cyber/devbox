# Termux and Android Support

Devbox runs directly on Android through Termux host mode. Docker is not required.

## Canonical Termux app

Use the signed Android builds from:

- Repository: <https://github.com/adybag14-cyber/termux-app>
- Releases: <https://github.com/adybag14-cyber/termux-app/releases>

The supported Devbox bootstrap binaries target Android API 21+ and cover arm64-v8a, armeabi-v7a, x86_64, and x86.

## Automatic setup

```bash
pkg install -y curl ca-certificates
curl --fail --location --output install-devbox.sh \
  https://raw.githubusercontent.com/adybag14-cyber/devbox/main/scripts/install-termux.sh
sh install-devbox.sh
```

The script detects the Android ABI, downloads `devbox-setup`, `devbox-tui`, and `devbox-mcp`, verifies each against the release `SHA256SUMS`, and installs them into `$PREFIX/bin`.

- Interactive terminal: starts the C++ TUI.
- Arguments or non-interactive stdin: runs the C++ CLI directly.

The C++ installer can install `nodejs`, `git`, `python`, `ripgrep`, `curl`, and CA certificates with `pkg`, clones/configures Devbox, installs npm dependencies, stages the bundled native server, links the command, and verifies startup. New clones select the binary's exact source commit. Existing checkouts are preserved and must match the selected binary's source identity.

To add Guardian supervision during setup, select it in the TUI or pass `--guardian`. Guardian uses Termux:Boot and requests a wake lock when available.

## Release assets

| Android ABI | Complete C++ bundle |
|---|---|
| arm64-v8a | `devbox-android-arm64-v8a.tar.gz` |
| armeabi-v7a | `devbox-android-armeabi-v7a.tar.gz` |
| x86_64 | `devbox-android-x86_64.tar.gz` |
| x86 | `devbox-android-x86.tar.gz` |

Each archive contains the server, installer, TUI, and build manifest. The three
executables are also available as standalone assets with the same Android ABI
suffix. Android builds use NDK 29.0.14206865, API 21, and static libc++.

## Existing checkout installation

```bash
pkg install -y nodejs git python ripgrep curl ca-certificates
cd "$HOME/devbox"
devbox-setup --repo "$HOME/devbox" --runtime host --guardian
```

Recommended host values:

```bash
DEVBOX_RUNTIME_MODE=host
DEVBOX_MCP_IMPLEMENTATION=cpp
HOST_WORKSPACE_PATH=$HOME/devbox/workspace
HOST_DEFAULT_WORKDIR=$HOME/devbox/workspace
HOST_SHELL=$PREFIX/bin/bash
ENABLE_HOST_EXEC=true
```

## Operational notes

- Android background limits may stop long-running processes; Termux:Boot plus a wake lock improves persistence.
- Use `HOST=127.0.0.1` for loopback-only access.
- Host mode runs with Termux app permissions and is not a container sandbox.
- Shared Android storage requires the relevant Android permission and `termux-setup-storage`.
- The TUI supports `none`, `oauth`, and `cloudflare` authentication on Termux. Public OAuth deployments require `PUBLIC_BASE_URL`; Cloudflare Access additionally requires the team domain and audience. Authentication is independent of the tunnel provider. To publish Devbox through Cloudflare Tunnel, install `cloudflared` plus `termux-services` and follow [Cloudflare Tunnel setup](./CLOUDFLARE_TUNNEL.md); `sh scripts/install-cloudflare-tunnel.sh termux` installs the persistent runit service.


## Full Termux Docker CI validation

The `C++ native runtime` workflow uses a digest-pinned official Termux x86-64 userspace image. It executes the three Android binaries produced by the NDK build, completes Termux's one-time login bootstrap, and validates the installer, MCP files/processes/jobs, persistent state, OAuth, gateway, disconnect recovery, managed startup, Guardian health, and rollback. Test configuration and containers are isolated from production.

This executes Android-native binaries in Termux userspace. It does not replace an Android emulator/device test because `termux-docker` cannot reproduce every Android framework or system-library behavior. The other three ABIs are cross-build gates, not claims of device runtime validation.
