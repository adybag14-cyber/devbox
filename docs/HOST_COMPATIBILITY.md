# Host Compatibility

For public deployments, `devbox-tui --cloudflare-help` prints platform-specific Cloudflare Tunnel installation/setup guidance; the complete guide is [CLOUDFLARE_TUNNEL.md](./CLOUDFLARE_TUNNEL.md).

Devbox host mode is supported on Windows, Linux, macOS, and Termux/Android. Docker remains optional where available.

## Display and window screenshots

The preferred cross-platform screenshot tools are:

- `host_capture_display`: capture the host desktop/virtual display
- `host_capture_window`: capture the largest visible application window owned by a PID or one of its child processes
- `host_capture_program`: compatibility alias for `host_capture_window`
- `windows_host_capture_display` and `windows_host_capture_program`: legacy aliases retained for existing clients

Windows uses a layered strategy rather than trusting `PrintWindow` alone. The capture code uses DWM extended frame bounds, skips cloaked/minimized helper windows, searches child processes for multi-process applications, tries `PrintWindow` with `PW_RENDERFULLCONTENT` and default flags, and samples both the complete frame and the client-area interior. If Windows reports a successful capture but the renderer area is effectively black (a common DirectX/DirectComposition, Android-emulator, WebView, or hardware-video failure), Devbox falls back to the pixels visible through the desktop compositor. That fallback can include occluding windows; the returned metadata explicitly reports `screen_fallback_may_include_occluders`.

macOS uses CoreGraphics to discover active display bounds and PID-owned windows, then uses the native `screencapture` compositor path. Screen Recording permission is required. Grant it under **System Settings -> Privacy & Security -> Screen Recording** to the terminal/Node host process and restart that process. The CoreGraphics discovery helper is typechecked on both Intel and Apple Silicon CI runners.

Linux supports both X11 and Wayland:

- X11 window discovery: `wmctrl`, `xdotool` + `xwininfo`, or `xprop` + `xwininfo`; capture uses `maim` or ImageMagick `import`.
- X11 full desktop: `maim`, `scrot`, `gnome-screenshot`, KDE `spectacle`, or ImageMagick `import`.
- wlroots/Sway/Hyprland Wayland windows: compositor PID/window metadata plus `grim` region capture.
- Wayland full desktop: `grim`, `gnome-screenshot`, or `spectacle`.

Pure Wayland deliberately prevents arbitrary cross-application window enumeration on some compositors. When GNOME/KDE does not expose a non-interactive PID-selected window path, Devbox returns an explicit portal/security limitation instead of silently capturing the wrong window. XWayland applications can still use the X11 window path when `DISPLAY` is available.

Termux/Android is intentionally different: ordinary terminal apps cannot capture another Android app by PID without Android's MediaProjection/user-consent flow, so the generic tools return an actionable unsupported error there.
## Native setup binaries

Every bootstrap release contains the Rust `devbox-setup` CLI and the C++17 `devbox-tui` frontend. Linux ships x86-64 and ARM64 builds; macOS ships Intel and Apple Silicon builds; Windows ships x86-64; Termux ships four Android API 21+ ABIs.

Linux/macOS users can install both verified binaries with:

```bash
curl --fail --location --output install-devbox.sh \
  https://raw.githubusercontent.com/adybag14-cyber/devbox/main/scripts/install-devbox.sh
sh install-devbox.sh
```

Interactive shells start the TUI. Pass CLI arguments to run the Rust installer non-interactively.

## Linux

- host runtime defaults to `/bin/sh` unless `HOST_SHELL` is set
- prerequisite installer supports apt, dnf/yum, pacman, zypper, and apk
- Guardian installs as a systemd user service when systemd is available
- a non-persistent background fallback is available on minimal systems

```bash
./scripts/install-guardian.sh auto
systemctl --user status devbox-guardian.service
```

## macOS

- host runtime uses `$SHELL` or `/bin/sh`
- missing prerequisites can be installed through Homebrew
- Intel and Apple Silicon setup/TUI binaries are built in CI
- Guardian installs as `~/Library/LaunchAgents/com.adybag14.devbox.guardian.plist`

```bash
./scripts/install-guardian.sh launchd
launchctl print "gui/$(id -u)/com.adybag14.devbox.guardian"
```

## Windows

Windows supports both host and Docker runtime modes. The host command layer prefers PowerShell 7 and automatically falls back to Windows PowerShell 5.1 only if the primary shell cannot launch. Guardian uses elevated scheduled tasks so normal `host_exec` calls do not require a per-command UAC prompt.

## Termux / Android

Termux is host-only and does not require Docker. `scripts/install-termux.sh` selects the Android ABI, verifies both native binaries, and can install Guardian through Termux:Boot.

See [TERMUX.md](./TERMUX.md).

## Host-mode security boundary

Host mode is not a hard sandbox. `devbox_exec_readonly` is cooperative read-only execution and host tools run with the permissions of the Devbox process. Use Docker mode when container isolation is required.

## Validation

```bash
npm ci
npm test
cargo test --manifest-path bootstrap/Cargo.toml
cmake -S setup-tui -B setup-tui/build -DCMAKE_BUILD_TYPE=Release
cmake --build setup-tui/build --config Release
DEVBOX_RUNTIME_MODE=host node bin/devbox.js start
curl --fail http://127.0.0.1:8100/healthz
node bin/devbox.js stop
```


## Runtime E2E CI

The `Platform runtime E2E` workflow validates the real host-mode MCP runtime, not only compilation. It runs the launcher, negotiates MCP over Streamable HTTP, verifies the tool registry, exercises read-only and mutating shell execution, text and exact-byte file round-trips, recursive listing/search, `host_exec`, `host_run_program`, and a Guardian health probe.

Current native/container coverage:

- macOS 15 Apple Silicon (`macos-15`)
- macOS 15 Intel (`macos-15-intel`)
- Ubuntu 26.04 LTS
- Debian 13
- Fedora 44
- Alpine 3.23
- Arch Linux rolling
- official `termux/termux-docker:x86_64`

The Linux distribution jobs run inside their upstream container images on a GitHub Ubuntu runner. The macOS jobs run directly on GitHub-hosted macOS machines.
