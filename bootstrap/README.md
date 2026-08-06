# Devbox setup binary

`devbox-setup` v0.4.1 is the automation-friendly Rust bootstrap for Devbox MCP on Windows, Linux, macOS, and Termux/Android. It is also the backend used by the C++ `devbox-tui`, so there is one setup implementation rather than separate interactive and CLI installers.

It can configure an existing checkout or clone the official repository, then:

- detects Windows, Linux, macOS, or Termux
- provisions missing Node.js/npm/Git prerequisites when requested
- verifies Node.js 18+, npm, and Git
- creates `.env` without discarding existing configuration
- configures `none`, built-in connector/test OAuth, or Cloudflare Access authentication
- creates `workspace/` and `run/`
- runs `npm install` and optionally `npm link`
- starts Devbox and verifies `/healthz`
- optionally installs Guardian with `--guardian`

## Automatic prerequisite installation

| Platform | Package manager used when available |
|---|---|
| Windows | `winget` (`OpenJS.NodeJS.LTS`, `Git.Git`) |
| macOS | Homebrew (`node`, `git`) |
| Debian/Ubuntu | `apt-get` |
| Fedora/RHEL | `dnf` / `yum` |
| Arch | `pacman` |
| openSUSE | `zypper` |
| Alpine | `apk` |
| Termux | `pkg` |

Linux package installation uses `sudo` when the process is not root. Use `--skip-system-packages` if prerequisites are already managed externally.

## Runtime defaults

Host mode is recommended on Linux, macOS, and Termux. Windows selects host mode when Docker is unavailable; `auto` remains available when Docker is healthy. You can always force `--runtime host` or `--runtime docker`.

## Build and test

```bash
cargo fmt --manifest-path bootstrap/Cargo.toml -- --check
cargo test --manifest-path bootstrap/Cargo.toml
cargo build --release --manifest-path bootstrap/Cargo.toml
```

Android release targets use NDK API 21 for `aarch64-linux-android`, `armv7-linux-androideabi`, `x86_64-linux-android`, and `i686-linux-android`.

## Examples

```bash
# Existing checkout
devbox-setup --repo . --guardian

# Fresh clone into ./devbox
devbox-setup

# OAuth
devbox-setup --repo . --auth oauth --public-base-url https://mcp.example.com --guardian

# Cloudflare Access-backed OAuth
devbox-setup --repo . --auth cloudflare \
  --public-base-url https://mcp.example.com \
  --cloudflare-team-domain https://team.cloudflareaccess.com \
  --cloudflare-aud <audience> --guardian

# Automation preview
devbox-setup --repo . --runtime host --no-start --dry-run
```

For Cloudflare Tunnel transport, use `docs/CLOUDFLARE_TUNNEL.md`. If public auth is selected and `cloudflared` is missing, the bootstrap prints the correct installation command for Windows, macOS, Termux, or the detected Linux package manager.

Use `devbox-setup --help` for all options. For interactive setup, keep `devbox-tui` beside this binary and run the TUI instead.
