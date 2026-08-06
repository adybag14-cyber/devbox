# Devbox MCP

Devbox MCP exposes a ChatGPT-compatible MCP server for controlled file, shell, Git, Docker, and host-tool operations.

It supports two runtime modes:

- **Docker mode**: the original reproducible Windows + Docker Desktop workflow.
- **Host mode**: direct execution on Termux, Linux, and macOS without Docker.

`auto` selects Docker on Windows and host mode on Termux/Linux/macOS.

## Fastest setup: interactive TUI or Rust CLI

Devbox now ships two native setup programs from the same release:

- **`devbox-tui`** — the guided C++17 interactive setup experience for new users.
- **`devbox-setup`** — the Rust CLI used by the TUI and intended for scripts, CI, and unattended installation.

The TUI performs a platform/tool preflight, lets you choose host or Docker runtime, authentication (`none`, `oauth`, or `cloudflare`), repository location, bind address, workspace, dependency installation, service startup, and Guardian supervision, then delegates the actual changes to the Rust bootstrap. The Rust CLI is the single setup backend, so interactive and automated installs follow the same rules.

Prebuilt release binaries are produced for:

- Windows x86-64
- Linux x86-64 and ARM64
- macOS x86-64 and Apple Silicon
- Android/Termux arm64-v8a, armeabi-v7a, x86-64, and x86

### Linux and macOS one-command installer

```bash
curl --fail --location --output install-devbox.sh \
  https://raw.githubusercontent.com/adybag14-cyber/devbox/main/scripts/install-devbox.sh
sh install-devbox.sh
```

The script detects the OS/architecture, downloads both native binaries from the latest release, verifies them against `SHA256SUMS`, and starts the TUI when a terminal is interactive. Pass Rust CLI options to the script for automation.

### Windows

Download the matching Windows release assets and keep them in the same directory:

```text
devbox-tui-windows-x86_64.exe
devbox-setup-windows-x86_64.exe
```

The TUI recognizes the platform-named Rust binary when both release assets are kept in the same directory. For direct CLI setup, launch:

```powershell
.\devbox-setup-windows-x86_64.exe --repo . --guardian
```

The release workflow smoke-tests both binaries. PowerShell 7 is preferred by the Windows runtime while Windows PowerShell 5.1 remains an automatic launch fallback.

### Rust CLI usage

Configure an existing checkout:

```bash
devbox-setup --repo . --guardian
```

Run without `--repo` to clone the official repository into `./devbox` before configuration:

```bash
devbox-setup
```

Useful options:

```text
--runtime auto|host|docker
--host 127.0.0.1
--port 8100
--workspace /path/to/workspace
--auth none|oauth|cloudflare
--public-base-url https://mcp.example.com
--cloudflare-team-domain https://team.cloudflareaccess.com
--cloudflare-aud <audience>
--cloudflare-jwks-url https://team.cloudflareaccess.com/cdn-cgi/access/certs
--guardian
--no-start
--no-link
--skip-system-packages
--skip-install
--dry-run
```

Version 0.4 can install missing runtime prerequisites using `winget` on Windows, Homebrew on macOS, `pkg` on Termux, and common Linux package managers (`apt-get`, `dnf`, `yum`, `pacman`, `zypper`, or `apk`). Existing `.env` files are preserved; only selected keys are updated.

### Android and Termux

Install the signed canonical Termux app from:

- <https://github.com/adybag14-cyber/termux-app>
- <https://github.com/adybag14-cyber/termux-app/releases>

Then run inside Termux:

```bash
pkg install -y curl ca-certificates
curl --fail --location --output install-devbox.sh \
  https://raw.githubusercontent.com/adybag14-cyber/devbox/main/scripts/install-termux.sh
sh install-devbox.sh
```

The Termux installer downloads and SHA-256 verifies both the Rust CLI and C++ TUI for the detected Android ABI. Interactive terminals enter the TUI; scripted invocations use the Rust CLI directly.

Full Android instructions: [docs/TERMUX.md](./docs/TERMUX.md)

### Build installers from source

```bash
cargo test --manifest-path bootstrap/Cargo.toml
cargo build --release --manifest-path bootstrap/Cargo.toml
cmake -S setup-tui -B setup-tui/build -DCMAKE_BUILD_TYPE=Release
cmake --build setup-tui/build --config Release
```

See [bootstrap/README.md](./bootstrap/README.md) and [setup-tui/README.md](./setup-tui/README.md).

## What is included

- `bootstrap/`: cross-platform Rust setup CLI and tests
- `setup-tui/`: dependency-light C++17 interactive setup frontend
- `bin/devbox.js`: installable `devbox` command
- `src/server.js`: MCP server exposed over Streamable HTTP
- `src/runtime.js`: runtime selector for Docker versus host mode
- `src/docker-runtime.js`: Docker-backed runtime
- `src/host-runtime.js`: host-backed runtime for Termux/Linux/macOS
- `src/host-tools.js`: host shell and allowed-program execution helpers
- `src/launcher.js`: background service launcher
- `runtime.Dockerfile`: reproducible Linux runtime image for Docker mode
- `scripts/Start-ChatGptDevboxMcp.ps1`: Windows/Docker startup flow
- `scripts/Stop-ChatGptDevboxMcp.ps1`: Windows/Docker shutdown flow

