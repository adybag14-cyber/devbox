# Devbox C++ 0.6.0

Native C++ MCP server, installer and setup TUI for Windows, Linux, macOS and
Android/Termux. The installer and TUI now share version 0.6.0 from one source file.

## Reliability fixes

- Guardian cancels its execution deadline as soon as child exit is observed.
  The bounded output drain can no longer convert an already completed command
  into a timeout. Separate regressions preserve deadlines that fire before exit.
- The inherited-pipe test now verifies a live, owned grandchild and releases it
  explicitly, instead of depending on sub-second process startup on CI runners.
- Retained Rust host-file aliases now use native POSIX paths on Linux and macOS,
  preserving Windows path behavior on Windows. Parity checks verify the actual
  destination bytes and use identical starting files for both implementations.

## Included since the previous published bundle

- Native Linux X11 computer use alongside Windows native computer use, including
  screenshots, Unicode input, scrolling, dragging and observation identity checks.
- Durable SQLite state, agent-run APIs, cancellation/recovery controls and
  qualification-backed artifact promotion.
- Public web research and source-evidence tools, product-offer extraction and
  explicit freshness/coverage reporting, plus structured WSL support.

The MCP contract is version 9 with 53 advertised tools; actual availability remains
platform/configuration dependent. Research source targets are not a guarantee
that sufficient accessible sources exist for every query.

Release archives contain the native server, installer, TUI and build manifest.
Checksums, qualification receipts, vulnerability evidence and signed provenance
accompany the release assets. Installing a prebuilt bundle requires no Rust or
C++ compiler. Managed setup still uses Node/npm/Git; see
[runtime dependency profiles](../RUNTIME_DEPENDENCIES.md) for current requirements
and the proposed minimal-runtime design.

The known absolute 50 GiB disk-warning threshold remains unsuitable for smaller
filesystems and is not changed by this release. Production migration remains a
separate operation that must drain admission and preserve state.
