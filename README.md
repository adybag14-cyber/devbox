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
- `devbox_run_program` is the preferred fast path for a single executable with structured arguments; it avoids shell startup and quoting overhead
- `devbox_exec_readonly` is best-effort and is not container-sandboxed; use it when shell syntax such as pipelines, variables, or redirection is actually needed
- host-mode `devbox_search_files` prefers native ripgrep when available and retains the portable JS walker as a fallback; normal searches respect ignore files for speed, while `include_ignored=true` opts into exhaustive hidden/ignored content
- synchronous process tools share a bounded execution pool; detached jobs cannot consume the reserved interactive slot, while `/healthz`, status, file I/O, and Guardian remain outside the process queue; recursive scans/copies/archives/package installs are classified separately as `io-heavy` so storage pressure cannot masquerade as light interactive work
- generic host tools are exposed through `host_*`
- legacy `windows_host_*` names remain compatibility aliases
- `host_capture_display` captures the native desktop; `host_capture_window` captures the largest visible window for a PID or its child processes
- Windows window capture detects black `PrintWindow` results from GPU/DirectComposition/video/emulator surfaces and falls back to compositor-visible pixels; macOS uses CoreGraphics + `screencapture`; Linux supports X11 plus Sway/Hyprland/wlroots Wayland paths

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
- `HOST_PROGRAM_ALLOWLIST` adds entries to the canonical host defaults; use `HOST_PROGRAM_ALLOWLIST_EXTRA` for explicit local additions, and set `HOST_PROGRAM_ALLOWLIST_REPLACE=true` only when intentionally replacing the canonical defaults
  - Migration note: older deployments treated `HOST_PROGRAM_ALLOWLIST` as a full replacement. If you intentionally narrowed the defaults, set `HOST_PROGRAM_ALLOWLIST_REPLACE=true`; otherwise the configured entries are now merged with the canonical safe defaults.
- `DEVBOX_PROGRAM_ALLOWLIST` controls the structured `devbox_run_program` fast path; in host mode the executable must also be allowed by `HOST_PROGRAM_ALLOWLIST`
- `HOST_SEARCH_BACKEND=auto|rg|js` selects host search acceleration; `auto` prefers ripgrep
- `MCP_EXEC_MAX_CONCURRENT`, `MCP_EXEC_RESERVED_INTERACTIVE`, `MCP_EXEC_QUEUE_TIMEOUT_MS`, and `MCP_BACKGROUND_QUEUE_TIMEOUT_MS` tune the shared execution pool
- `MCP_WATCH_MAX_CONCURRENT` gives passive watchers such as `gh run watch` a separate pool; `MCP_EXEC_HEAVY_WEIGHT`/`MCP_EXEC_HEAVY_CAPACITY` (defaults `2`/`4`) control CPU-heavy build/browser work, while `MCP_EXEC_IO_HEAVY_WEIGHT`/`MCP_EXEC_IO_HEAVY_CAPACITY` (defaults `2`/`2`) serialize recursive scans, large copies/archives, package installs, and similar storage-intensive work
- `MCP_BACKGROUND_PRIORITY_AGE_MS` (default `30000`) prevents sustained interactive traffic from starving background jobs indefinitely; normal interactive priority remains in effect before the aging threshold
- `MCP_JOB_LOG_MAX_BYTES` and `MCP_JOB_LOG_ROTATIONS` bound detached-job stdout/stderr on disk; `MCP_JOB_HEARTBEAT_MS` and `MCP_JOB_ORPHAN_STALE_MS` control orphan detection; `MCP_JOB_RETENTION_HOURS` bounds persisted terminal-job history; `MCP_JOB_STORE_MAX_BYTES` and `MCP_JOB_STORE_MAX_TERMINAL_JOBS` apply global pressure limits that evict the oldest terminal jobs toward a low-water mark without deleting active jobs
- incremental job maintenance reports cycle number, cursor, total jobs, progress percent, and cycle age; quota accounting remains independent so a large retained history does not require a full rescan every minute
- `MCP_WAIT_MAX_SECONDS` bounds no-process waits; prefer `devbox_wait`, `devbox_wait_for_file`, or `devbox_job_status(wait_seconds=...)` over shell `sleep`/`Start-Sleep`
- `SCREEN_CAPTURE_ATTEMPT_TIMEOUT_MS`, `SCREEN_CAPTURE_RETRIES`, and `SCREEN_CAPTURE_QUEUE_TIMEOUT_MS` control serialized fail-fast screenshot capture
- `GUARDIAN_HOST_PRESSURE_SAMPLE_MS` controls diagnostic Windows CPU/memory/commit/pagefile sampling; it does not trigger repair by itself
- `DEVBOX_VERSION_CACHE_MS` controls the toolchain-version cache; Rust refreshes it in a supervised background loop so `devbox_status` remains subprocess-free while normally returning fresh versions
- `devbox_status` exposes cached scheduler/store health plus `executionStore.diskPressure`, a robust multi-sample free-space trend (`freeBytesTrendPerHour`, sample count, and window), and `operationalWarnings`; low disk percentage warns before the hard readiness floor. Warning/critical pressure serializes weighted work, and critical pressure rejects new mutating heavy/I/O-heavy work while leaving read-only inspection and cleanup available
- `npm run repo:freshness` compares `HEAD`, cached `origin/main`, and the actual remote head so a stale remote-tracking ref cannot masquerade as a current checkout
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

