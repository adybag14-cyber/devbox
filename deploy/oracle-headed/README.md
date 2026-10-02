# Oracle headed runtime profile

This is the small-VM profile used for the existing Oracle deployment. The MCP
executable is the qualified native C++23 `linux-musl-x86_64` artifact. Node runs
the existing launcher/Guardian and supervises the desktop; it does not implement
the computer-use actions. Those run in `cpp-mcp/src/computer_x11.cpp`.

Build this image independently of the application release. Verify the C++ artifact
using `cpp-mcp/scripts/verify-promotion.mjs` and run the isolated deployment canary
before promotion. Mount the exact release at `/opt/devbox:ro`, its qualification
evidence at `/assurance:ro`, private persistent state at `/opt/devbox/run`, and a
writable workspace at `/workspace`. The workspace and `.home` must belong to UID
996; parent-directory ownership matters. Mount this directory at `/desktop:ro`.
Keep the existing environment file, OAuth identity, state backend and public URL.

Use the existing admission-drain and Guardian maintenance workflow before replacing
a serving container. Never run old and new containers concurrently against shared
SQLite state. Retain a private offline state backup and the stopped old container.
Do not restore old data over work accepted after promotion.

The profile starts Xvfb at `:99`, 1280x800x24, Openbox, a private session bus, and
headed Chromium. It creates a private Xauthority cookie, exposes no X11 TCP or
remote-debugging listener, and retains Chromium's namespace and seccomp sandbox.
Set `DISPLAY=:99`, `XAUTHORITY=/tmp/devbox-desktop/authority`,
`XDG_RUNTIME_DIR=/tmp/devbox-desktop/runtime` and `DEVBOX_COMPUTER_USE_X11=1` in the
MCP container environment. The launcher script also supplies them to its children.

For the approximately 1 GB Oracle VM, the qualified deployment should use a
512 MiB container memory limit, 1 GiB combined memory/swap limit, 1.25 CPU quota,
256 PID limit, 96 MiB `/dev/shm`, and a 96 MiB private `/tmp` tmpfs. These are
deployment limits, not a claim that arbitrary browser workloads fit. Keep the
root filesystem read-only, run as UID/GID 996, drop all capabilities, set
`no-new-privileges`, and use bounded Docker logs. Publish MCP only on the host's
loopback address for the existing authenticated Cloudflare tunnel. No VNC/CDP
port is required. Browser cold start can be slow on a small VM; verify its actual
visible window and a complete MCP input flow, rather than process existence alone.

`desktop-supervisor.mjs` records private child identities in
`run/desktop/status.json`. `ready` means the desktop children have started; it does
not establish that a web page has loaded. It restarts its owned desktop children
without restarting MCP or cancelling unrelated jobs. Stop/restart operations
check child PID birth times. Guardian continues to supervise the MCP frontend.

## Container seccomp profile

`chromium-seccomp.json` derives from Moby v26.1.3's default profile:

- Source: <https://github.com/moby/moby/blob/v26.1.3/profiles/seccomp/default.json>
- Original SHA256: `9c1025c88ccaa517b648da571961838744ea2137f176bfe6a48b21294cae9c76`
- License: Apache-2.0, reproduced in `LICENSE.moby`.

The extra rules allow creation of child user/PID namespaces, unsharing only
user/PID/network namespaces, and `chroot` inside those child namespaces. The outer
container still has no capabilities; the kernel therefore denies privileged
namespace operations outside its own child user namespace. Other Docker syscall
restrictions remain, including the `clone3` fallback and denial of mount, BPF and
ptrace without capabilities. Do not substitute `seccomp=unconfined`, `--privileged`
or Chromium's `--no-sandbox`. Verify nested `NSpid`, `NoNewPrivs: 1` and additional
seccomp filters on Chromium children during deployment.
