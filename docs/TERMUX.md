# Termux and Android Support

Devbox runs directly on Android through Termux host mode. Docker is not required.

## Canonical Termux app

Use the signed Android builds from the repository maintained for this project:

- Repository: <https://github.com/adybag14-cyber/termux-app>
- Releases: <https://github.com/adybag14-cyber/termux-app/releases>

The canonical fork tracks `termux/termux-app`, publishes signed weekly universal APKs, supports Android API 21 or newer, and includes arm64-v8a, armeabi-v7a, x86_64, and x86 support.

Do not install a build signed by a different distributor over the canonical build. Android treats different signing keys as different update lineages, so changing source can require uninstalling the existing app and losing its private app data.

## Automatic setup

Inside the canonical Termux app:

```bash
pkg install -y curl ca-certificates
curl --fail --location --output install-devbox.sh   https://raw.githubusercontent.com/adybag14-cyber/devbox/main/scripts/install-termux.sh
sh install-devbox.sh
```

The script detects the Android ABI, downloads and SHA-256 verifies the matching `devbox-setup` binary from the latest Devbox GitHub release, installs it into `$PREFIX/bin`, and runs it.

The Rust setup program then:

1. Detects Termux from `TERMUX_VERSION` or `$PREFIX`.
2. Installs `nodejs`, `git`, `python`, `ripgrep`, `curl`, and CA certificates with `pkg`.
3. Clones Devbox into `./devbox`, unless it is run from an existing checkout.
4. Creates a Termux host-mode `.env`.
5. Installs npm dependencies and links the `devbox` command.
6. Starts the MCP service and verifies `/healthz`.

Use `--skip-system-packages` only when the required Termux packages are already installed.

## Supported Android binaries

| Android ABI | Common devices | Release asset |
|---|---|---|
| `arm64-v8a` | Most modern Android phones and tablets | `devbox-setup-android-arm64-v8a` |
| `armeabi-v7a` | Older 32-bit ARM devices | `devbox-setup-android-armeabi-v7a` |
| `x86_64` | Android emulators and some ChromeOS devices | `devbox-setup-android-x86_64` |
| `x86` | Older Android emulators/devices | `devbox-setup-android-x86` |

The binaries target Android API 21, matching the canonical Termux app minimum.

## Manual source installation

```bash
pkg install -y nodejs git python ripgrep curl ca-certificates
git clone https://github.com/adybag14-cyber/devbox.git "$HOME/devbox"
cd "$HOME/devbox"
cp .env.example .env
npm install
npm link
node bin/devbox.js start
```

Recommended configuration:

```bash
DEVBOX_RUNTIME_MODE=host
HOST_WORKSPACE_PATH=$HOME/devbox/workspace
HOST_DEFAULT_WORKDIR=$HOME/devbox/workspace
HOST_SHELL=$PREFIX/bin/bash
ENABLE_HOST_EXEC=true
```

## Service commands

```bash
devbox status
devbox restart
devbox stop
devbox run
```

Verify locally:

```bash
curl --fail http://127.0.0.1:8100/healthz
curl --fail http://127.0.0.1:8100/
```

## Android operational notes

- Keep the Termux session or wake lock active when Android background restrictions would otherwise stop the process.
- The MCP endpoint listens only on the configured interface. The default `0.0.0.0` binding is reachable from the device network; set `HOST=127.0.0.1` for loopback-only access.
- Host mode is not a container sandbox. `host_exec` operates with the same Android/Termux permissions as the app.
- Files under shared Android storage require Android storage permission and `termux-setup-storage`; Devbox does not request that permission automatically.
- Public OAuth deployments still require `PUBLIC_BASE_URL` and the appropriate authentication variables.
