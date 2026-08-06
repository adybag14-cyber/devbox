# Devbox Setup TUI

`devbox-tui` is a native C++17 interactive frontend for the Rust `devbox-setup` installer.

It provides:

- platform and architecture detection
- Node/npm/Git/Docker/PowerShell preflight
- package-manager visibility
- repository and runtime selection
- bind address, port, and workspace prompts
- dependency/link/start choices
- optional Guardian installation
- final setup review before execution

The TUI does not duplicate installation logic. It invokes the Rust bootstrap with structured arguments, which keeps scripted and interactive installs consistent.

## Build

```bash
cmake -S setup-tui -B setup-tui/build -DCMAKE_BUILD_TYPE=Release
cmake --build setup-tui/build --config Release
```

Keep `devbox-tui` and `devbox-setup` in the same directory for release bundles. You can also specify the backend explicitly:

```bash
devbox-tui --bootstrap /path/to/devbox-setup
```

Useful noninteractive checks:

```bash
devbox-tui --version
devbox-tui --diagnostics --no-color
```

For CI or automation, call `devbox-setup` directly rather than driving the TUI.
