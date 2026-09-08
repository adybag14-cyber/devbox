# Native C++ runtime rewrite

The rewrite branch implements the MCP runtime and setup CLI in C++20. The existing
C++ setup TUI and JavaScript/PowerShell launchers remain the integration surfaces.
The C++ executable runs its own HTTP server, tools, job runners, capture workers,
and Windows elevation workers; it does not invoke the Rust executable.

The candidate remains under certification. Its `--parity-report` returns
`complete: false` and `cutover_allowed: false`, and managed launch refuses it
before stopping an existing runtime. [port-status.json](../cpp-mcp/port-status.json)
records the implementation and validation status. This branch does not change the
existing production deployment.

## Compatibility authority

The frozen reference is Rust source `cd8803c81ad3a14ec3b3fa2afa0d08256975b1e9`.
Windows differential tests use a separately copied executable with SHA-256
`6b33147a1032368a293557705811047cd4be01b080000de0de7fc2f4dcad0114`.
Reference executables are development inputs and are excluded from the C++
distribution. They must never be rebuilt in, or started from, a production runtime
directory for these comparisons.

The compatibility contract includes:

- All 45 tools, schemas, annotations, result envelopes, output limits, and legacy
  aliases, with host and Docker configuration profiles.
- The deployed legacy MCP wire behavior and the reference server's negotiated
  `2026-07-28` behavior, including metadata, method/name headers, discovery, cache
  fields, and error responses.
- Persistent jobs, weighted execution slots, cancellation markers, task revisions,
  retry receipts, atomic append/replay behavior, and retained job logs.
- Persistent OAuth clients, authorization codes, access/refresh tokens, rotation,
  revocation, and Cloudflare JWKS validation.

Some legacy names have deliberately specific behavior. `windows_host_*` file
tools apply Windows lexical path rules even on POSIX, as the deployed Rust server
does. The ordinary Devbox file tools use the selected runtime's paths.

## Runtime architecture

Boost.Asio/Beast provides the native HTTP and MCP transport. Bounded worker pools
separate command execution, files, lifecycle operations, capture, and maintenance.
Passive waits leave the HTTP executor responsive. Filesystem waits with an external
deadline own their completion state, so a delayed OS operation can finish after
the caller returns without retaining the caller's coroutine.

Windows children use native process handles and Job Objects. POSIX children get
an explicit standard-I/O mapping and do not inherit listening sockets, unrelated
pipes, or private parent descriptors. Android API 21 and musl use a fork/exec path
that prepares arguments before fork and performs only signal-safe child operations.
Detached jobs own their runtime state independently of the serving process.

Standard C++ allocations are counted in production builds. Address/undefined
behavior sanitizer builds report that those counters are unavailable and identify
themselves in build metadata. Managed launch rejects sanitizer artifacts.

Capture runs in a bounded, independently killable C++ worker. Windows uses GDI,
PrintWindow/compositor fallback, and WIC JPEG encoding. The native POSIX behavior
matches the Rust capture implementation: Linux uses its external screenshot and
X11 discovery utilities; macOS discovers windows with CoreGraphics/libproc and
uses `screencapture`. macOS no longer requires Python Quartz bindings for discovery.
Platform permissions and graphical-session requirements still apply.

## Build and verification

Use CMake 3.24 or newer, a C++20 compiler, and the vcpkg revision pinned by
`vcpkg.json`. The root CMake project builds `devbox-mcp`, `devbox-setup`, the TUI,
and native regression suites. CMake installation stages all three executables in
`bin`. Windows release builds use the static MSVC runtime. Android uses the pinned
NDK and four API 21 triplets, with static libc++.

The `C++ native runtime` workflow validates Windows/MSVC, Linux/GCC, Linux/Clang
with sanitizers, Linux ARM64, both macOS architectures, and four Android ABIs. Its
integration jobs cover the supported Linux distributions, native musl, real Termux
userspace, live Docker operations, and capture of owned X11/Cocoa test windows.
These are certification targets; pending targets are listed in `port-status.json`.

The 9b3f7b9 candidate passed the full Windows, Linux ARM64, Linux Clang sanitizer,
and both macOS native jobs, plus all four Android builds. Both macOS jobs passed
native window/display capture. Linux Docker functional checks passed; fixture
ownership cleanup, Alpine build prerequisites, and Termux fixture environment
transfer required corrections before the remaining integration jobs could pass.

The focused scripts are:

- `cpp-mcp/scripts/runtime-crossover.mjs`: bidirectional Rust/C++ job, receipt,
  task-state, OAuth, restart, and cancellation compatibility.
- `cpp-mcp/scripts/wire-revision-parity.mjs`: raw HTTP differential behavior.
- `cpp-mcp/scripts/engine-sdk-smoke.mjs` and the retained SDK regression runners:
  standalone C++ protocol and tool behavior.
- `cpp-bootstrap/scripts/fresh-install.mjs` and `download-install.mjs`: real
  packaged installers, sibling discovery, exact binary staging, and checksum
  failure preservation.
- `cpp-mcp/scripts/docker-sdk-smoke.mjs`: real Docker files, processes, GitHub CLI
  integration with test credentials, recreation, and `/tmp` migration.

SDK scripts accept the native candidate through `DEVBOX_MCP_TEST_BINARY`.
Differential scripts additionally require the frozen Rust reference. Tests use
private state directories, owned process IDs, and uniquely named containers.

## Promotion requirements

Before changing the default or a production deployment, finish the platform and
managed-launcher gates, publish the complete three-binary installer artifacts,
and verify the exact committed source, executable digest, readiness, and rollback
path. The launcher stages immutable executables and promotes the manifest only
after the selected process passes readiness. Source drift, untracked build inputs,
hash mismatches, incomplete certification, or sanitizer builds fail preflight.

## C++ release packages

The installer and TUI share `cpp-bootstrap/VERSION` (0.5.0); the MCP protocol
server keeps its independent 0.3.0 version. `record-artifacts.mjs` records the
source commit/tree, compiler, dependency baseline, and hashes of all three native
executables. `package-release.mjs` requires the same clean source for every
platform, checks executable architectures, and extracts each archive to verify
its bytes. The release workflow uses these tested artifacts and refuses to
overwrite an existing versioned release.

Packages cover Windows x86-64, Linux x86-64/ARM64, macOS Intel/Apple Silicon,
Alpine musl x86-64, and the four Android ABIs. The POSIX downloader detects musl
and selects its matching package. Android cross-builds are distinguished from
the actual Termux x86-64 runtime gate. These packages are unreleased until the
complete certification workflow passes for a version tag.

Repository protection currently names the older Rust/platform checks. Transition
those required statuses to the completed C++ certification checks when landing
the rewrite; do not bypass protection or treat skipped integration jobs as passed.
