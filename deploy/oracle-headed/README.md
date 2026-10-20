# Oracle headed runtime profile

This is the small-VM profile used for the existing Oracle deployment. The MCP
executable is the qualified native C++23 `linux-musl-x86_64` artifact. The native
`--desktop-supervisor` mode supervises the desktop. Node remains in the existing
launcher/Guardian migration path; it is no longer required by desktop
supervision or computer-use actions. Input runs in `cpp-mcp/src/computer_x11.cpp`.
The image uses pinned Alpine 3.24 with its newer Chromium; the core release's
musl ABI baseline remains separately qualified.
ImageMagick, xdotool, xwininfo and xset are no longer installed in this local-X11
profile: capture, window enumeration and input use the native XCB/XTEST path.
Small xauth/xdpyinfo utilities remain for desktop authentication and startup
verification. Wayland and remote-display compatibility helpers are outside this
profile.

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
public remote-debugging listener, and retains Chromium's namespace and seccomp sandbox.
Set `DISPLAY=:99`, `XAUTHORITY=/tmp/devbox-desktop/authority`,
`XDG_RUNTIME_DIR=/tmp/devbox-desktop/runtime` and `DEVBOX_COMPUTER_USE_X11=1` in the
MCP container environment. The launcher script also supplies them to its children.

For the qualified native browser controller, `DEVBOX_BROWSER_SHARED=1` opts into
one warmed, managed Chromium with a private loopback CDP endpoint and a temporary
profile. Each `devbox_browser open` receives an isolated incognito context and its
own C++ public-only egress proxy. This mode leaves the old saved browser profile
untouched. The default blank context cannot browse; native X11 tools operate on
the window opened by `devbox_browser`. Status reports warming until Chromium's
endpoint is ready (up to 180 seconds). Session teardown disposes its context;
the desktop supervisor retains and restarts only its own managed process.
See [native browser limits and validation](../../docs/NATIVE_BROWSER.md).

For the approximately 1 GB Oracle VM, the qualified deployment should use a
512 MiB container memory limit, 1 GiB combined memory/swap limit, 1.25 CPU quota,
256 PID limit, 96 MiB `/dev/shm`, and a 96 MiB private `/tmp` tmpfs. These are
deployment limits, not a claim that arbitrary browser workloads fit. Keep the
root filesystem read-only, run as UID/GID 996, drop all capabilities, set
`no-new-privileges`, and use bounded Docker logs. Publish MCP only on the host's
loopback address for the existing authenticated Cloudflare tunnel. No VNC/CDP
port is required. Browser cold start can be slow on a small VM; verify its actual
visible window and a complete MCP input flow, rather than process existence alone.

The native supervisor records private child identities in
`run/desktop/status.json`. `ready` means the desktop children have started; it does
include a successful private Chromium version probe but does not establish that
a web page has loaded. It restarts its owned desktop children
without restarting MCP or cancelling unrelated jobs. Stop/restart operations
use the native process owner's handles/cancellation and retain PID birth times
and executable identities. Child environments exclude application credentials;
logs rotate at 1 MiB per file and retain one previous file. Guardian continues to
supervise the MCP frontend.

Stage the exact qualified binary before starting this profile. The default
desktop executable is `/opt/devbox/bin/native/devbox-mcp`;
`DEVBOX_DESKTOP_BINARY` can select its verified immutable runtime path.
`DEVBOX_DESKTOP_SUPERVISOR=node` is an explicit legacy rollback option.
The public tool schema is unchanged. Native fixtures can place the private
profile elsewhere using `DEVBOX_BROWSER_SHARED_PROFILE` in the server's operator
environment; this is never accepted as a model tool argument.

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