Runtime telemetry is appended to `run/tool-usage.jsonl` and `run/http-usage.jsonl`. Log size is tracked in memory after the first stat so normal tool calls do not perform an extra filesystem metadata lookup per telemetry event. Tool telemetry also records execution-slot queue wait where applicable. Summarize it with `npm run usage:summary`, or run the live reliability probe with `npm run soak:live`.

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

Docker mode includes stale-container start/replace repair plus exponential backoff and a persistent circuit breaker after repeated Docker Desktop failures. On Windows, the boot-critical `AtStartup` task launches the portable Node Guardian directly at Highest/S4U; logon and the 10-minute KeepAlive task retain the independent PowerShell Ensure recovery path. Windows/host startup also has single-owner lifecycle locking, a bounded startup deadline, immediate MCP PID ownership, cleanup-on-failure, and a machine-readable `run/startup-state.json` phase journal that Guardian uses to avoid racing an in-progress start. See [docs/GUARDIAN.md](./docs/GUARDIAN.md) for readiness fields, status commands, and service-manager setup.

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

- Synchronous shell tools are intentionally capped at **90 seconds** because upstream MCP/connector requests can be aborted around the two-minute mark. Do not use a long synchronous timeout for builds, exports, sleeps, or soak tests.
- `devbox_run_program` should be preferred for one executable such as `git`, `gh`, `node`, `python`, or `rg`; it bypasses the shell. Long direct programs should use `devbox_run_program_start`.
- `devbox_exec_start` starts a detached shell job and returns a job ID immediately; use `resource_class=watch|light|heavy|io-heavy` when auto-detection is not appropriate.
- Passive watchers use their own bounded pool; CPU-heavy and I/O-heavy jobs consume weighted capacity. Under disk pressure, weighted and light interactive work use separate FIFO lanes and light work is kept outside the weighted slot corridor, avoiding head-of-line stalls while preserving per-class FIFO ordering.
- `devbox_job_status(wait_seconds=...)` long-polls using a Node timer without occupying an execution slot. `devbox_wait` and host-mode `devbox_wait_for_file` likewise avoid spawning a shell just to sleep/poll.
- Detached job logs rotate at configured byte limits, and dead runners with stale heartbeats reconcile to terminal `interrupted` state.
- Synchronous shell/direct-program tools support `output_mode=head|tail|summary`, `max_output_chars`, and `max_output_lines` for bounded large-output inspection.
- `devbox_job_logs` returns bounded log tails.
- `devbox_job_cancel` cancels the detached process tree.

When `MCP_AUTH_MODE=none`, local loopback requests can expose the browser bridge for configured ChatGPT origins. Disable it with `ENABLE_GATEWAY_BRIDGE=false` when it is not needed.

## Security notes

