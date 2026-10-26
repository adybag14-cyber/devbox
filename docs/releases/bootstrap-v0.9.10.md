# Devbox C++23 0.9.10

This release carries the native dependency reductions, forwarded-metadata
privacy correction and X11 cancellation protection documented in
[0.9.9](bootstrap-v0.9.9.md).

The optional ripgrep qualification now gives its access-time sorting fixtures
explicit, distinct timestamps and verifies those timestamps after closing the
file handles. This replaces creation-time sleeps while preserving the existing
sort and reverse-sort assertions. All upstream integration tests remain required.

The fixture edits are restricted to three pinned upstream test files. Before and
after digests are checked, original bytes and line endings are restored, and
unexpected source changes or file aliases are refused. Qualification also checks
that the runtime executable's digest is unchanged across the test-only edits.
The fixture descriptor and file digests are retained in the optional-tool build
manifest. No runtime code, Cargo dependency, regex feature or installed payload
is changed by the fixture correction.

Independent probes also assign shuffled access times at 100-millisecond and
two-second spacing. They check forward and reverse ordering against the actual
filesystem metadata and verify that the metadata remained stable during the
probe. This retains explicit coverage of subsecond timestamp comparison.

The earlier macOS ARM64 qualification failure is retained as evidence. Diagnostic
runs on a fresh macOS host did not reproduce it, so no specific filesystem or
clock failure is asserted as its cause. The corrected fixtures remove the
unverified assumption that short creation delays establish distinct access times.
