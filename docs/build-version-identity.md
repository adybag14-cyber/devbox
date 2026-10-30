# Version identity in native status

Native status, capabilities and `--build-info` return three compiled version
fields alongside the existing source SHA, tree, binary hash and deployment
generation:

| Field | Meaning |
| --- | --- |
| `nativeVersion` | The CMake native server version, also returned by the existing version function. |
| `packageVersion` | The version from `cpp-bootstrap/VERSION` in the built source. This can identify a development candidate before publication. |
| `releaseTag` | `bootstrap-v<packageVersion>` only when that exact tag resolves to the clean build commit; otherwise `null`. Detached release checkouts are supported. |

These are compiled into the C++23 binary. Status calls do not invoke Git, read a
version file or inspect the installation directory. A tag or source version is
not proof of CI success, signature verification, publication or deployment;
those gates still bind the source and binary hashes independently.

The 0.9.15 candidate adds this reporting after the 30 October ChatGPT Pro check
successfully verified the deployed 0.9.14 hashes but could not discover its
semantic package version. Existing 0.9.14 artifacts remain immutable.

The identity generator includes both new fields in its fingerprint, so adding
a matching tag or changing the version regenerates the header. Dirty builds
never claim a release tag. Native package assembly checks the compiled package
version against the source manifest, and tagged release assembly also checks
the exact compiled release tag.
