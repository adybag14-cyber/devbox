# Recovering a stopped native Windows supervisor

The native supervisor repairs its children while it is running. Windows Task Scheduler
must recover the supervisor itself. Production stopped with task result 1 on
2026-11-15 and stayed stopped despite `RestartOnFailure`. Its operational history
was disabled, and the direct native task action did not retain stderr, so the
original exception could not be established. Earlier frontend telemetry recorded
state-coordinator and file-lock timeouts; these are precursors, not a proven cause.

`scripts/Enable-DevboxNativeTaskRecovery.ps1` adds an indefinite one-minute time
trigger to an existing task. It requires `IgnoreNew`, no hard termination, no
runtime limit, and an exact expected executable, digest and argument string.
It preserves existing boot/logon triggers, principal, action and settings. Reapplying
it is a no-op. The native `manage run` command honors `config.json`'s `desired=false`,
so a deliberate maintenance stop does not restart the frontend.

First run without `-Apply` to inspect the proposed trigger and saved task definition.
Use an elevated operator context for `-Apply`; `-EnableTaskHistory` additionally
enables the standard Task Scheduler Operational event log. No watchdog process,
new production task, binary rebuild or change to the native configuration is needed.

```powershell
$root = 'C:\Users\adyba\docker-chatgpt-devbox'
$config = Get-Content -Raw -LiteralPath "$root\run\native\config.json" | ConvertFrom-Json
# Use the argument spelling and quoting from the independently verified deployment.
# This example uses the native generator's spelling for this space-free root.
$expectedArguments = 'manage run --root C:\Users\adyba\docker-chatgpt-devbox'
./scripts/Enable-DevboxNativeTaskRecovery.ps1 `
    -TaskName DevboxNative-Production `
    -ExpectedExecutable $config.current.file `
    -ExpectedSha256 $config.current.sha256 `
    -ExpectedArguments $expectedArguments `
    -EvidenceDirectory C:\Users\adyba\devbox-task-recovery-evidence `
    -Apply -EnableTaskHistory
```

Verify that the existing supervisor PID, creation time and deployment generation
are unchanged, the task has a next run time, and both local `/readyz` and an
authenticated connector call succeed. A skipped repetition while the task is
running is expected. A time trigger cannot repair a still-running hung supervisor.
An abrupt exit should be retried at the next one-minute trigger; host suspension
and Task Scheduler delays can extend that interval.

The elevated `scripts/ci/test-windows-native-task-recovery.ps1 -EvidenceDirectory PATH`
test creates only its own temporary task, intentionally exits it with code 1,
observes automatic recovery, and crosses another interval to prove that `IgnoreNew`
preserves the second process without a duplicate. It checks identity refusal and
idempotence, then releases its child cooperatively and unregisters the fixture.

For rollback, remove only the trigger with ID `DevboxNativeRecovery`, using
`Set-ScheduledTask -Trigger` with the other saved triggers. Preserve running work.
The installer saves before/after task XML and a JSON receipt for auditing.

Microsoft documents [repetition patterns](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-repetition-triggerbasetype-element)
and [IgnoreNew](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-multipleinstancespolicy-settingstype-element).
