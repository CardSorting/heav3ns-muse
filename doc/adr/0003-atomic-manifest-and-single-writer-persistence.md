# ADR 0003: Atomic Manifests, File Locks, and Request Idempotency

<!-- docs-scope: decision-record -->

- **Status**: accepted
- **Date**: 2026-10-13
- **Domain**: domain-session-durability
- **Owner**: Session Storage Maintainers

---

## Context

AI agent sessions can crash, exceed memory or time budgets, or experience abrupt process kills (`kill -9`). Furthermore, network retries from remote LLM orchestrators can resubmit mutating requests, causing duplicate notes, double-created mixer channels, or corrupted project states.

## Decision

We implement durable session persistence using three core primitives in `StudioStorage.cpp` and `StudioRuntime.cpp`:
1. **Single-Writer File Lock (`.lock`)**: A POSIX `flock` or Win32 file lock guarding the session directory from concurrent access.
2. **Two-Phase Atomic Commit (`manifest.json`)**: Checkpoints (`checkpoints/<uuid>.mmp`) are written to temporary files and verified before updating an atomic manifest that points to the latest valid revision.
3. **Idempotency Receipts (`receipts/<uuid>.json`)**: Every mutating request is fingerprinted via SHA-256. Re-submitting an identical request ID returns the persisted response marked with `"replayed": true`.

## Consequences

- **Positive**: Complete crash resilience. Incomplete renders or crashed writes leave the last committed manifest intact.
- **Positive**: Protection against duplicate calls during network blips.
- **Trade-off**: Requires local disk I/O for receipt verification and checkpoint serialization on mutations.

## Recovery MIDI state clarification — 2026-10-15

**Supersedes:** the implicit reuse of portable project/track automatic-MIDI
exclusion during recovery checkpoint serialization. The manifest, lock and
receipt decisions above remain unchanged.

Recovery checkpoints preserve current instrument-port configuration, including
active automatic MIDI input. Ordinary exports can omit automatic assignment for
portability; they suspend only a currently active assignment and restore it after
serialization. A stale assignment flag must not enable a disabled input or erase
manual input settings while saving. Checkpoint capture does not use that export
suspension because restoring its output must retain the live input state.

This corrects observed rollback state loss without changing the native XML
format or studio capability signatures. Device delivery and broad crash recovery
still require their own runtime evidence; the general crash-resilience rationale
above is not evidence that every serializer or backend is failure-safe.