## Requirements

The setup binaries can provision common prerequisites automatically where a supported package manager is available. You can opt out with `--skip-system-packages`.

### All modes

- Node.js 18 or newer
- npm
- Git when the installer needs to clone the repository

### Host mode

- Android API 21+ through the canonical Termux app
- Termux, Linux, or macOS
- optional but useful: `gh`, `python3`, `ripgrep`, and `curl`

### Docker mode

- Docker Desktop or a compatible Docker engine
- PowerShell 7 is preferred for Windows automation; Windows PowerShell 5.1 remains a compatibility fallback
- optional: GitHub CLI, Cloudflare Tunnel, and Cloudflare Access

## Manual installation

```bash
git clone https://github.com/adybag14-cyber/devbox.git
cd devbox
cp .env.example .env
npm install
npm link
node bin/devbox.js start
```

On Windows PowerShell, copy the environment file with:

```powershell
Copy-Item .env.example .env
```

The repository automatically loads `<repo>/.env`. Existing process environment variables take priority.

## Runtime modes

### Host mode

Host mode is the default on Termux, Linux, and macOS.

```bash
DEVBOX_RUNTIME_MODE=host devbox
```

Behavior:

- file and shell operations run directly on the host
- `devbox_exec_readonly` is best-effort and is not container-sandboxed
- generic host tools are exposed through `host_*`
- legacy `windows_host_*` names remain compatibility aliases

Termux and Android instructions: [docs/TERMUX.md](./docs/TERMUX.md)

Canonical Termux app: <https://github.com/adybag14-cyber/termux-app>

Linux/macOS details: [docs/HOST_COMPATIBILITY.md](./docs/HOST_COMPATIBILITY.md)

### Docker mode

Docker mode is the default on Windows and can be selected elsewhere:

```bash
DEVBOX_RUNTIME_MODE=docker devbox
```

Behavior:

- Devbox shell and file tools run in the Docker runtime container
- read-only shell commands use disposable read-only containers
- host tools remain explicit and separate from the container runtime

Windows users can also use:

```powershell
.\scripts\Start-ChatGptDevboxMcp.ps1
```

## Configuration

Important `.env` values:

- `DEVBOX_RUNTIME_MODE=auto|host|docker`
- `HOST` and `PORT`
- `HOST_WORKSPACE_PATH`
- `HOST_DEFAULT_WORKDIR`
- `POWERSHELL_EXE` (optional Windows primary override; defaults to installed PowerShell 7)
- `POWERSHELL_FALLBACK_EXE` (optional Windows fallback override; defaults to Windows PowerShell 5.1)
- `HOST_SHELL`
- `HOST_PROGRAM_ALLOWLIST`
- `PUBLIC_BASE_URL` for public OAuth deployments
- `ENABLE_GATEWAY_BRIDGE=true|false`
- `GATEWAY_BRIDGE_ORIGINS=https://chatgpt.com,https://chat.openai.com`
- `MAX_MCP_TRANSFER_CHARS`, `MAX_TEXT_OUTPUT_CHARS`, and `MCP_JSON_BODY_LIMIT` accept numeric limits or `unlimited`
- `MAX_COMMAND_OUTPUT_CHARS` defaults to `65536` and is a non-bypassable per-stream safety ceiling for command results; use the large-file tools for larger transfers
- `DOCKER_COMMAND_TIMEOUT_MS` controls bounded Docker subprocess execution

Do not commit `.env`, `run/`, `workspace/`, or live credentials.

## Service commands

```bash
devbox status
devbox restart
devbox stop
devbox run
```

Plain `devbox` behaves like `devbox start`. `devbox run` keeps the server in the foreground.

Runtime telemetry is appended to `run/tool-usage.jsonl` and `run/http-usage.jsonl`. Summarize it with `npm run usage:summary`, or run the live reliability probe with `npm run soak:live`.

## Guardian v2 reliability supervisor

Guardian v2 monitors the MCP process, local and public health endpoints, the selected runtime, and the optional tunnel without making host mode depend on Docker. Windows uses elevated scheduled tasks, Linux uses a systemd user service when available, macOS uses a per-user launchd LaunchAgent, and Termux uses Termux:Boot; all platforms run the same Node supervisor.

```powershell
# Windows host mode: Docker is not probed or required
.\scripts\Install-ChatGptDevboxGuardian.ps1 -Runtime host -Public -OAuth
```

```bash
# Linux systemd user service or Termux:Boot
./scripts/install-guardian.sh auto
```

