# Devbox Chrome Bridge

An independent local Chrome bridge for agents, with a Rust native host and MCP
server. It does not rely on OpenAI's desktop authorization host, a model API key,
Node, Python, Docker, a remote debugging port, or a public listening socket.
The Chrome service worker and popup use JavaScript because Chrome exposes its
extension APIs there. Browser work uses Chrome's documented debugger API.

## Access and routing

The extension has `activeTab`, `debugger`, and `nativeMessaging` permissions and
no blanket host permissions, content scripts, cookie APIs, or externally
connectable page-message interface. Chrome's debugger permission is powerful:
the implementation additionally requires an explicit, one-hour grant for each
tab, rejects access after cross-origin navigation, and revokes grants on that navigation or debugger
detach. New same-origin tabs require a separate popup checkbox. A popup button
revokes all grants. Untrusted page content cannot grant access.

The native host communicates through a local Windows named pipe with an explicit
current-user/SYSTEM ACL and remote clients rejected. It validates bounded JSON
frames, uses operation deadlines, and does not log page contents or tokens.
Protocol version 2 rejects mismatched old hosts, propagates client disconnect
cancellation, and acknowledges complete replies before closing the pipe.
Only this extension's stable ID is allowed by its native-host manifest.

Use the configured Devbox `devbox_chrome_bridge` action when available. A local
agent can use this package's `chrome_bridge` MCP tool independently when Devbox
is unavailable. If the extension is unavailable before an action is sent, the
independent `native_chrome` MCP tool provides screenshots and input in an
explicitly selected Google Chrome window. This fallback does not require a
Devbox process. Routing is explicit: an unknown input outcome must be reconciled
by observation, never automatically replayed through another backend.
Native input must not be used to bypass denied tab grants or browser policy.
The independent MCP fallback is local: a cloud-only website agent still needs
an available remote MCP connection to reach this computer.

## Build and install on Windows

From an extracted portable ZIP, run the bundled installer directly. Rust and
other build tools are not needed:

```powershell
.\devbox-chrome-bridge.exe --install
```

To build from the repository instead, run these commands in `chrome-bridge`:

```powershell
cargo test --locked --jobs 3
cargo build --release --locked --jobs 3
./target/release/devbox-chrome-bridge.exe --install
```

The installer registers only the current user's native-host key and copies the
executable to a version-and-hash-specific path under
`%LOCALAPPDATA%/DevboxChromeBridge`. Existing versions remain available. No admin
rights, browser restart, browser policy change, or machine-wide service is
required. Registration alone does not load the Chrome extension.
The x64 Windows build links the C runtime statically. CI inspects the produced
PE imports to reject a dependency on a separately installed VC++ redistributable.
The host manifest is replaced atomically after its contents have been flushed.

In Google Chrome's Extensions page, use the normal developer **Load unpacked**
flow and select this package's `extension` directory. Review the permissions.
Open its popup on a public test page, connect the Rust host, and grant that tab.
Do not load it into unrelated profiles or silently grant existing signed-in tabs.

Configure a local MCP client to launch the built/installed executable with
`--mcp`. For a single structured browser request, `--request` reads JSON from
stdin and returns one JSON result; it does not interpret shell code.

For Codex CLI, register the installed immutable executable with:

```powershell
codex mcp add devbox_chrome -- C:/path/to/installed/devbox-chrome-bridge.exe --mcp
```

New client sessions discover `chrome_bridge` and `native_chrome`. Existing
sessions may need to restart their MCP client. The original OpenAI Browser
authorization host is separate and is not replaced or impersonated.

The optional C++23 Devbox action requires Windows host mode, host execution,
and operator configuration of `DEVBOX_CHROME_BRIDGE_EXE` plus the exact
`DEVBOX_CHROME_BRIDGE_SHA256`. The adapter locks/verifies the image and sends
bounded JSON through stdin without starting a shell. It is disabled by default.

## Functionality and limits

The extension supports granted-tab discovery, same-origin tab creation,
navigation/back/forward/reload, focus/close, bounded DOM text and element
references, screenshots, retained HTML byte ranges, click/double-click/move/
drag, scroll with before/after movement evidence, typing, keys, input-text
selection and select options. Every mutation needs a fresh observation and a
stable unique request ID. Retried identical IDs recover receipts; conflicts
fail. Invalidated observations prevent later replays after receipt eviction.

HTML is a bounded serialization of the current DOM, not the original network
response. It is retained up to 2 MiB per observed tab, with an aggregate 8 MiB
payload budget, at most eight observations and sixteen tab grants. Replay
records are capped at 64 entries and 2 MiB. These are retained-payload budgets,
not total browser heap measurements. Node references are weak references.
`read_html` returns at most 64,000 exact retained bytes with their hash;
truncation is explicit. Main-frame
DOM inspection includes iframe metadata, but direct cross-origin iframe DOM
interaction is not implemented. Native screenshot input can handle visible
content where DOM interaction is unavailable. File upload, browser download
management, arbitrary JavaScript evaluation and cookie APIs are not exposed.
The extension navigation API rejects privileged URLs. Its native fallback is
a desktop UI interface and remains subject to the user's action permissions.
Chrome coordinates use CSS viewport pixels; prefer returned element IDs.
Use `type` for text including punctuation. `key` accepts named navigation keys
and ASCII letter/digit shortcuts; it rejects punctuation key names instead of
guessing a Windows virtual-key code. `BRIDGE_BUSY` means another request is
still running: wait for it, without switching to native fallback input.
Control names include associated HTML labels and `aria-labelledby`. A label
covering its own control is an allowed click target; unrelated overlays still
reject input. The compact text view normalizes whitespace; retained HTML keeps
the underlying DOM serialization separately.

The native fallback currently supports Windows Google Chrome only. It pins the
window handle, process birth, title, geometry and foreground; inputs expire
after 30 seconds and consume their observation. Screenshots are visible desktop
crops, capped at 1600 by 1200 pixels, and can include occlusion. `header_only`
captures the upper 160 pixels of the window to reduce incidental page capture.
Windows accessibility APIs can activate the selected Chrome window, with
identity/foreground checks retained. Secure desktops and active physical input
are rejected; partial input cleanup releases only accepted key/button downs.
Scroll units
are wheel notches for native input and pixels for the extension. Input delivery
does not prove the intended page effect; inspect the returned screenshot.

There is no claim of complete parity or universal site access. Chrome policies,
site authentication, user permissions, interstitials, CAPTCHAs and other access
controls remain in force. This is a new implementation; consult retained test
evidence for which capabilities have actually been exercised on each build.

The Windows CI workflow builds a portable ZIP with the executable, extension,
dependency lock and SHA-256 inventory. It does not silently install an extension
or mark an untested package as live-browser certified. Current support is one
Chrome profile per Windows user; other browsers and operating systems are not
qualified by this package.
