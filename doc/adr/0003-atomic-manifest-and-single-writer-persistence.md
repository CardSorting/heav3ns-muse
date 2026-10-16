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

## Empty automation restoration clarification — 2026-10-15

**Supersedes:** the implicit reuse of interactive first-target initialization
when reconnecting serialized automation target IDs. Manifest, lock and receipt
decisions remain unchanged.

Loading a project or recovery checkpoint preserves its saved automation point
map, including emptiness. Reconnecting a saved target does not author an initial
point. Interactive attachment still initializes a previously unbound empty curve
using its target value. This distinction prevents an empty saved global curve
from turning into active automation that overrides a later tempo or master
control edit during playback/rendering. The native XML format is unchanged.

## Persisted native control metadata clarification — 2026-10-15

**Supersedes:** treating every `metadata` element as disposable during native
file serialization. The manifest, lock and receipt decisions remain unchanged.

Clip and inline-note controls retain the legacy `metadata=1` skip marker so older
MIDI-clip readers do not mistake them for notes. Those persistent elements also
write `native_state=1`, which exempts them from temporary metadata removal while
still removing nested journal elements. Their values, controller connections and
linked-control references must survive actual project/template/preset file
writing, not only in-memory DOM copies. Files previously written without those
controls cannot reconstruct the lost values from their remaining graph alone.

Automation clips persist the explicit custom name, including emptiness, rather
than materializing a derived target/attachment label into a custom name.

## Session memory and filename clarification — 2026-10-16

**Supersedes:** the `.lock` and `manifest.json` names in decision entries 1–2
above and any implication that session preferences live in a separate memory
directory. The single-writer, atomic-publication and request-replay decisions
remain unchanged.

StudioRuntime uses `QLockFile` at `session.lock` and publishes the session
manifest as `session.json` through `QSaveFile`. Explicit preference memory is
the manifest's `memory` object. `session.memory.set` replaces that object;
the caller must retain any existing keys it wants to preserve. A successful
response follows commit, and startup reads the committed object independently
of the native project history cursor. Undo/redo restores project checkpoints
without reverting current memory. An acknowledged request replay returns its
old response without reapplying that older memory over a newer replacement.

These are existing implementation contracts. The dedicated native process
harness verifies clean/abrupt restarts, replacement, clearing and session
isolation on the observed platform; see the [engineering record](../agent-native-architecture.md)
for dated results. That observation does not establish power-loss durability,
arbitrary file-tamper recovery or inferred musical taste.
