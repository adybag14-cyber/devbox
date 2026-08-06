# Host Compatibility

Devbox host mode is supported on Windows, Linux, macOS, and Termux/Android. Docker remains optional where available.

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
