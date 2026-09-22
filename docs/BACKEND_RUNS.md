# Native backend runs

The optional C++23 backend harness is separate from ChatGPT's existing website orchestration. It uses the indexed state coordinator and the `devbox_agent_run` tool. No Hermes UI is included. Provider configuration and grant issuance remain operator actions; a model cannot create its own authority.

The initial model tool is `program`: it proposes a program label and exact arguments, waits for an operator grant, then executes the grant's pinned executable in the run's private workspace using the qualified Windows or Linux isolation backend. This is a deliberately bounded tool surface, not unrestricted host-shell delegation. Task completion means the controller received a final model response; the API labels task success as not independently verified.

## Configure a provider

Select the SQLite backend after the guarded state migration. Put an operator-owned configuration in `MCP_STATE_ROOT/providers.json`. A local example is:

```json
{
  "profiles": [{
    "id": "local",
    "base_url": "http://127.0.0.1:8080/v1",
    "protocol": "chat_completions",
    "model": "YOUR_CONFIGURED_MODEL_ID",
    "local": true,
    "tools": true,
    "vision": false,
    "streaming": true,
    "context_tokens": 32768,
    "output_tokens": 4096,
    "max_run_cost_micro_usd": 0
  }]
}
```

This does not install or start an inference server. Capabilities and context/VRAM limits must match the operator's model deployment. Local endpoints must use literal loopback addresses. Remote endpoints require HTTPS and a private `secret_file`; keys are read through a run/principal/destination-scoped broker, never put into model messages or generic process environments. The file must be private and unaliased. Remote tariffs are configured with `input_micro_usd_per_million` and `output_micro_usd_per_million`. Cost limits enforce these configured conservative tariff ceilings; they are not a statement about an independently verified provider invoice. Paid cost defaults to zero and cannot exceed the profile's operator policy.

Supported protocols are OpenAI Responses and the local Chat Completions-compatible wire format. Streaming function arguments must complete and parse before a call becomes executable. The adapter bounds bytes, context, tokens, time, and configured cost/VRAM. It does not guess model capabilities from a model name and does not retry generation requests automatically. Keyed text deltas are held until the response passes the credential-echo check; unkeyed local text can stream. Responses continuation items are retained without promoting provider output into a system role.

## Run and approve

Call `devbox_agent_run` with `action: "providers"` to inspect the configured profiles, then `action: "create"` with a stable `request_id`, `provider_id`, and `goal`. Optional limits include `max_rounds`, `max_tool_calls`, `max_tokens`, `max_cost_micro_usd`, and `timeout_seconds`. Retrying the same create identity returns the same run; changed inputs conflict.

`get` returns the principal ID, run state, private workspace and any `pending_approval`. `events` returns ordered transition metadata. `artifact` reads an exact, run-owned content hash and labels the content as untrusted evidence. An authenticated client cannot read another principal's run; the server derives the principal from authenticated identity, not from a tool argument.

When the model proposes a program, the operator reviews the actual pending arguments and issues a grant using `--grant-issue PRIVATE_JSON` (see [Scoped execution](SCOPED_EXECUTION.md)). The grant must use the reported principal, run ID, pending operation ID and tool name, the exact argument object, this run's workspace, and the chosen executable's SHA-256. Call `action: "approve"` with that existing grant ID. This only attaches authority that already exists; it cannot enlarge the grant.

The independent native driver resumes the model/tool loop and survives a frontend restart. Each model request is recorded before dispatch. Each tool effect goes through the durable grant-operation receipt. Losing a completed effect's acknowledgement replays its receipt without executing it again. Losing an unconfirmed effect or model reply moves the run to `uncertain`; it does not authorize a blind retry.

`pause` stops further dispatch after the current step; `cancel` requests cancellation. A request acknowledgement is distinct from the eventual terminal state. Remote provider cancellation and billing can remain unknown after local transport cancellation. The run retains that uncertainty and its reserved budget. Operator `reconcile` supports abandoning a run, discarding an unknown model reply while keeping its budget charged, or recording an explicitly operator-supplied effect observation without repeating the effect. Such observations are labeled as not independently verified.

## Qualification status

The fixtures use recorded/mock provider responses and real isolated program effects. Windows and Linux core tests cover fragmented streams, malformed calls, budgets, secret boundaries, exact grants, context ownership, prompt-injection role boundaries, control races, and real process exits around model/effect durability. The MCP integration fixture performs two model requests, waits for an operator grant, executes one native isolated worker, restarts the frontend, and verifies completed-request replay without another generation or effect. These results do not claim deterministic live model generation or benchmark live model task quality.

Full hosted certification, broader task-quality evaluation, negotiated MCP Tasks/resources, and release/canary work remain tracked in `audit-backlog-progress.json`. No paid provider request or production deployment was performed for this development qualification.
