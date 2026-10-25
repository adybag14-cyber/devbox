# Devbox C++23 0.9.9

This release includes the native dependency reductions and performance changes
from [0.9.7](bootstrap-v0.9.7.md) and the forwarded-metadata privacy correction
from [0.9.8](bootstrap-v0.9.8.md).

The private Linux window manager now blocks SIGPIPE on its own XCB worker
thread. Closing the pinned X11 socket during cancellation can race a write in
XCB; that write must report a connection failure rather than terminate the
desktop supervisor. The application's process-wide signal disposition and
other threads are unchanged. A pending worker signal is not unblocked during
teardown.

A regression test delivers SIGPIPE to the exact owned worker, then verifies
normal window lifecycle operations, duplicate-manager refusal, bounded shutdown,
stalled-server cancellation and descriptor cleanup. The test also checks that
the process-wide SIGPIPE policy remains unchanged.
