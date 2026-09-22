# Scoped backend execution (implementation in progress)

The existing MCP tools retain the explicitly named `trusted_operator` profile. They use the configured host account, and their read-only hints are not an OS sandbox. The new backend grant path defaults to a restricted worker and never falls back to ordinary host execution if isolation fails. It is intended for the separate backend run controller; it does not change the ChatGPT website's existing tool orchestration or add Hermes UI.

Operator-only native commands, with the SQLite state backend selected:

```text
devbox-mcp --grant-create-workspace RUN_ID
devbox-mcp --grant-issue PRIVATE_DEFINITION_JSON
devbox-mcp --grant-inspect GRANT_ID
devbox-mcp --grant-revoke GRANT_ID
devbox-mcp --execute-granted PRIVATE_REQUEST_JSON
```

Grant creation and revocation are deliberately absent from the model's MCP tool registry. A definition contains `principal`, `run`, `operation`, `tool`, exact `arguments`, `workspace`, `executable`, `executable_sha256`, `egress_origins`, and `expires_at_ms`. Program grants require an executable digest, a private workspace, and an empty egress list. Their arguments contain `args` and optional bounded `input`, `timeout_ms`, `memory_bytes`, `cpu_ms`, and `output_chars`. Execution requests identify the grant and the exact principal/run/operation plus the approved arguments. Grants expire within 24 hours.

Admission checks the immutable context and canonical arguments, validates the executable identity, appends a durable security event, and records the operation before dispatch. Expired or revoked grants reject new admission. Revocation is a boundary for future admissions; cancel an already admitted run separately. A completed receipt can be read again after revocation, but its effect is never dispatched again. An uncertain operation requires reconciliation and cannot be blindly retried with the same identity. These rules do not make external side effects transactional.

Windows workers use a fresh LPAC identity, a private executable copy, directory and executable handles that prevent path/image replacement, an explicit credential-free environment, a single-process Job Object, and memory/CPU/wall-time/output limits. Only the run workspace receives write access. The broker supplies `registryRead` for capability-ACL-governed runtime registry access; no network, COM, camera, or credential capability is granted. Win32k calls and child-process creation are disabled. Consequently, a program must support Win32k lockdown; a binary that initializes desktop libraries can fail explicitly. This is a qualified restricted profile, not a universal promise of compatibility with every Windows CLI program.

The worker uses detached console semantics while remaining owned by its kill-on-close Job Object. This avoids console initialization failing inside LPAC. Critical-error dialogs are disabled for the headless service. Profile/ACL cleanup is checked after execution, and untrusted workspace contents are retained rather than recursively erased by the broker.

The Windows fixture verifies the real AppContainer token, the effective LPAC denial of an `ALL APPLICATION PACKAGES`-only file, denial of a private outside file, absence of synthetic ambient secrets, writable workspace, disabled Win32k and child creation, and OS denial of network sockets. The direct `TokenIsLessPrivilegedAppContainer` query returns `ERROR_INVALID_PARAMETER` on the local Windows build, so the fixture also tests the actual distinguishing access semantics. Controlled replacement attempts after hashing are rejected by the held file/directory handles. The combined grant/worker fixture verifies durable receipt replay without repeating a completed write.

Linux isolation is not implemented/qualified at this checkpoint and explicitly refuses autonomous execution. Other unqualified platforms also refuse it. Linux namespace isolation, broader adversarial coverage, provider/run-controller integration, and hosted qualification remain H04/H12–H14 work. None of this checkpoint has been promoted to production.

Implementation references: [Microsoft's AppContainer and LPAC startup guidance](https://learn.microsoft.com/en-us/windows/win32/secauthz/implementing-an-appcontainer), [Chromium's Windows sandbox design](https://chromium.googlesource.com/chromium/src/+/main/docs/design/sandbox.md), and [Chromium's broker process-creation implementation](https://chromium.googlesource.com/chromium/src.git/+/refs/heads/main/sandbox/win/src/broker_services.cc).
