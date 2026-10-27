# Browser memory-pressure estimates

Linux browser admission and early session cleanup use the smaller of host
`MemAvailable` and the process's applicable cgroup memory headroom. A service
can belong to a nested systemd cgroup even when `/sys/fs/cgroup` exposes the
whole hierarchy. Reading only the mount root misses that service's hard limit.

Native C++23 discovery now reads `/proc/self/cgroup` and `/proc/self/mountinfo`,
resolves v1 memory-controller and v2 unified membership against their mounts,
and samples the member group plus visible ancestors. Bind-mounted subtrees and
namespace roots are handled without walking outside the exposed mount. Unrelated
subtrees are excluded. Legacy parents that explicitly disable hierarchical
accounting are not applied to their children.

This follows the kernel's hierarchical resource model and the mount-info root
and mount-point mapping. See the [kernel cgroup v2 documentation](https://docs.kernel.org/admin-guide/cgroup-v2.html)
and [Linux mountinfo specification](https://man7.org/linux/man-pages/man5/proc_pid_mountinfo.5.html).

Each finite budget uses its own usage and reclaimable inactive-file counters.
Legacy groups without hierarchical accounting use their local cache counter.
The existing conservative cache-credit calculation is retained; inconsistent or
duplicate cache counters receive no credit. Swap is not counted as available
physical memory. Integer parsing rejects negative, overflowing and trailing
values, and exhausted budgets cannot wrap around to a large positive estimate.

Discovery is bounded to 16 KiB of membership, 1 MiB of mount information, 256
group paths, 4 KiB paths and 64 KiB per statistics file. Paths containing NUL,
line breaks or traversal components are rejected. Kernel counters are sampled
fresh; there is no stale topology cache. If proc discovery is unavailable, the
existing standard namespace-root fallback remains. Missing or unreadable
counters provide no estimate, so this remains a best-effort pressure signal.
The kernel continues to enforce limits independently; Devbox does not change
limits, cgroup membership, mounts or permissions.

Tests cover a systemd service below an unlimited root, v2 parent pressure from
sibling processes, namespace roots, bind mounts, escaped mount points, mixed
controller names, invalid counters, non-hierarchical v1 parents, bounded input
and traversal rejection. The module is native C++23 and adds no dependency.
