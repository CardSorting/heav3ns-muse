# Runbook: DSP Crash Recovery & Plugin Isolation

<!-- docs-scope: current-guide -->

Operational procedure for isolating, diagnosing, and mitigating instrument or effect plugin crashes in the headless HEAV3NS-MUSE runtime.

---

## Purpose and trigger

- Trigger: Headless studio crash (`SIGSEGV`, `SIGFPE`), runaway memory leak, or broken socket connection during playback or export.
- Purpose: Restore studio host stability, isolate offending synth or effect code, and preserve user project state.
- Primary Owner: Audio DSP Maintainers.

## Authority and prerequisites

- Local build tree with debug symbols: `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DHEADLESS=ON`.
- LLDB (macOS) or GDB (Linux) installed.
- Access to disposable reproduction project manifests.

## Procedure

1. **Capture Backtrace**: Reproduce the crash under LLDB/GDB batch mode (`lldb -- ./build/lmms --studio --socket /tmp/triage.sock`).
2. **Identify Faulting Subsystem**: Pinpoint whether the fault originated in an instrument oscillator, an effect filter, or the `Mixer` buffer loop.
3. **Isolate Plugin**: Remove or bypass the offending plugin channel using `client.remove_track(track_index=N)`.
4. **Audit Numerical Invariants**: Inspect C++ loop for divide-by-zero, uninitialized buffers, or buffer indexing overruns.
5. **Add Regression Test**: Create an automated test in `tests/studio_process_test.py` reproducing the exact parameter configuration.

## Stop conditions

- Halt live audio routing immediately if high-gain runaway feedback or infinite floating-point loops occur.
- Never deploy a crash fix that suppresses asserts without addressing underlying memory bounds.

## Recovery and rollback

- Revert the offending plugin commit if an immediate hotfix is unavailable.
- Restore the last known clean project checkpoint from `.mmpz` backup.

## Verification and evidence

- Build with AddressSanitizer and verify zero memory errors: `ASAN_OPTIONS=detect_leaks=1 python3 tests/studio_process_test.py`.
- Verify clean offline audio export with bit-identical output.