- `host_exec` provides direct host shell access and should be enabled only in a trusted environment.
- `host_run_program` is constrained by `HOST_PROGRAM_ALLOWLIST` and is the preferred native-host fast path for a single executable.
- Persistent OAuth state automatically prunes expired authorization codes and tokens during load/persistence and bounds dynamic client registration with `MCP_OAUTH_MAX_CLIENTS` (default `256`), evicting only the oldest unreferenced clients.
- OAuth advertises capability scopes `mcp:devbox:read`, `mcp:devbox:exec`, `mcp:host:read`, `mcp:host:exec`, and `mcp:admin`. The legacy `mcp:tools` scope remains a full-access compatibility scope so existing connector sessions continue to work.
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

## Optional browser console helper: send "continue"

This helper sends `continue` immediately and every two minutes while this page remains visible. It requires exactly one visible, empty editable field inside a form and a unique Send button in that same form. It pauses while a response is running, leaves existing drafts alone, and stops on navigation. If the page structure is unsupported, it does nothing. This is a page-dependent convenience example; Devbox's persistent jobs are the supported way to keep commands running across requests.

Stop it with `window.devboxContinue.stop()`. Pasting the snippet again stops the previous instance, including any pending attempt.

<!-- devbox-auto-continue:start -->
```javascript
(() => {
  window.devboxContinue?.stop();
  clearInterval(window.continueTimer);
  const route = location.href;
  let stopped = false;
  let busy = false;
  const sendSelector = 'button[type="submit"], button[aria-label="Send message" i], button[data-testid="send-button"]';
  const stopSelector = 'button[data-testid="stop-button"], button[aria-label="Stop streaming" i], button[aria-label="Stop generating" i]';
  const visible = (el) => {
    if (!el?.isConnected || !el.getClientRects().length) return false;
    for (let parent = el; parent; parent = parent.parentElement) {
      const style = getComputedStyle(parent);
      if (style.visibility !== 'visible' || Number(style.opacity) === 0 || style.display === 'none') return false;
    }
    const rect = el.getBoundingClientRect();
    return rect.bottom > 0 && rect.right > 0 && rect.top < innerHeight && rect.left < innerWidth;
  };
  const generating = () => [...document.querySelectorAll(stopSelector)].some(visible);
  const sendButtons = (form) => [...form.querySelectorAll(sendSelector)].filter(visible);
  const active = () => !stopped && location.href === route && document.visibilityState === 'visible';
  const stop = () => {
    stopped = true;
    clearInterval(window.continueTimer);
  };
  async function tick() {
    if (location.href !== route) stop();
    if (!active() || busy || generating()) return;
    const boxes = [...document.querySelectorAll('[contenteditable]')]
      .filter(el => el.isContentEditable && visible(el));
    if (boxes.length !== 1) return;
    const box = boxes[0];
    const form = box.closest('form');
    if (!form || box.textContent.trim() || sendButtons(form).length !== 1) return;
    busy = true;
    try {
      box.focus();
      const selection = window.getSelection();
      if (!selection) return;
      const range = document.createRange();
      range.selectNodeContents(box);
      range.collapse(false);
      selection.removeAllRanges();
      selection.addRange(range);
      if (!document.execCommand('insertText', false, 'continue')) return;
      const deadline = Date.now() + 5_000;
      while (active() && Date.now() < deadline) {
        if (!visible(box) || box.closest('form') !== form || box.textContent.trim() !== 'continue' || generating()) return;
        const buttons = sendButtons(form);
        if (buttons.length !== 1) return;
        const button = buttons[0];
        if (!button.disabled && button.getAttribute('aria-disabled') !== 'true') {
          button.click();
          return;
        }
        await new Promise(resolve => setTimeout(resolve, 100));
      }
    } catch (error) {
      console.warn('Continue helper stopped this attempt:', error);
    } finally {
      busy = false;
    }
  }
  window.devboxContinue = { stop, tick };
  window.continueTimer = setInterval(tick, 2 * 60 * 1000);
  void tick();
})();
```
<!-- devbox-auto-continue:end -->


### Windows named-tunnel source binding

For Windows named Cloudflare Tunnel, `CLOUDFLARED_EDGE_BIND_ADDRESS=auto` selects the current IPv4 address on the active **physical** default-route adapter and records the interface/address in `run/host-cloudflared.transport.json`. If an explicitly configured DHCP address is no longer assigned, startup automatically resolves the replacement physical default-route IPv4 instead of leaving cloudflared bound to a stale lease.
