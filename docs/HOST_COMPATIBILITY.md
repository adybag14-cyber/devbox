# Host compatibility

The native C++ MCP service supports Windows, Linux, macOS, and Termux host mode.
Docker mode uses an external Docker engine and the same C++ HTTP/tool server.
The launcher and Guardian remain Node.js/PowerShell integrations.

## Platform matrix

| Platform | Native artifact | Runtime validation |
|---|---|---|
| Windows x86-64 | Static MSVC runtime | Native tests, MCP, process ownership, owned-window capture, managed Guardian lifecycle |
| Linux x86-64 | glibc bundle built on Ubuntu 24.04 | Ubuntu 26.04, Debian 13, Fedora 44, Arch containers; native Docker and X11 fixtures |
| Linux ARM64 | glibc bundle built on Ubuntu 24.04 ARM64 | Native tests, MCP, installer, managed launcher |
| Alpine 3.23 x86-64 | Native musl bundle | Built and exercised inside Alpine |
| macOS Intel / Apple Silicon | Native per-architecture bundle | Native tests, MCP, installer, Cocoa window/display capture, managed launcher |
| Android API 21, four ABIs | NDK 29 with static libc++ | Four cross-builds; actual Termux x86-64 userspace execution |

These are the certification gates for a release. Candidate progress is recorded
in [the rewrite status](../cpp-mcp/port-status.json). Android cross-build results
do not establish device/framework parity for every Android version or ABI.
The glibc binaries require a compatible libc/libstdc++; use the matching musl
bundle on Alpine or build from source on older Linux systems.

## Display and window screenshots

Use `host_capture_display` for the display and `host_capture_window` for a PID's
window. `host_capture_program` and the `windows_host_capture_*` aliases remain
available. Capture work runs in bounded, independently killable native workers.

Windows selects eligible visible windows from the requested process tree, skips
minimized/cloaked windows, and uses DWM frame bounds. It tries PrintWindow with
full-content and default flags and samples frame/interior luminance. Black
renderer results fall back to the visible desktop compositor. That fallback can
include occluding windows, which is reported in
`screen_fallback_may_include_occluders`. Output is JPEG.

macOS discovers on-screen PID-owned windows through CoreGraphics/libproc and
captures with `screencapture`. Screen Recording permission and an active
graphical session are required. Python Quartz bindings are not required. Both
architectures are tested against owned Cocoa windows and decoded PNG output.

Linux PID-selected capture uses X11: `xdotool --onlyvisible --pid` or `wmctrl -lp`
finds candidates, `xwininfo` supplies geometry, and ImageMagick `import` captures
the largest candidate. Full-display capture tries Wayland `grim`, then
`gnome-screenshot`, `scrot`, or ImageMagick `import`. The output is PNG.
The native runtime does not implement arbitrary PID-based enumeration of pure
Wayland windows. XWayland applications can use the X11 path when its display
and discovery tools are available. Missing sessions/backends return an error.

Termux does not provide generic capture of another Android app by PID. The MCP
tools report that Android capture requires a supported consent-based platform
integration.

## Installation and supervision

Each C++ bundle contains the MCP service, setup CLI, and TUI. Download scripts
verify all three before replacing installed files. New source clones select the
binary's exact source revision. Existing checkouts keep their revision and must
satisfy the launcher's source/hash preflight.

Linux/macOS:
```sh
curl --fail --location --output install-devbox.sh \
  https://raw.githubusercontent.com/adybag14-cyber/devbox/main/scripts/install-devbox.sh
sh install-devbox.sh
```

Linux Guardian uses a systemd user service when available, with a background
fallback for minimal systems. macOS uses a launchd user agent. Windows uses
scheduled tasks and prefers PowerShell 7, with Windows PowerShell 5.1 as a launch
fallback. Termux supports Termux:Boot and wake locks; see [TERMUX.md](./TERMUX.md).

For Cloudflare Tunnel installation and authentication, use
`devbox-tui --cloudflare-help` and [CLOUDFLARE_TUNNEL.md](./CLOUDFLARE_TUNNEL.md).

## Host-mode boundary

Host mode runs with the service process's permissions. `devbox_exec_readonly`
is cooperative read-only execution. Use Docker mode where container isolation is
required. On Windows, ordinary Devbox commands honor `HOST_SHELL`; explicit
`host_exec` retains the administrative PowerShell policy.

## Validation

The `C++ native runtime` workflow builds, tests, stages, and verifies the native
artifacts before package assembly. Managed lifecycle gates exercise actual
launcher startup, MCP file/process/job operations, Guardian health, and rollback.
Containers have unique fixture names and verified workspace mounts; graphical
tests create their own windows or X11 display. See
[CPP_RUNTIME_REWRITE.md](./CPP_RUNTIME_REWRITE.md) for the frozen compatibility
reference, artifact provenance, and release requirements.
