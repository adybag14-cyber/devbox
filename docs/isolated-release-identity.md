# Release identity across isolated source transfers

The 0.9.15 tagged workflow completed its platform builds but packaging rejected
the Alpine binary because its compiled `releaseTag` was null. The outer checkout
had the correct tag. The isolated build received `git bundle create ... HEAD`,
which retained the commit and tree but omitted the tag ref used by the native
identity generator. The packaging gate correctly prevented publication.

The certification source bundle now carries HEAD and only the package-version
tag that resolves to that exact clean commit. Tagged workflows require that ref
before starting an expensive build. The isolated clone verifies the source and
tag, and the built Alpine artifact must report the same identity. Other distro
checks still validate reused native artifacts; a tagged release requires the
exact release tag on every target. Non-release reruns can reuse an artifact
compiled before the tag existed without relabelling it.

Fast tests use real Git bundles, real clones and the production CMake identity
generator. They reproduce the old missing-tag result, then prove both lightweight
and annotated tags survive the corrected transfer. Missing, stale, mismatched
and dirty release inputs are rejected. The exact source/tag packaging assertion
remains in force. Previously created tags and published assets are not changed.

This 0.9.23 candidate includes the 0.9.21 performance/recovery integration and the
0.9.22 unparsed-currency-text correction. Their benchmark and validation evidence
remains linked from the corresponding documents. Full qualification of this
combined source is required before release or production promotion.
