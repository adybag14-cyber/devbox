# Native computer use

The C++23 MCP contract version 3 exposes 47 tools: the frozen 45-tool compatibility surface plus `host_computer_windows` and `host_computer_use`. Computer input is supported on the interactive Windows host runtime with `ENABLE_HOST_EXEC=true`. Other operating systems explicitly report unsupported input; Docker mode does not direct mouse or keyboard events into a container.

These tools use native Windows window/process APIs, SendInput, and GDI/WIC screenshots. They do not evaluate browser JavaScript or run a shell. Existing capture tools remain available for read-only capture.

## Services in Windows session 0

A startup/S4U task runs in session 0 and cannot use the logged-in desktop directly. Keep the MCP service and Guardian in their existing service session. Run the **same immutable C++ executable** in the user's interactive session as a desktop worker, and set `DEVBOX_COMPUTER_USE_PIPE` in the MCP runtime environment to its local pipe name. Without that setting, a directly launched interactive MCP continues to execute CUA in its own process.

```powershell
./scripts/Start-ChatGptDevboxComputerUse.ps1 -Install -Start `
  -Executable '<project>\run\bin\<verified-cpp-candidate>.exe' `
  -Sha256 '<verified-sha256>' -PipeName 'DevboxComputerUse-<deployment-id>'
```

Register using the existing elevated Devbox operator account. The task uses an Interactive token, starts at that user's logon, and restarts on failure. Its hidden launcher verifies the executable hash on every launch and records the exact worker PID, creation time, session, source and binary identity under `run/computer-use/`. The MCP does not need desktop credentials. Guardian and the tunnel retain their existing lifecycle. Updating the worker task requires quiescing CUA and gracefully stopping only the verified old worker first; updating a running task to a different candidate is rejected.

The worker's named pipe rejects remote clients and grants access only to its Windows user and SYSTEM. Both peers verify the other process's executable path and Windows user, retaining the process handle for each request. The deployment must preserve the immutable executable at that path. Only the two CUA operations are accepted; the worker revalidates their tool schemas. Requests are limited to 64 KiB, replies to 16 MiB, and each exchange to 30 seconds. There is one worker connection at a time. A missing worker fails explicitly; input is never silently retried or redirected to another session.

Cancellation sends a control message and waits for the action to unwind; an MCP disconnect also cancels the native action and releases keys/buttons it pressed. A lost acknowledgement produces an unknown-outcome error and requires a fresh observation. Worker restart invalidates its observations. In broker mode, observations live in the worker and can survive an MCP-only restart; treat all observations as stale after any operational transition. Results include the actual desktop session and worker PID. Use the same executable's `--computer-use-probe PIPE` command from the service session for a read-only inventory/identity check. Logging off removes desktop availability while the MCP's other tools remain available; locked and secure desktops remain inaccessible.

## Observe and act

1. Call `host_computer_windows`, optionally with `title_contains`, and choose a returned `window_id` by its title and PID.
2. Call `host_computer_use` with `action: "observe"` and that `window_id`. Observation brings the selected window to the foreground and returns a JPEG image and structured metadata.
3. Inspect the image. For the next action, use its `observation_id` and coordinates in **that returned image**, not desktop coordinates. Each successful call returns the next image and a new ID.
4. After a failed or uncertain call, observe again before deciding what to retry. Never blindly repeat input after losing its acknowledgement.

```json
{"action":"observe","window_id":"<returned window_id>","max_width":1600}
```

```json
{"action":"click","observation_id":"<latest observation_id>","x":420,"y":260}
```

```json
{"action":"type","observation_id":"<latest observation_id>","text":"Hello"}
```

```json
{"action":"key","observation_id":"<latest observation_id>","keys":["CTRL","L"]}
```

Use a new returned ID for every example; they are not interchangeable. Literal typing does not use the clipboard and does not accept control characters. Send Enter, Tab, arrows, shortcuts, or a bounded key hold through `key`. Omit fields that do not belong to the selected action; unsupported combinations are rejected before input.

| Action | Fields |
|---|---|
| `observe` | `window_id`; optional `max_width` 640–1920 (default 1600), `quality` 30–90 (default 70) |
| `click`, `double_click` | `observation_id`, `x`, `y`; optional `button` left/right/middle |
| `move` | `observation_id`, `x`, `y` |
| `drag` | `observation_id`, `path` of 2–256 `{x,y}` points; optional `button`, `duration_ms` |
| `scroll` | `observation_id`, `x`, `y`, nonzero `scroll_y` or `scroll_x`; positive means down/right; each axis ±20 wheel notches |
| `type` | `observation_id`, `text`, at most 4096 Unicode characters |
| `key` | `observation_id`, `keys` chord; optional `hold_ms` 0–5000 |
| `key_sequence` | `observation_id`, `sequence` of 1–32 `{keys, duration_ms}` segments; 1–5000 ms each, total at most 5000 ms |
| `wait` | `observation_id`; optional `duration_ms` 0–5000 |

`duration_ms` defaults to 300. All actions accept `settle_ms` from 0–1000, default 100, before the final screenshot. Later actions retain the image scale/quality of their observation; call `observe` to change those settings. Supported keys include CTRL/CONTROL, ALT, SHIFT, ENTER/RETURN, TAB, ESC/ESCAPE, SPACE, BACKSPACE, DELETE, INSERT, LEFT/RIGHT/UP/DOWN, HOME/END, PAGEUP/PAGEDOWN, PLUS/MINUS, letters, digits, and F1–F12. Windows/system-global keys and Ctrl+Alt+Delete are unavailable.

