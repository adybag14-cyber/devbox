# Devbox C++23 0.9.12

The native Linux window manager now counts each completed X11 reply as progress.
A map or focus event can require several successful round trips on a throttled
host; previously the desktop watchdog could mistake that work for a stall and
restart Chromium during a browser call. The individual two-second reply timeout
and five-second progress threshold are unchanged. A genuinely blocked reply still
fails within its original bound. The desktop status retains the last failure's
reason and timestamp after recovery, including the concrete window-manager error.
See [native desktop progress and validation](../native-desktop-progress.md).

Linux browser pressure estimates now resolve the process's actual cgroup v1/v2
membership and mount layout, including nested systemd services, visible ancestors,
bind mounts and namespace roots. The estimate uses the tightest available host
or cgroup budget. The native reader keeps bounded input, strict counters and
conservative inactive-file cache credit; it adds no runtime dependency and changes
no kernel limits. See [browser memory pressure](../browser-memory-pressure.md).

Regression coverage includes a private delayed-reply X11 relay, unchanged stalled
server detection, native window lifecycle and descriptor cleanup, nested memory
budgets, malformed counters and path bounds. Local sanitizer checks supplement the
release workflow's required platform, runtime, packaging and security jobs.

The offline installer, portable/user-level Windows packages, separate lean/core
archives and optional verified ripgrep fallback continue from
[0.9.11](bootstrap-v0.9.11.md). Node, Python, a Docker engine, Chromium and a
Cloudflare tunnel are not bundled. Browser and desktop functionality requires
the corresponding supported host components.

Release qualification and production validation are separate. A production
upgrade must verify signed artifacts, preserve active work through maintenance
gates and run a fresh website-agent browser check; publishing a release alone does
not establish that a particular live browsing failure has been resolved.
