# Native core server bundle

`devbox-core-TARGET.zip` (Windows) or `devbox-core-TARGET.tar.gz` contains one
C++23 executable, `devbox-mcp`, plus its build manifest, dependency inventory,
licenses, SBOM and this guide. The full `devbox-TARGET` bundle remains available
and additionally includes the installer and TUI. Core uses the exact same tested
MCP executable and supports the same protocol. Optional computer-use, browser,
Docker, WSL and development tools require their corresponding OS facilities or
applications; none are installed or launched by extracting the core archive.

Node, npm, Git, Python, Rust, a compiler, a browser and a display server are not
required for the core server or its native management loop. Linux release builds
bundle GCC's C++/support runtime; the operating system's libc, networking and TLS
trust remain required. X11 libraries load only when desktop capabilities are used.

## Verified native installation

Download the archive for the correct OS/architecture, `qualification-receipt.json`
and `provenance.sigstore.json` from the same release. Extract the archive and run:

```sh
./devbox-mcp manage init --root /absolute/path/devbox \
  --receipt /downloads/qualification-receipt.json \
  --provenance /downloads/provenance.sigstore.json \
  --source RELEASE_COMMIT_SHA --target linux-musl-x86_64
./devbox-mcp manage start --root /absolute/path/devbox
./devbox-mcp manage status --root /absolute/path/devbox
```

Use `devbox-mcp.exe` and an absolute Windows path on Windows. GitHub CLI (`gh`)
is needed during signed installation/promotion verification, using the same
repository, workflow, source and hosted-runner restrictions as existing qualified
promotion. It is not used for normal startup, health checks, supervision or
shutdown. The verifier authenticates the immutable staged executable and receipt
before executing the candidate. Missing/invalid proof fails closed.

For a deliberately unqualified local development build, use
`manage init --allow-local-build --root ABSOLUTE_PATH`. Its persistent policy is
reported as `local-development`; this option cannot downgrade a signed installation.

Initialization requires a new empty directory and does not import a live
installation. State is private under `ROOT/run/state`, workspace under
`ROOT/workspace`, and management records under `ROOT/run/native`. Default binding
is `127.0.0.1:8100`. Use `--port NUMBER` or `--env-file FILE` at initialization to
configure authentication, public routing and optional capabilities. Runtime
environment is persisted in the private management configuration; ambient project
configuration cannot redirect its state to another installation.

Optional companions are supplied at initialization with `--companions FILE`.
The file contains at most eight entries, for example:

```json
[{"name":"tunnel","program":"/usr/local/bin/cloudflared", "args":["tunnel","run","devbox"],
  "environment":{},"requiredBeforeNext":false}]
```

Programs must be absolute paths; initialization pins their hashes. Each process
has its own identity, bounded logs and restart backoff. Optional `readinessUrl`
must be an HTTP numeric-loopback URL. `requiredBeforeNext` orders prerequisites.
Companion failures do not restart a healthy MCP frontend. Existing externally
managed services are not adopted by name. A recovered owned companion must match
its recorded birth identity, executable path and pinned hash.

## Lifecycle and updates

```sh
./devbox-mcp manage run --root /absolute/path/devbox     # foreground supervision
./devbox-mcp manage restart --root /absolute/path/devbox
./devbox-mcp manage stop --root /absolute/path/devbox
./devbox-mcp manage promote --root /absolute/path/devbox \
  --binary /downloads/new/devbox-mcp --receipt /downloads/new/qualification-receipt.json \
  --provenance /downloads/new/provenance.sigstore.json --source NEW_SHA --target linux-musl-x86_64
./devbox-mcp manage rollback --root /absolute/path/devbox
```

The supervisor owns only its installation and recorded process instances. An OS
lock prevents duplicate supervisors. It verifies binary hashes before launch,
checks readiness against the candidate hash and unique deployment generation,
and applies bounded restart backoff. Stop/restart/promotion close admission and
wait for the exact drain acknowledgement, active calls and durable jobs/runs.
Busy or uncertain work causes refusal; commands do not silently cancel jobs.
Compatible upgrades keep SQLite state and release the old coordinator only after
drain. Failed candidate readiness restores the previous immutable binary. State
schema/protocol changes require a separately reviewed migration. Public tunnel
health is reported separately and does not authorize restarting the MCP server.

## OS startup integration

Render an installation-specific definition without activating it:

```sh
./devbox-mcp manage service-file --root /absolute/path/devbox \
  --service systemd --output /tmp/devbox-native.service
```

Supported formats are `systemd` (user service), `launchd` (user LaunchAgent), and
`windows` (Task Scheduler XML for the actual interactive account). The Windows
interactive task supports native desktop input; a session-zero service cannot
provide the same interactive desktop. Service generation never registers,
enables, starts or replaces an existing service. Use the appropriate OS service
manager to register the reviewed file when deploying. Keep the referenced
executable at its stable path; regenerate the definition when moving or upgrading
that supervisor executable. The manager and controlled MCP may be the same file.

The packaged `devbox-setup --native-root ABSOLUTE_PATH` route accepts the same
qualification arguments and configures this native supervisor. `--repo PATH`
selects the retained source-checkout installer for development/legacy management,
which still uses Node/npm/Git. Existing production installations are not migrated
by building or publishing this release.
