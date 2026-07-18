# Devbox setup binary

`devbox-setup` is a dependency-free Rust bootstrap program for new Devbox MCP installations on Windows, Linux, macOS, and Termux on Android.

It can configure an existing checkout or clone the official repository into `./devbox`, then:

- detects Termux and provisions Android packages with `pkg`
- verifies Node.js 18+ and npm
- creates `.env` from `.env.example` without discarding an existing configuration
- creates `workspace/` and `run/`
- runs `npm install`
- attempts `npm link` without blocking setup if the global npm prefix is not writable
- starts the service and checks `/healthz`

Build and test on the host:

```bash
cargo test --manifest-path bootstrap/Cargo.toml
cargo build --release --manifest-path bootstrap/Cargo.toml
```

Android release targets are built with Android NDK API 21 for:

- `aarch64-linux-android`
- `armv7-linux-androideabi`
- `x86_64-linux-android`
- `i686-linux-android`

Run from an existing checkout:

```bash
./bootstrap/target/release/devbox-setup --repo .
```

Run from an empty directory to clone and configure Devbox:

```bash
devbox-setup
```

Termux users can install the matching Android binary with:

```bash
curl --fail --location --output install-devbox.sh   https://raw.githubusercontent.com/adybag14-cyber/devbox/main/scripts/install-termux.sh
sh install-devbox.sh
```

Canonical Termux app: <https://github.com/adybag14-cyber/termux-app>

Use `devbox-setup --help` for runtime, port, workspace, package-provisioning, and non-starting setup options.
