# Devbox Setup TUI

`devbox-tui` is a native C++17 interactive frontend for the Rust `devbox-setup` installer.

It provides:

- platform and architecture detection
- Node/npm/Git/Docker/PowerShell/cloudflared preflight
- package-manager visibility
- platform-specific Cloudflare Tunnel help and recovery instructions
- repository and runtime selection
- authentication selection: `none`, `oauth`, or `cloudflare`
- public base URL prompt for OAuth modes
- Cloudflare Access team-domain/audience/JWKS prompts only for `cloudflare`
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
devbox-tui --cloudflare-help --no-color
```

For CI or automation, call `devbox-setup` directly rather than driving the TUI.

## Authentication choices

The TUI maps the user-facing choices onto the server configuration used by the Rust bootstrap:

| TUI choice | `MCP_AUTH_MODE` | Additional values |
|---|---|---|
| `none` | `none` | none |
| `oauth` | `demo-oauth` | `PUBLIC_BASE_URL`; protocol flow only, no external identity check |
| `cloudflare` | `cloudflare-access` | `PUBLIC_BASE_URL`, team domain, audience, optional JWKS URL |

Authentication does not imply a specific tunnel provider. `cloudflared` is optional and appears in preflight only as a transport helper.
