# Runtime dependencies and a minimal deployment target

The production MCP core is native C++23. A small installation can bundle its
native libraries, but that does not remove those libraries from its source,
licensing, vulnerability, or update responsibilities. This document distinguishes
the current implementation from proposed dependency reduction; it does not claim
that the managed installation is already dependency-free.

## Current boundaries

| Component | Current requirement | Reduction opportunity |
|---|---|---|
| MCP, files, jobs, state, HTTP research and OAuth | Native executable, supported OS/ABI, writable state/workspace, and valid TLS trust | Package native libraries consistently; verify runtime imports on each target |
| Managed installation and supervision | Node/npm/Git are provisioned by the current installer; launcher and Guardian use Node | Add native service management and supervision while retaining promotion and recovery guarantees |
| Public Cloudflare routing | `cloudflared` for this deployment topology | Treat it as a separate optional routing component; bundling it still retains a dependency |
| Native Windows computer use | Interactive Windows desktop and Windows APIs | Use existing OS facilities; the application being controlled remains external |
| Native Linux computer use | Local X11 display, XCB/XTEST libraries, suitable GUI application and fonts | Keep headless installations free of desktop packages; install the desktop profile only when needed |
| Headed web browsing on a server | Browser plus desktop components; Oracle currently uses Chromium, Xvfb and Openbox | Make this an explicit optional profile; shipping a browser inside an archive does not eliminate it |
| Git, Docker, WSL, language tools and user commands | The requested executable or platform capability | Discover capabilities and fail explicitly when optional tools are unavailable |

`vcpkg.json` is the build dependency inventory. The native core uses established
networking/TLS, URL parsing, HTML/XML, regular-expression, compression, JSON and
SQLite libraries. Replacing these with custom implementations solely to reduce
the dependency count would create substantial correctness and maintenance work.

The Windows qualification build uses the `x64-windows-static` triplet and a static
MSVC runtime. Other targets need their own import/ABI verification; do not infer
that every artifact is fully static. Linux XCB libraries are loaded on demand
when native computer use is enabled. The existing musl target supplies an Alpine
compatible artifact, not a promise that every feature works without OS libraries.

## Proposed minimal profiles

1. **Core:** a native MCP package for files, jobs, state and public HTTP research,
   with optional external tools accurately reflected in its capabilities.
2. **Managed:** core plus native lifecycle integration with the operating system
   and a native replacement for the remaining Guardian/launcher responsibilities.
3. **Desktop:** managed plus the browser/display/font components needed for CUA.
4. **Development:** optional Git, compilers, package managers, Docker or WSL,
   selected for the work the agent is expected to perform.

Native supervision is follow-up work, not part of the 0.6.0 implementation. An OS
restart policy alone does not replace artifact verification, admission draining,
process identity checks, state fencing, public-tunnel diagnostics, or rollback.
Those behaviors must survive any removal of the current Node layer.

Acceptance should include clean-machine startup, authenticated MCP calls,
restart/cancellation and state-recovery tests, missing-optional-tool behavior,
runtime import inspection, and desktop tests only for the desktop profile.
Measure package size, installed size, idle RSS, CPU, cold startup and operation
latency before and after. Bundled dependencies still require notices, provenance,
an accurate vulnerability inventory, and security updates.

References: [vcpkg static/dynamic linkage controls](https://learn.microsoft.com/en-us/vcpkg/users/triplets)
and [SQLite's embeddable amalgamation](https://www.sqlite.org/amalgamation.html).
