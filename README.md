# Devbox MCP

Devbox MCP exposes a ChatGPT-compatible MCP server for controlled file, shell, Git, Docker, and host-tool operations.

It supports two runtime modes:

- **Docker mode**: the original reproducible Windows + Docker Desktop workflow.
- **Host mode**: direct execution on Termux, Linux, and macOS without Docker.

`auto` selects Docker on Windows and host mode on Termux/Linux/macOS.

## Fastest setup: Rust bootstrap binary

The cross-platform `devbox-setup` program can clone the repository or configure an existing checkout. It verifies Node.js, creates the local configuration and working directories, installs dependencies, links the `devbox` command when permissions allow, starts the MCP service, and checks its health endpoint.

Prebuilt binaries are produced by the **Build bootstrap binaries** GitHub Actions workflow for:

- Windows x86-64
- Linux x86-64
- macOS x86-64
- macOS Apple Silicon
- Android/Termux arm64-v8a
- Android/Termux armeabi-v7a
- Android/Termux x86-64
- Android/Termux x86

Download the artifact for your operating system from the latest successful workflow run, extract it, and run it.

### Configure an existing checkout

Windows PowerShell:

```powershell
.\devbox-setup.exe --repo .
```

Linux:

```bash
chmod +x ./devbox-setup
./devbox-setup --repo .
```

macOS Apple Silicon:

```bash
chmod +x ./devbox-setup
./devbox-setup --repo .
```

### Clone and configure from an empty directory

Run the binary without `--repo`. It clones the official repository into `./devbox` and completes setup:

```bash
devbox-setup
```

Useful installer options:

```text
--runtime auto|host|docker
--host 127.0.0.1
--port 8100
--workspace /path/to/workspace
--no-start
--no-link
--skip-system-packages
--skip-install
--dry-run
```

The installer does not replace an existing `.env` with `.env.example`; it preserves existing lines and updates only explicitly selected keys.


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

The installer chooses the correct Android ABI, SHA-256 verifies and installs the matching release binary, provisions the required Termux packages, configures host mode, starts Devbox, and checks its health endpoint.

Full Android instructions: [docs/TERMUX.md](./docs/TERMUX.md)

## Build the Rust installer yourself

Rust 1.74 or newer is sufficient:

```bash
cargo test --manifest-path bootstrap/Cargo.toml
cargo build --release --manifest-path bootstrap/Cargo.toml
```

The resulting executable is:

- Windows: `bootstrap/target/release/devbox-setup.exe`
- Linux/macOS: `bootstrap/target/release/devbox-setup`

Full installer documentation: [bootstrap/README.md](./bootstrap/README.md)

## What is included

- `bootstrap/`: cross-platform Rust setup binary and tests
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

The Rust bootstrap binary still needs the runtime prerequisites used by Devbox itself.

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
- Windows PowerShell for the supplied Windows automation scripts
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
- `HOST_SHELL`
- `HOST_PROGRAM_ALLOWLIST`
- `PUBLIC_BASE_URL` for public OAuth deployments
- `ENABLE_GATEWAY_BRIDGE=true|false`
- `GATEWAY_BRIDGE_ORIGINS=https://chatgpt.com,https://chat.openai.com`
- `MAX_MCP_TRANSFER_CHARS`, `MAX_TEXT_OUTPUT_CHARS`, and `MCP_JSON_BODY_LIMIT` accept numeric limits or `unlimited`
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

Guardian v2 monitors the MCP process, local and public health endpoints, the selected runtime, and the optional tunnel without making host mode depend on Docker. Windows uses the existing scheduled-task watchdog, Linux can use a systemd user service, and Termux can use Termux:Boot; all three run the same foreground supervisor.

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
- **Authentication**: `none`, `demo-oauth`, or `cloudflare-access`

When `MCP_AUTH_MODE=none`, local loopback requests can expose the browser bridge for configured ChatGPT origins. Disable it with `ENABLE_GATEWAY_BRIDGE=false` when it is not needed.

## Security notes

- `host_exec` provides direct host shell access and should be enabled only in a trusted environment.
- `host_run_program` is constrained by `HOST_PROGRAM_ALLOWLIST`.
- Host-mode read-only execution is cooperative rather than a hard sandbox.
- Public deployments should use an appropriate OAuth mode and a properly configured public URL.

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
