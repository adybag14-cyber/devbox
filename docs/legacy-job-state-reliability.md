# Legacy detached job-state replacement

PR146's Windows qualification completed the native tests, then failed its
JavaScript rollback MCP fixture: a detached worker was absent while its last
status remained `running`. The original worker stderr was discarded, so the
precise cause of that hosted occurrence cannot be proved from its retained log.

A local regression using a real Windows file handle reproduced that failure
shape. Holding the status file open without delete sharing caused the old
worker's terminal `rename` to throw `EPERM` outside its execution catch block.
The command had completed, but the worker exited without committing success.

The optional JavaScript backend now shares an atomic JSON writer for job status,
requests and heartbeats. It serializes writes per file within the process, uses
an exclusively created temporary file, retries transient Windows replacement
errors for at most ten attempts (555 ms maximum backoff), and always preserves
the previous complete destination on failure. It never truncates that file as
a fallback. Temporary files are cleaned up without deleting the destination.

Fatal detached-worker stderr is retained in a private job file through an
inherited file descriptor, without adding a parent-pipe lifetime dependency.
The MCP regression includes that bounded diagnostic in a failure report.
Failed queued or terminal status persistence emits an explicit bounded
`JOB_STATUS_PERSIST_FAILED` record and exits nonzero. A failed queued write
prevents command execution; a failed terminal write does not rerun the command
or claim that its outcome was durably recorded.

The original real-lock regression failed before the fix and passed afterward.
A persistent-lock case proves bounded failure, preservation of the old record,
temporary cleanup and later recovery. Concurrent readers/writers continue to
observe complete JSON. The full local unit suite passed 209 tests with two
platform skips, and all 19 Windows rollback MCP tests passed.

This repairs the retained optional rollback backend. Production remains native
C++23; Node is not added to the native release assets.
