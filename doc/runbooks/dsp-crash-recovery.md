# Runbook: DSP Crash Recovery & Plugin Isolation

<!-- docs-scope: current-guide -->

Diagnose native playback, rendering and worker failures using the studio's
line-delimited JSON protocol. Use a disposable reproduction session and preserve
its acknowledged state before repair. A request interface cannot recover a
processor or native thread that no longer returns.

## Purpose and trigger

Use this runbook for a native crash, stalled worker, failed render, nonfinite PCM
or backend processing failure. Audio DSP Maintainers own the investigation.

## Authority and prerequisites

Follow the native build instructions in the [README](../../README.md). The
commands below assume a Debug executable at `build/native/lmms`, LLDB on macOS
or an equivalent debugger on the target platform, and a disposable session.
Protocol contracts come from the [Native Studio Protocol](../native-studio-protocol.md);
actual verification and limits belong in the [engineering record](../agent-native-architecture.md).

## Procedure

1. Preserve the reproduction project, last acknowledged session state, protocol
   requests and responses, stderr, platform and build revision. Keep private
   paths and credentials out of public reports.
2. Start a disposable headless reproduction under the debugger:
   ```sh
   lldb --batch -k 'thread backtrace all' -o run -- ./build/native/lmms studio ./build/triage-session --config ./build/triage-config.xml
   ```
   Studio mode uses `QCoreApplication` and dummy audio/MIDI by default. Send
   requests as JSON lines on stdin; responses use stdout and diagnostics use
   stderr. There is no studio socket startup option.
3. If the process still responds, inspect native DSP and backend state:
   ```json
   {"id":"dsp-state","op":"audio.processing.inspect","args":{}}
   {"id":"transport-state","op":"transport.inspect","args":{}}
   ```
   Worker counts are a snapshot, not a hang detector. For a persistent wait,
   capture all thread stacks and correlate the coordinator, queued/scanning
   workers and model owner. Preserve the original failed run before retrying.
4. Isolate the observed instrument, effect, route or resource in a copy of the
   project. Rediscover native indices after a restore or structural change.
   Do not remove tracks from the sole copy of the user's project.
5. Add or strengthen the regression at the owning native boundary. Keep actual
   PCM, ownership, rollback and receipt assertions relevant to the failure.

## Stop conditions

Stop the identified output path if playback is unsafe. If the process is
unresponsive, preserve its stacks and let its supervisor stop that specific
process. In-process C++ catches do not contain deadlocks, signals or corrupt
memory. Do not suppress assertions or drop the worker completion barrier to
make a test finish.

## Recovery and rollback

Restart only after the previous process has exited and its session writer lock
has been released. Reopen the preserved session through the normal studio
startup and verify the restored project. An incomplete or corrupt serializer
still requires diagnosis; a manifest alone does not prove complete recovery.
Repair the project before requesting `audio.processing.reset`, which has native
stopped/disarmed/export/recording admission checks. Backend restoration uses
its separate `audio.device.restore` contract.

## Verification and evidence

Run the relevant native tests, then the complete required suite and process
harness for the change. For worker and mixer changes:

```sh
ctest --test-dir build/native --output-on-failure -R '^(AudioEngineWorkerTest|MixerProcessingTest|StudioRuntimeTest)$'
python3 tests/studio_process_test.py --lmms build/native/lmms --output build/studio-evidence
```

Record failures, repetitions, observation date, platform and limitations.
Sanitizer validation requires an instrumented build; setting `ASAN_OPTIONS`
alone does not instrument a binary. Compare finite/nonzero PCM and the fixture's
actual audio requirements. Exact checksums apply only where determinism was
established for that input and configuration.
