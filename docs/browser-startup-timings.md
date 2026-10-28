# Browser session setup timings

The native C++23 browser controller retains the existing startup totals in
milliseconds and adds `timing.startup.session_setup_phases_us`. This small object
contains fixed phase names and nonnegative integer microsecond durations from
the native steady clock. It carries no page content, URLs, credentials, protocol
arguments or arbitrary labels.

The phases cover browser-version retrieval, download policy, target attachment,
page enablement, frame-tree retrieval, resource interception, network enablement,
cache/service-worker policy and viewport setup. An owned browser reports
`target_discovery`; a managed shared browser reports `context_create` and
`target_create` for its fresh isolated context and window.

These are contiguous native setup intervals, including protocol waits and local
processing. They are not browser JavaScript performance entries or measurements
of model thinking. The detail uses microseconds so fast local operations do not
all round to zero; the enclosing `session_setup_ms` and other existing totals keep
their original units and meaning. The fields are additive, and command ordering,
ownership, network policy, sandbox settings, cancellation and deadlines are
unchanged.

The browser qualification checks the fixed field set, nonnegative values and that
the detailed sum fits within the enclosing interval and its millisecond rounding.
An 80 ms delay injected through the existing native-only diagnostic callback is
checked in the version phase during the real Chromium fixture. The callback is
not exposed through MCP or an environment setting. Shared and owned browser
flows both validate their appropriate phase shapes.

This instrumentation identifies which setup operations warrant a controlled
optimization. It does not itself establish a browser-startup speedup.
