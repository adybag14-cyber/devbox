# C++23 dependency reduction

The scope is Devbox's own runtime and management operations. Python, Node, Git,
shells and compilers remain optional tools for user jobs and source builds;
Devbox does not implement those languages or their complete command interfaces.
Neither the full offline installer nor the lean core bundle ships Node, Python
or a Docker engine. Existing host tools are not uninstalled.

| Former reliance | Native runtime behavior |
|---|---|
| libdeflate | Bounded C++23 LZ77 and fixed/dynamic Huffman encoding, with gzip/zlib wrappers. Streaming zlib remains. |
| xauth | Private, atomic 0600 Xauthority record; local hostname/display and a random 128-bit cookie. Symlinks, unsafe owners and hard links are refused. |
| xdpyinfo | Authenticated local X11 setup handshake with response-size, deadline, cancellation and peer-identity bounds. |
| Openbox | A bounded native window manager on Devbox's private Xvfb: map/configure, stacking, focus, fullscreen, maximize, minimize/restore and WM_DELETE_WINDOW. Existing managers are never replaced. |
| D-Bus daemon | Devbox has no bus service to host. Its private browser desktop disables bus auto-launch/host discovery and needs no idle daemon. Chromium sandbox settings remain enabled. |
| Docker and docker-init | Native C++ management under the OS service manager. The host init handles orphan adoption; native process ownership/cancellation handles Devbox children. Container compatibility is optional. |
| Node/npm, Python, shells, Git, curl CLI and archive helpers | Normal installation, core startup, supervision, local file operations, network fetch and offline extraction already use C++/OS/library implementations. Optional user commands retain their existing execution controls. |
| ripgrep | Devbox's own structured file search is native. Explicit `rg` program requests prefer an installed host tool, then a verified optional bundled executable. |
| Build-only CMake/vcpkg/Boost helper packages | Remain source-build dependencies; not runtime processes or release executables. Compiler/toolchain compatibility is not reimplemented. |

The window manager is a borderless private automation desktop, not a general
human desktop environment. It advertises only implemented EWMH states. Its XCB
connection runs on one owned thread; a duplicated socket descriptor allows
cancellation to unblock a stalled server without acting on a reused descriptor.
Window/event counts are bounded. XCB/XTEST and Chromium/Xvfb remain optional
desktop dependencies. Database, TLS, HTTP, URL and parser libraries remain.

## Optional search fallback and supply-chain evidence

`cpp-mcp/bundled-ripgrep.json` pins upstream source and the build-only Rust
toolchain. The fallback is compiled without PCRE2. The inspected upstream
15.2.0 Windows archive contains PCRE2 10.45; its published security advisories
are why that prebuilt binary is not used for Devbox's package. The normal
finite-state regex engine remains available; `-P` is unavailable in the bundled
variant. Operators can supply their own maintained host build for that feature.

The native resolver never downloads a tool or changes PATH. Empty/relative PATH
entries cannot select a workspace executable as its host fallback. Missing host
tools resolve only to the package's `devbox-rg` beside the runtime or in the
installation's `bin` directory. Each fallback invocation checks the compiled-in
digest and size. Allowlist, host-execution policy, working directory, timeouts,
output limits and cancellation still apply. This does not isolate arbitrary
program execution from other processes authorized to act as the same OS user.

Hosted packaging requires the upstream integration suite and actual file search
to pass. Source/feature identity, all used Cargo packages, Rust standard-library
notices, binary imports and digests are retained. Known/unresolved OSV findings
block release. These checks do not claim that any software is permanently free
of vulnerabilities. Zvec is not bundled: its indexed vector/full-text retrieval
has a different lifecycle from a file-search executable fallback.

## Matched compression measurements

On 25 October 2026, Windows MSVC Release measurements used three ABBA cycles
(six before and six after invocations), matching payload CRCs and 25 paired
component samples per invocation. The median of six invocation medians uses
the mean of the middle two values:

| Payload | Previous libdeflate ms | Native C++23 ms | Before / after output bytes |
|---|---:|---:|---:|
| ASCII, 128 KiB | 0.13355 | 0.10565 | 4,789 / 4,665 |
| Unicode, 512 KiB | 0.39400 | 0.33115 | 12,504 / 12,372 |
| ASCII, 1 MiB | 0.74935 | 0.59920 | 37,163 / 36,399 |
| Poorly compressible, 512 KiB | 0.04910 | 0.04460 | both rejected |

These are component results, not whole-service latency claims. The new encoder
uses a bounded 192 KiB scratch allocation released after each call, small stack
arrays, and no retained/shared dictionary. Verification
includes independent decoding, randomized and skewed payloads, both wrappers,
32 KiB boundaries, exact/undersized output guards, a 128 KiB worker stack and
cancellation. An integer lower bound on prefix-code size rejects impossible
compression probes before constructing Huffman trees. Linux ASan
and UBSan additionally cover the encoder, Xauthority, private Xvfb authentication,
window lifecycle, duplicate-manager refusal and cancellation during stalled XCB
setup. The real Chromium/MCP desktop smoke test restricts runtime PATH to Xvfb
only; it checks capture and owned-browser restart without the removed helpers.

Publishing or building these changes does not itself migrate a production
installation. Oracle cutover must preserve its state/workspace paths, Chromium
sandbox and authentication, validate its particular OS/browser libraries, drain
active work, retain rollback and verify the final public MCP route.

The isolated Oracle qualification used the native service profile in
[`deploy/oracle-native`](../deploy/oracle-native/README.md), a separate database,
workspace, display and loopback port. Actual MCP calls exercised bundled `rg`,
live Apple UK HTML/aggregate offer extraction, native point scrolling (independent
DOM position changed from 0 to 360), URL typing and HTML artifact reads. Manager
and frontend crash recovery retained the desktop. The host-side processes ran
as UID 996 with no capabilities and `NoNewPrivs`; Chromium additionally used
private user/PID namespaces and seccomp. Its namespace zygote's capability was
confined to a one-user mapping, not the host namespace. Actual kernel limits were
576 MiB physical and 1 GiB combined RAM/swap. The final graceful stop took 3.14
seconds and left no owned process or mounted desktop runtime. This canary proof
does not claim production activation or a selected product's checkout price.
