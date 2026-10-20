# Native offline installation

The offline package contains the native C++23 server, installer and setup TUI,
their source manifest, dependency inventory, SBOM and third-party notices.
Installation does not download software or require Node, npm, Git, a compiler,
GitHub CLI, an external archive extractor or administrator privileges.

Each executable targets a specific operating system and CPU architecture.
Download the matching release asset and verify its release checksum and GitHub
attestation before transferring it to an offline computer.

Windows uses an `asInvoker` executable. Its default directory is
`%LOCALAPPDATA%\Programs\Devbox`. It creates no machine-wide registry entries,
services, scheduled tasks, firewall rules or PATH changes. Linux defaults to
`$XDG_DATA_HOME/devbox`, or `~/.local/share/devbox`. macOS defaults to
`~/Library/Application Support/Devbox`.

```
devbox-offline --user
devbox-offline --portable "/path/you/own/devbox" --no-start
devbox-offline --inspect
```

The destination must be empty or absent on a local filesystem qualified for
Devbox's private durable state. On Windows that means a fixed local disk with
private ACL support; the installer itself can be carried on removable media.
Installation verifies the compressed
payload against an identity embedded in the installer, verifies every extracted
file, checks the native runtime's source and capability identity, then publishes
the installation. Extraction uses bounded buffers. An invalid payload is never
executed. This verification trusts the installer executable already obtained
from the release; it does not authenticate an arbitrary installer by itself.

Offline installations record the `bundled-qualified` policy and their hosted
workflow/source identity. Existing online Sigstore verification continues to
use `signed-qualified`. Neither policy permits an unqualified development
binary to be promoted over the installed runtime.

The initial server listens on `127.0.0.1` and uses port 8100 unless `--port` is
supplied. `--no-start` leaves it configured and stopped. Use the installed native
CLI, with the absolute installation directory:

```
bin/devbox-mcp manage status --root "/path/to/devbox"
bin/devbox-mcp manage start --root "/path/to/devbox"
bin/devbox-mcp manage stop --root "/path/to/devbox"
```

On Windows the CLI filename ends in `.exe`. The runtime and private state stay
inside the installation directory. Moving an initialized directory requires
reinitialization because native management deliberately binds process and
binary ownership to canonical paths.

Offline installation does not make remote websites or a ChatGPT connection
available without a network. The core package does not contain Chromium,
desktop capture services or a Cloudflare tunnel. Browser-inclusive variants,
where offered, identify those payloads and operating-system requirements
separately. No live production configuration or credentials are bundled.