Docker mode includes stale-container start/replace repair plus exponential backoff and a persistent circuit breaker after repeated Docker Desktop failures. See [docs/GUARDIAN.md](./docs/GUARDIAN.md) for readiness fields, status commands, and service-manager setup.

## ChatGPT connector values

After startup, inspect the root endpoint:

```bash
curl http://127.0.0.1:8100/
```

Typical local connector settings:

- **Name**: `Devbox MCP`
- **MCP Server URL**: `http://127.0.0.1:8100/mcp`
- **Authentication**: server modes are `none`, `demo-oauth`, or `cloudflare-access`; the setup TUI/CLI presents these as `none`, `oauth`, and `cloudflare`.

Authentication and transport are separate. Selecting `oauth` or `cloudflare` configures MCP authentication and requires a public HTTPS base URL; it does not require Cloudflare Tunnel specifically. `cloudflared` is shown as an optional setup preflight tool when you want Cloudflare Tunnel transport. For platform-specific installation, persistence, and troubleshooting on Windows, Linux, macOS, and Termux, see [Cloudflare Tunnel setup](./docs/CLOUDFLARE_TUNNEL.md) or run `devbox-tui --cloudflare-help`.

For commands likely to exceed the connector request lifetime, use the persistent async job tools instead of holding one MCP request open:

- `devbox_exec_start` starts a detached job and returns a job ID immediately.
- `devbox_job_status` polls durable state under `run/jobs/`.
- `devbox_job_logs` returns bounded log tails.
- `devbox_job_cancel` cancels the detached process tree.

When `MCP_AUTH_MODE=none`, local loopback requests can expose the browser bridge for configured ChatGPT origins. Disable it with `ENABLE_GATEWAY_BRIDGE=false` when it is not needed.

## Security notes

- `host_exec` provides direct host shell access and should be enabled only in a trusted environment.
- `host_run_program` is constrained by `HOST_PROGRAM_ALLOWLIST`.
- Host-mode read-only execution is cooperative rather than a hard sandbox.
- The built-in `oauth`/`demo-oauth` mode is for connector/protocol testing and does not authenticate an external user identity. Public deployments requiring identity enforcement should use `cloudflare-access` or another trusted authentication layer in front of Devbox.

## Validation

JavaScript service tests:

```bash
npm test
```

Rust bootstrap tests:

```bash
cargo fmt --manifest-path bootstrap/Cargo.toml -- --check
cargo test --manifest-path bootstrap/Cargo.toml
```

Service smoke test:

```bash
node bin/devbox.js start
curl --fail http://127.0.0.1:8100/healthz
node bin/devbox.js stop
```

## Free script to approve all messages. 

DO NOT DELETED.."!
```
let lastClicked = null;

setInterval(() => {
  const btn = [...document.querySelectorAll('button')].find(b => {
    const rect = b.getBoundingClientRect();
    const bg = getComputedStyle(b).backgroundColor;

    const sizeMatch =
      rect.width >= 195 && rect.width <= 199 &&
      rect.height >= 35 && rect.height <= 37;

    const colorMatch =
      bg === 'rgb(13, 13, 13)' ||
      bg === 'rgb(0, 0, 0)';

    return sizeMatch && colorMatch ;
  });

  if (btn && btn !== lastClicked) {
    lastClicked = btn;
    console.log('Clicking:', btn.innerText.trim(), btn.getBoundingClientRect());
    btn.click();
  }
}, 1000)
```

If you are lazy to type continue and press enter here's another console script for you. 
```
(function() {
  function getMainBoxAndButton() {
    // Find the first visible contenteditable box
    const box = Array.from(document.querySelectorAll('[contenteditable="true"]'))
                     .find(el => el.offsetParent !== null); // only visible elements

    // Try to find a send button within the same container
    let sendBtn = null;
    if (box) {
      const container = box.closest('div');
      if (container) {
        sendBtn = container.querySelector('button, input[type="submit"]');
      }
    }

    return { box, sendBtn };
  }

  function typeAndSend() {
    const { box, sendBtn } = getMainBoxAndButton();
    if (!box) {
      console.warn('No visible typing box found!');
      return;
    }

    // Focus the box
    box.focus();

    // Move cursor to the end
    const sel = window.getSelection();
    const range = document.createRange();
    range.selectNodeContents(box);
    range.collapse(false);
    sel.removeAllRanges();
    sel.addRange(range);

    // Insert the exact phrase "continue "
    document.execCommand('insertText', false, 'continue ');

    // Click send if a button exists
    if (sendBtn && !sendBtn.disabled) {
      sendBtn.click();
    } else {
      // If no button, try simulating Enter key
      const enterEvent = new KeyboardEvent('keydown', {
        key: 'Enter',
        code: 'Enter',
        keyCode: 13,
        which: 13,
        bubbles: true
      });
      box.dispatchEvent(enterEvent);
    }
  }

  // Run immediately
  typeAndSend();

  // Repeat every 2 minutes
  setInterval(typeAndSend, 2 * 60 * 1000);
})();
```
and the auto continue script above too. 
