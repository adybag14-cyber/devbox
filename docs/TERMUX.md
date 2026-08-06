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

The script detects the Android ABI, downloads both `devbox-setup` and `devbox-tui`, verifies each against the release `SHA256SUMS`, and installs them into `$PREFIX/bin`.

- Interactive terminal: starts the C++ TUI.
- Arguments or non-interactive stdin: runs the Rust CLI directly.

The Rust bootstrap can install `nodejs`, `git`, `python`, `ripgrep`, `curl`, and CA certificates with `pkg`, clones/configures Devbox, installs npm dependencies, links the command, starts the service, and verifies `/healthz`.

To add Guardian supervision during setup, select it in the TUI or pass `--guardian`. Guardian uses Termux:Boot and requests a wake lock when available.

## Release assets

| Android ABI | Rust CLI | C++ TUI |
|---|---|---|
| arm64-v8a | `devbox-setup-android-arm64-v8a` | `devbox-tui-android-arm64-v8a` |
| armeabi-v7a | `devbox-setup-android-armeabi-v7a` | `devbox-tui-android-armeabi-v7a` |
| x86_64 | `devbox-setup-android-x86_64` | `devbox-tui-android-x86_64` |
| x86 | `devbox-setup-android-x86` | `devbox-tui-android-x86` |

## Manual source installation

```bash
pkg install -y nodejs git python ripgrep curl ca-certificates
git clone https://github.com/adybag14-cyber/devbox.git "$HOME/devbox"
cd "$HOME/devbox"
cp .env.example .env
npm install
npm link
DEVBOX_RUNTIME_MODE=host node bin/devbox.js start
```

Recommended host values:

```bash
DEVBOX_RUNTIME_MODE=host
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

The `Platform runtime E2E` workflow uses the official `termux/termux-docker:x86_64` userspace image. Inside that Termux environment CI installs the current Termux packages required by Devbox, builds and tests the Rust bootstrap natively, builds the C++ TUI natively, runs `devbox-setup` to perform the actual npm setup and launcher startup, connects through MCP, exercises the Devbox and host bridges, runs Guardian's read-only health check, checks launcher status, and shuts the service down cleanly.

This is materially stronger than Android cross-compilation. It still does not replace an Android emulator/device test because `termux-docker` cannot reproduce every Android framework or system-library behavior.
