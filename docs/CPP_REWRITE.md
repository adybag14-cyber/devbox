# Complete C++ replacement

Baseline: `cd8803c81ad3a14ec3b3fa2afa0d08256975b1e9`, contract version 2, 45 tools. The Rust runtime and bootstrap installer are the reference implementations during this port. Production remains pinned to that existing Rust deployment until the C++ replacement is complete and validated.

## Completion contract

The replacement must execute its HTTP/MCP transport, authorization, host/Docker commands, detached runners, scheduling, cancellation, files/search, task/operation receipts, capture, monitoring and installation logic in C++. It must not call the Rust executable or expose unimplemented tool handlers as supported features. Guardian and external host programs remain integrations.

Both the MCP runtime and Rust bootstrap installer are in scope. Existing job requests, statuses, receipts, task checkpoints and OAuth state must remain readable. The 37 legacy tools and eight native durable-agent tools retain their input/output contracts. C++20 is the portable language baseline, with pinned native dependencies and native platform process/filesystem backends.

## Module map and acceptance

| Area | Rust authority | C++ destination | Required evidence |
| --- | --- | --- | --- |
| Configuration, outputs, identity | config, result, output, provenance | shared core | Environment/schema/result fixtures, clean-build identity |
| HTTP/MCP and scopes | server, gateway, request_control, schema_parity | transport and dispatch | SDK, protocol revisions, cancellation, all 45 schemas |
| OAuth/JWKS | oauth | auth | PKCE, registration, token rotation/revocation, bounded JWKS stalls |
| Host/process control | runtime, process, windows_* | process and runtime | Native Windows/POSIX execution, pipe bounds, ownership, deadlines |
| Scheduling/jobs | execution, jobs, job_manager, job_runner, job_logs | scheduler and jobs | Cross-process queue compatibility, dedup, admission, restart, cancellation, retention |
| Files and task state | files, atomic_file, task_store, docker_files | storage | Byte exactness, conditional writes, ACLs, alias policy, durable retries |
| Search, inspection, lifecycle | search, host_inspect, lifecycle, github_auth | tools | Functional parity in host and Docker modes |
| Capture | capture, windows_capture, posix_capture | capture | Real images, process-window selection, timeout/cold-start checks |
| Monitoring | background, performance, usage, incident_task, quota_task | monitoring | Bounded queues, rotation, health, metrics and failure visibility |
| Installer and deployment | bootstrap, managed launchers | cpp-bootstrap and launcher integration | Fresh install, immutable C++ candidates, Guardian and rollback |

Production changes are a separate final phase. Until every row is complete and cross-platform gates pass, no production checkout update, configuration edit, service restart, Rust binary replacement or tunnel change is permitted for this rewrite.
