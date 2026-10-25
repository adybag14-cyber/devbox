# Devbox C++23 0.9.7

This release replaces Devbox-owned compression and private Linux desktop helper
processes with bounded C++23 implementations. User-requested Python, Node, shell,
Git and compiler jobs remain optional host capabilities. The native packages do
not include those interpreters or a Docker engine.

- Native LZ77 and fixed/dynamic Huffman encoding replaces libdeflate for responses
  up to 1 MiB. Larger responses retain cancellable streaming zlib. Scratch memory
  is bounded to 192 KiB per call and released afterwards; a 128 KiB worker-stack
  regression protects musl hosts.
- Native Xauthority, authenticated X11 readiness and private window management
  remove Devbox's xauth, xdpyinfo, Openbox and session D-Bus daemon processes.
  Xvfb, Chromium and XCB/XTEST remain desktop capability dependencies. Existing
  desktop managers and Chromium's sandbox are preserved.
- Full desktop archives and offline installers contain a source-pinned ripgrep
  fallback without PCRE2. Host `rg` takes precedence. The fallback is checked
  against its compiled-in digest on every use, and ordinary execution policy,
  cancellation and output limits still apply. Lean core archives omit it.
  No tool is downloaded during fallback resolution.
- The Oracle service profile runs native supervision under systemd, without a
  Docker daemon. It supplies a private read-only browser filesystem, persistent
  desktop runtime mount and verified physical/combined memory limits, including
  cgroup v1 on systemd 245. Production activation is a separate operator step.
- Desktop shutdown now allows its supervisor to clean up owned children before
  forced termination. Linux ephemeral Xvfb/Chromium children are bound to their
  parent's lifetime. Manager and frontend recovery preserve the private browser.
- The frozen Rust compatibility implementation tolerates contention in its
  derived queue-head cache and retries transient Windows atomic-rename sharing
  conflicts without deleting the previously committed state. JavaScript test
  and Rust TLS dependency locks include the security fixes reported by CI.

Matched Windows component measurements show 16–21% less compression time on three
compressible payloads, slightly smaller output, and 9% faster noise rejection.
These are not whole-service speed claims. Methodology and results are in
[NATIVE_DEPENDENCY_REDUCTION.md](../NATIVE_DEPENDENCY_REDUCTION.md).

Qualification covers independent decompression, sanitizers, fuzzing, thread
safety, native desktop input and crash recovery, offline tamper rejection,
dependency inventories and blocking vulnerability checks. A separate Oracle
canary verified live Apple UK HTML/offer extraction, native scrolling and URL
typing, manager/frontend crash recovery, Chromium namespace/seccomp isolation,
actual memory limits and complete shutdown. A product-page price range remains
an aggregate offer; it is not represented as a selected variant's checkout price.

Use full/offline packages for the optional ripgrep fallback, or a `devbox-core-*`
archive for only the server and native management. Windows offline installation
supports current-user and portable directories without administrator rights.
Zvec is not included: its indexed retrieval lifecycle is different from a
file-search executable fallback. Supported native libraries and browser updates
remain part of ongoing security maintenance.
