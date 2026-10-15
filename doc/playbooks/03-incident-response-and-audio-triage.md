# Playbook 03: Incident Response & Audio Triage

<!-- docs-scope: current-guide -->

Triage unsafe output, native crashes, unresponsive studio processes and failed
exports while preserving the last acknowledged project/session state. The
studio interface is JSON lines over stdin/stdout; it has no studio socket.

## 1. Severity Classification Matrix

| Severity | Definition | Immediate action |
| :--- | :--- | :--- |
| SEV-1 | Unsafe output, driver freeze or native host crash during a live session | Stop the identified output path and preserve available state/stacks |
| SEV-2 | Unresponsive studio process, missing MIDI delivery or failed export | Capture protocol/backend state and thread stacks before supervised restart |
| SEV-3 | Recoverable artifact, refused operation or documentation mismatch | Preserve a reproduction and track the verified issue |

These are triage priorities, not guaranteed resolution times.

## 2. Emergency Audio Kill & Containment

Identify the exact affected process and output before stopping it. For a
responsive session, use the native transport/backend operations and inspect
their acknowledged results. A hung processor can prevent those requests from
returning; its supervisor must then stop that specific process.

Do not use a broad process-name kill or remove invented socket files. Preserve
unrelated desktop sessions and user work. Confirm process exit before reopening
the same single-writer session directory.

## 3. Post-Incident Forensic Recovery

1. Preserve the project, acknowledged session state, exact requests/responses,
   stderr, thread stacks, build revision and platform. Keep private paths and
   credentials out of public records.
2. Reproduce with a disposable Debug session using the actual native startup:
   ```sh
   lldb --batch -k 'thread backtrace all' -o run -- ./build/native/lmms studio ./build/triage-session --config ./build/triage-config.xml
   ```
3. Follow the [DSP recovery runbook](../runbooks/dsp-crash-recovery.md) to
   distinguish backend failure, DSP failure, worker waits and native ownership.
   A momentary worker snapshot alone does not establish a deadlock.
4. Verify repair with owning native tests and the process harness. Record both
   successful retries and the original failures as dated evidence in the
   [engineering record](../agent-native-architecture.md) and
   [rolling log](../roadmap/ROLLING_LOG.md).
5. Change an existing roadmap gate only when its physical evidence changes,
   following the [roadmap's authoritative register](../roadmap/register.json).
   Documentation synchronization does not certify runtime or gate completion.
