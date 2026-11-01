# Android build output binding

Cross-compiled Android binaries cannot be executed on the ordinary packaging
host to obtain their build identity. Previously the artifact recorder read the
current generated identity header but independently hashed files from the
requested package directory. A stale package directory could therefore be
labelled with a newer header's identity outside the normal fresh CI staging flow.

Starting with the 0.9.20 candidate, `build-android.mjs` writes an
`android-build-binding.json` record only after its checked CMake build succeeds.
That record binds the ABI, generated identity-header digest, parsed source/version
identity, and hashes and lengths of all three completed build outputs.

The artifact recorder reads this record from the selected build directory,
including a custom `--build` directory. It requires the current header, source,
tree, package version and build outputs to match, then verifies each staged
runtime, installer and TUI image against the completed output. Release packaging
checks the embedded binding again against the bytes it receives. A changed
header, stale same-size image, truncated image, wrong ABI or missing completion
record fails before a new manifest is accepted.

This is an explicit binding to a trusted completed build invocation, not an
independent execution of every Android ABI and not an additional signature.
Existing hosted qualification, package digests and GitHub attestations remain
required. The x86_64 Termux runtime test remains a separate execution gate. Local
dirty diagnostic builds can still complete, but cannot be recorded as clean
release artifacts. No interpreter or dependency is added to production packages.

Focused tests passed on Windows and Linux, covering exact matches, all three
staged components, same-size tampering, truncation, missing records, changed
headers/build outputs, wrong ABI/source/tree/version and dirty release inputs.
The existing five release-assurance tests also passed. Native CI runs the new
guard tests, and the four real Android builds exercise record creation and
staging through the normal workflow.
