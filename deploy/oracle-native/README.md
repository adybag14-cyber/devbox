# Native Oracle Fallback service

This profile runs the qualified C++23 executable directly under systemd. Its
browser libraries live in a read-only Debian filesystem selected by
`RootDirectory=`. The host OS provides process supervision, cgroups and mount
namespaces. No Docker daemon, containerd, docker-init, Node launcher or Python
runtime is needed for the service.

The filesystem supplies the packages in `browser-packages.txt`, installed from
authenticated Debian main, updates and security repositories without recommended
packages. Record the resolved package versions, archive-keyring identity and
filesystem archive digest. Do not include Node/npm, Python or container engines.
Debian's browser dependencies can include D-Bus and X11 utility files. Devbox
does not launch those helpers: its private desktop uses native authentication,
readiness, window management and XCB/XTEST input, plus Xvfb and Chromium. Keep
the browser's package dependencies consistent and retain its own sandbox.

The service template is an operator artifact requiring host qualification before
activation. Substitute only reviewed absolute paths for these placeholders:

| Placeholder | Host content |
|---|---|
| `@RUNTIME_ROOT@` | Immutable browser/OS filesystem with UID/GID 996 named `devbox` |
| `@INSTALL_ROOT@` | Verified installation files, mounted at `/opt/devbox` |
| `@ASSETS_ROOT@` | Verified C++ executable and qualification evidence, mounted at `/assurance` |
| `@STATE_ROOT@` | Existing private writable state, mounted at `/opt/devbox/run` |
| `@WORKSPACE_ROOT@` | Existing workspace, mounted at `/workspace` |
| `@DESKTOP_ROOT@` | Dedicated private 96 MiB tmpfs mounted at `/tmp/devbox-desktop` |
| `@RUNTIME_MOUNT_UNIT@` | The matching `desktop-runtime.mount.in` unit filename, obtained with `systemd-escape --path --suffix=mount DESKTOP_ROOT` |

Install the mount definition before the service. Keep it mounted across service
restarts: an adopted browser and a replacement frontend must see the same
authority and DevTools endpoint files even when systemd creates a new private
`/tmp` namespace. It has no long-lived helper process. Stop/unmount it only after
the installation is stopped and its owned desktop children are confirmed gone.
Use a separate mount and display number for a canary.

Keep these in-service paths unchanged when migrating persisted jobs/artifacts.
The native configuration must bind MCP to **127.0.0.1**, using the host port
already selected by the authenticated tunnel. A container's `0.0.0.0` listener
must not be carried into the host network namespace. Authentication, public URL,
OAuth state and tunnel identity remain independently configured in private state.
No credentials belong in the template or a release artifact.

The existing native desktop companion should use the verified executable's
`--desktop-supervisor` mode and the same `/workspace`/private authority paths.
A canary needs its own state, workspace, loopback port and display number; Linux
abstract X11 sockets share the network namespace even with private `/tmp`.
Never start two frontends against the production database.

## Resource limits on older systemd

The profile requests 576 MiB physical RAM, 1 GiB combined RAM/swap, 256 tasks and
1.25 CPUs. systemd 245 enforces physical memory on cgroup v1 but does not map
`MemorySwapMax=` to its combined-limit file. The C++ operator command

```
devbox-mcp --service-memory-guard devbox-native-fallback.service 603979776 1073741824
```

verifies the actual physical limit and applies/reads back the combined limit.
It requires root startup credentials, the exact `devbox-*.service` cgroup in
`/proc/self/cgroup`, real kernel cgroup files and root-owned directories. It
cannot choose an arbitrary path or increase an observed limit. Writes require
the helper to be alone in a fresh service cgroup. Recovery can re-verify an
already enforced bound while existing owned children remain alive.

The `!` prefix elevates only this short pre-start command's user credentials;
the filesystem namespace, empty capability set and other restrictions remain.
The long-lived supervisor and all its workers run as UID/GID 996. Verify the
hierarchy's read-only mount and the narrow exception for this root-owned service
cgroup; UID 996 must not be able to write those controls. This explicit mount
policy supports systemd 245, whose `ProtectControlGroups` remount prevents a
nested write exception needed by the pre-start guard. Verify the
actual kernel limits and permissions on the target host; a parsed unit file alone
does not establish enforcement. The guard is not exposed as an MCP tool.

## Qualification and activation

Before activation, qualify a separate local canary using the exact executable,
filesystem and service restrictions. Verify private desktop startup, actual
Chromium rendering/HTML/screenshots, native click/scroll/type, stale-observation
checks, browser crash recovery, service restart and owned-child cleanup. Confirm
Chromium's namespace/seccomp sandbox and `NoNewPrivs`, real cgroup limits and a
loopback-only listener. This filesystem namespace is not a virtual machine;
arbitrary user jobs remain subject to its service policy.

For cutover, verify source/signatures and state compatibility, close admission
and wait for active work to drain, retain private state/configuration backups,
then stop only the verified previous deployment. Start the native service with
the existing state/workspace mounts and validate the public authenticated MCP
route and headed computer use before reopening admission. Keep the stopped old
deployment and its settings for rollback. Never restore an old database over
work accepted after cutover. Building or publishing this profile does not perform
any activation, global OS upgrade, tunnel change or mailbox operation.
