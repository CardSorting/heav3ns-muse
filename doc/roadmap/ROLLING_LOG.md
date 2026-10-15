# Rolling Roadmap Progress Log

<!-- docs-scope: current-guide -->

Auto-generated from [events.jsonl](events.jsonl) and [register.json](register.json). Chronological audit log of completed gates, verified milestones, and operational transitions.

---

## 2026-10-15

### EVT-20261015-ROADMAP-ROLL — 2026-10-15T11:45:00Z
- **Gate**: `GATE-014` • **Status**: `in-progress`
- **Action**: Roadmap rolled to Horizon 2 activation.
- **Result**: Horizon 0 certified 100% complete; Horizon 1 88% verified; Horizon 2 active.

### EVT-20261015-HARNESS — 2026-10-15T08:00:00Z
- **Gate**: `GATE-013` • **Status**: `completed`
- **Action**: Comprehensive process integration test suite executed.
- **Result**: `tests/studio_process_test.py` completed end-to-end testing headless execution, protocol boundaries, signal interruption, and sound rendering.

---

## 2026-10-14

### EVT-20261014-AUDIO-DSP — 2026-10-14T23:55:00Z
- **Gate**: `GATE-012` • **Status**: `completed`
- **Action**: Multi-codec rendering and physical audio analysis verified.
- **Result**: WAV, FLAC, OGG, and MP3 exports verified with `audio.analyze` reporting peak dBFS, RMS power, and clipping count.

### EVT-20261014-MODULATION — 2026-10-14T23:15:00Z
- **Gate**: `GATE-011` • **Status**: `completed`
- **Action**: Automation and modulation controller subsystems verified.
- **Result**: Hermite cubic automation curves and hardware MIDI CC learn binding verified.

### EVT-20261014-MIXER — 2026-10-14T21:40:00Z
- **Gate**: `GATE-010` • **Status**: `completed`
- **Action**: Mixer channel routing and send cycle detection verified.
- **Result**: Mixer channel creation, sends, and insert effect chains verified under engine locks.

### EVT-20261014-SCALA — 2026-10-14T19:30:00Z
- **Gate**: `GATE-009` • **Status**: `completed`
- **Action**: Microtonal tuning and Scala support verified.
- **Result**: Scala `.scl` and `.kbm` file loading and real-time frequency inspection verified.

### EVT-20261014-SYNTHS — 2026-10-14T17:00:00Z
- **Gate**: `GATE-008` • **Status**: `completed`
- **Action**: Synthesizer inspection and sample slicing verified.
- **Result**: Parameter discovery across TripleOscillator, ZynAddSubFX, and SlicerT transient detection verified.

### EVT-20261014-BEATMATRIX — 2026-10-14T13:10:00Z
- **Gate**: `GATE-007` • **Status**: `completed`
- **Action**: BeatMatrix step sequencing verified.
- **Result**: 16-step beat sequencing (`step.set`, `step.resize`, `step.clone`) verified with native undo.

### EVT-20261014-PIANO-ROLL — 2026-10-14T09:30:00Z
- **Gate**: `GATE-006` • **Status**: `completed`
- **Action**: High-speed batch note insertion and microtonal detuning verified.
- **Result**: Batch insertion of up to 4,096 notes, quantization, shifting, and per-note pitch curves verified.

---

## 2026-10-13

### EVT-20261013-ARRANGEMENT — 2026-10-13T22:00:00Z
- **Gate**: `GATE-005` • **Status**: `completed`
- **Action**: Arrangement track and clip mutations verified.
- **Result**: `track.*` and `clip.*` operations verified under C++ engine model guards.

### EVT-20261013-RECEIPTS — 2026-10-13T16:20:00Z
- **Gate**: `GATE-004` • **Status**: `completed`
- **Action**: Request idempotency and receipt caching verified.
- **Result**: Mutating requests fingerprinted with SHA-256; deduplication returns cached result with `replayed: true`.

### EVT-20261013-LOCK-STORAGE — 2026-10-13T10:15:00Z
- **Gate**: `GATE-003` • **Status**: `completed`
- **Action**: Single-writer file lock and atomic manifests verified.
- **Result**: `.lock` file mutual exclusion and atomic manifest two-phase commit verified against process kills.

---

## 2026-10-12

### EVT-20261012-PROTOCOL — 2026-10-12T18:45:00Z
- **Gate**: `GATE-002` • **Status**: `completed`
- **Action**: Line-delimited JSON-Lines wire protocol verified.
- **Result**: 1 MiB frame limits, error formatting, and asynchronous event notifications verified.

### EVT-20261012-HEADLESS — 2026-10-12T14:30:00Z
- **Gate**: `GATE-001` • **Status**: `completed`
- **Action**: In-process studio runtime and headless lifecycle verified.
- **Result**: `QCoreApplication` initialized with dummy drivers and stdio stream isolation.


### 2026-10-15 — Feature freeze and first native runtime evidence

- Feature additions are frozen while the existing application is compiled and
  stabilized; no gate status changes in this entry.
- The complete configured macOS arm64/Qt 5 Debug build passed. The first native
  CTest run reported 100 passing and 92 failing test executables, including
  crashes and a timeout; the broad studio process harness remains failing.
- A real synthesis-render deadlock and short-arrangement/export-name defects
  were repaired. The focused synth/save/render/analyze/restart regression passed,
  with further validation and unresolved regressions documented in the
  [engineering record](../agent-native-architecture.md#feature-freeze-and-native-stabilization-2026-10-15).


### 2026-10-15 — Stabilization regression follow-up

- The complete rebuilt application and test targets passed compilation again.
- The second full native run reported 108 passing and 85 failing test executables
  out of 193. The focused render process timed out once during instrument loading
  in that run; this failure is retained and remains unexplained.
- Three isolated repeats, a concurrent reproduction and 20 further consecutive
  load/render/restart repeats passed. No gate status changes follow from these
  limited results. Broad runtime stabilization remains the active work.

### 2026-10-15 — Checkpoint MIDI stabilization

- Repaired checkpoint publication after recording refusal/track removal and
  active MIDI input preservation through rollback, without adding operations.
- The complete macOS arm64/Qt 5 Debug rebuild and local installation passed.
  InstrumentPresetSaveTest passed 45 Qt test executions, including 14 input-state
  export/checkpoint scenarios. Native synthesis/render/restart also passed.
- The third full CTest run reported **116 passing and 77 failing executables**
  out of 193. Nine previous failures passed; a controller owner-retirement crash
  appeared and reproduced in isolation. It remains unresolved.
- The broader studio process reaches selected-pattern validation after passing
  recording refusal and MIDI recovery. The [engineering record](../agent-native-architecture.md#native-checkpoint-midi-repair-2026-10-15)
  retains limits and next defects. Feature development remains frozen; no gate
  status changes follow from this run.

### 2026-10-15 — Controller retirement and pattern selection repairs

- Reproduced a controller-removal use-after-free in the native debugger and
  stopped the continuation after a callback destroys its Song owner.
- Corrected new-pattern selection by refreshing the selector range before
  selecting the new column, including legacy project recovery behavior.
- The complete native rebuild passed; six focused controller, pattern,
  track-move and synthesis/render/restart executables passed. The previous
  116-pass/77-failure run remains the latest complete suite observation.
- The broad process now reaches a rendered-audio mismatch after mixer channel
  movement. This and retained failures remain open in the
  [engineering record](../agent-native-architecture.md#controller-retirement-and-pattern-selection-stabilization-2026-10-15).
  Features remain frozen; no gate status changes.
