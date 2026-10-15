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