The guard also rejects Ctrl+Esc (including Ctrl+Shift+Esc), Alt+Tab, and Alt+Esc before key-down, including variants with additional modifiers. Use the explicit window discovery/observation flow to select a different window.

`key_sequence` changes key combinations within one native call, without a model/network round trip between segments. Keys shared by adjacent chords remain pressed; keys removed from the next chord are released before new keys are pressed. An empty `keys` array releases all owned keys for a bounded pause. Every chord and the total duration are checked before any input. The final screenshot is returned after the sequence, and all keys are released on completion, cancellation or a window/focus/title change. No key remains held between calls.

```json
{"action":"key_sequence","observation_id":"<latest ID>","sequence":[{"keys":["UP"],"duration_ms":500},{"keys":["UP","RIGHT"],"duration_ms":300},{"keys":["UP"],"duration_ms":500},{"keys":[],"duration_ms":100}]}
```

This is a bounded, preplanned gesture. Use it where intermediate input is predictable; it does not inspect changing application content between segments. Window identity, bounds, foreground and desktop checks continue during execution. Re-observe after the returned image or an uncertain outcome. This additive action retains contract version 3 and 47 tools, but changes the schema hash; refresh the client before using it. Capabilities report the segment and duration limits.

## Bounds, ownership, and outcomes

- Window IDs bind the handle to its PID and process creation time. Input validates the current window identity, physical bounds, title, and foreground ownership against the observation. Pointer operations additionally check the window owning the target point.
- DPI-aware physical capture bounds are mapped to the returned image dimensions. Moved/resized windows invalidate the old observation. Unrelated windows covering the target prevent capture, and an occluded pointer target prevents input.
- Observation IDs expire after 180 seconds, are kept in a bounded 32-entry store, and are consumed before input. Restarting the process that owns computer use (the MCP or its desktop worker) invalidates all observations. A rejected parameter validation does not perform an input action.
- One worker executes computer-use operations, with a queue bound of eight; a per-session Windows mutex also prevents simultaneous input from another Devbox instance. Passive/network requests continue on their normal executors.
- Input runs only on the accessible default interactive desktop. Locked/secure desktops and different sessions are rejected. Release physical mouse buttons/modifier keys before starting a computer-use operation.
- Drag, hold, and wait durations are bounded to five seconds. Input loops check cancellation and release keys/buttons they pressed when unwinding. A process crash, desktop transition, or lost acknowledgement can still leave an uncertain outcome; a fresh observation is required, and ambiguous held input may require the user to release it.
- `COMPUTER_INPUT_OUTCOME_UNKNOWN` means some input may already have occurred. The consumed ID cannot repeat it. Inspect the current UI instead of treating this result as proof of no side effect.
- Local root metadata includes activity counts for managed quiescence checks. A deployment or fallback must wait for active computer-use and other tool calls to finish before stopping the server; an in-process key hold has no child process to discover through PID inspection alone.

Window targeting does not make arbitrary application content trustworthy or grant permission for what is displayed. The caller must follow the user's task and authorization, and inspect each new image before choosing another action.

## Authorization and telemetry

`host_computer_windows` requires `mcp:host:read`. `host_computer_use`, including foreground observation, requires `mcp:host:exec`. Existing broad `mcp:tools` authorization remains compatible. Input is annotated as a non-idempotent write with open-world/destructive potential, so clients can preserve their approval behavior.

CUA start/finish/failure events include `usage_type: "computer_use"`. Typed text and key-array values are redacted from usage logs; their lengths remain available. Screenshot bytes are returned as MCP image content rather than included in usage-log argument previews.

Nested `key_sequence` contents are also redacted; telemetry retains only the segment count. For side-by-side candidate staging, the desktop-worker installer accepts a `-TaskName ChatGptDevbox-ComputerUse-<candidate>` name. Each worker still has an immutable executable and distinct pipe, with the per-session input mutex preventing simultaneous injection. Retire only the verified old worker after the new service is healthy; preserve its binary/launcher for rollback.

## Verification and client refresh

`computer-contract` checks schemas, limits, authorization mapping and unsupported-platform behavior. Windows `computer-native` uses a dedicated owned window to exercise screenshots, click/double-click, Unicode, chords, key release on cancellation, wheel direction, drag, stale identities, moved/resized windows, occlusion, and image-to-screen scaling. `computer-broker-native` repeats those actions through a separate, exact-owned worker process and checks protocol rejection, disconnect handling and graceful shutdown. Existing CTest, SDK, OAuth, persistence, and cross-platform gates remain required. Frozen Rust comparisons continue validating all 45 old tools; the two C++ extension schemas are checked independently against `cpp-mcp/contract/computer-tools.json`.

After deploying a committed, verified C++ candidate, refresh the existing Devbox connection in ChatGPT's plugin settings. Confirm both new tool names and the contract version/schema hash, then start a fresh conversation with the plugin enabled. For an integration test, explicitly request these native tools so browser-DOM or shell automation cannot be mistaken for proof of native input. Keep the test in a dedicated window and record the model's actual tool calls and visible result.
