# Runtime dependencies

The MCP core and the new native management CLI are C++23. Beginning with bundle
0.7.0, releases include full packages and **core server archives**. The core
archive contains the exact same qualified MCP executable, with native management
commands, plus documentation, provenance, dependency inventory, SBOM and notices.
It omits the installer and TUI. See [CORE_SERVER.md](CORE_SERVER.md).

## Installation and runtime boundaries

| Component | Requirement |
|---|---|
| Core MCP, files, jobs, SQLite state, HTTP research and OAuth | Native executable, supported OS/ABI, writable state/workspace and valid TLS trust |
| Native supervision and launcher | Built into `devbox-mcp manage`; no separate interpreter |
| Signed installation/promotion | GitHub CLI to verify exact-artifact attestations; not needed during normal operation |
| OS startup integration | Generated systemd user unit, launchd LaunchAgent or Windows interactive scheduled-task definition; activation is an explicit deployment step |
| Optional supervised companions | Operator-configured absolute executable paths, pinned hashes, bounded restart policy and separate health reporting |
| Retained source-checkout setup/TUI | Node/npm/Git and the existing JavaScript launcher/Guardian compatibility workflow |
| Public Cloudflare routing | Optional `cloudflared` sidecar; not part of the core package |
| Native Windows computer use | Interactive Windows desktop and the external application being controlled |
| Native Linux computer use | X11 display, XCB/XTEST libraries, GUI application and fonts, installed only when needed |
| Headed web browsing on a server | Browser and display components; an Oracle deployment can use Chromium, Xvfb and Openbox |
| Git, Docker, WSL, language tools and user commands | Their executable/platform capability, selected for the requested work |

Existing production deployments are not automatically migrated. The legacy
source-checkout installer is explicitly selected by `--repo`; new native
installation uses a fresh directory and cannot silently overwrite an existing
deployment. Normal full bundles remain available alongside core archives.

## Native library packaging

Windows qualification uses static library linkage and the static MSVC runtime.
Linux GCC release builds additionally link C++ and GCC support runtimes
statically. Sanitizer builds retain instrumentation requirements. Android uses
the static C++ runtime. macOS retains the platform's system libraries.

CI parses imports from the actual PE/ELF/Mach-O executables. Release packaging
repeats that check and binds the import inventory to the binary digest. Normal
release binaries must import only permitted OS libraries; compiler flags alone
are not accepted as proof. Linux XCB libraries remain optional, loaded on demand
for computer use. A core package does not include a browser, display or developer
toolchain, although its MCP contract retains optional capabilities.

Bundled TLS, SQLite, parser, compression, JSON and networking libraries remain
dependencies for licensing, vulnerability inventory and security maintenance.
They have not been replaced by custom implementations to reduce the count.

## Qualification

Native management checks cover startup with an empty runtime PATH, actual MCP
file effects, ownership and duplicate suppression, bounded admission drain,
active-request/job refusal, restart/state preservation, frontend and supervisor
crash recovery, failed-candidate rollback, independent companions, proof rejection
before candidate execution, and service-file generation without activation.

Clean Debian and Alpine container gates run the server, supervisor and native
lifecycle with networking disabled and no Node/npm/Git, Python, browser or X11
packages installed. Native unit tests and existing platform, security, SDK,
Docker, desktop and signed-release gates remain required.

Core and full archives are round-trip extracted and byte-checked. Both are signed
and covered by the release checksum manifest. Compare their actual compressed
sizes and measured startup/idle memory rather than inferring performance from
language choice or dependency count.

References: [GCC linking options](https://gcc.gnu.org/onlinedocs/gcc/Link-Options.html),
[vcpkg linkage controls](https://learn.microsoft.com/en-us/vcpkg/users/triplets),
and [SQLite's embeddable amalgamation](https://www.sqlite.org/amalgamation.html).
