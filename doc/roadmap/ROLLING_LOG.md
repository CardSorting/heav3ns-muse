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

### 2026-10-15 — Mixer fixture and rendered routing verification

- Isolated the initial mixer PCM mismatch to a native fader smoothing period;
  a settled before/after channel move produced identical decoded audio.
- Corrected static gain setup and XML attribute-order assumptions in mixer
  fixtures. MixerStructureTest passed 61 Qt test executions. The broad process
  now passes whole-buffer channel-move, send, reset and removal audio checks,
  plus history/replay and project reload.
- The next broad-process failure is initialization of the built-in amplifier
  effect. The [engineering record](../agent-native-architecture.md#mixer-audio-fixture-stabilization-2026-10-15)
  retains the original observations and remaining limits. Feature development
  stays frozen; no gate status changes.

### 2026-10-15 — Native effect creation and restoration

- Corrected native factory association checks that refused built-in effect
  creation and real-effect restoration during undo/project loading.
- Five focused executables passed, including actual Amplifier add/restore,
  native effect controls/lifetimes, mixer state and synthesis/render/restart.
- The complete configured application/plugin/test rebuild passed again.
- The broad process now passes Amplifier/Stereo Matrix stereo processing,
  bypass, order, history, clear/removal and project reload. Its next assertion
  exposed tempo reverting after native project save/load and later rendering.
  This remains open in the [engineering record](../agent-native-architecture.md#native-effect-factory-and-restoration-repair-2026-10-15).
- Feature development remains frozen; no gate status changes.

### 2026-10-15 — Empty automation persistence and desktop startup

- Preserved empty automation point maps during native target restoration,
  preventing saved projects/checkpoints from inventing tempo/master automation.
- The extended native save/load/edit/render/analyze/restart regression passed
  with tempo 122, four notes and three empty global curves preserved. The
  complete native rebuild and local installation passed again.
- The desktop demo reached its Qt event loop. Corrected local dependency
  placement also allowed bundled startup to discover 55 native plugins without
  library-loader errors. Visual interaction and physical output remain unverified.
- The broad process passes cancellation and sampler/resource workflows and now
  stops at an Opulenz attenuation assertion. Retained automation fixture failures
  remain open in the [engineering record](../agent-native-architecture.md#empty-automation-persistence-and-runnable-desktop-2026-10-15).
- The latest complete suite reported **120 passing and 73 failing executables**
  out of 193; an earlier live-project LFO PCM mismatch reappeared and remains
  unexplained. A separate actual PortAudio output-only playback advanced the
  native clock without reported stream/DSP failure; acoustic receipt is unverified.
- Feature development remains frozen; no gate status changes.

### 2026-10-15 — Working compile/run checkpoint

- Calibrated the OpulenZ level fixture at full MIDI velocity while keeping its
  original 50× attenuation requirement and normal matched preset/restart values.
  No C++ DSP or capability was added or changed in this correction.
- The entire native executable process harness passed in 86.98 seconds,
  including actual audio, assets, effects, tuning, deterministic inference,
  cancellation, history, restart, abrupt-termination recovery and MIDI import.
- The complete configured native build, desktop event loop and real output-only
  PortAudio run establish the requested compile/run checkpoint on macOS arm64/
  Qt 5 Debug. Feature development remains frozen.
- The preceding 120-pass/73-failure full-suite baseline remains recorded. This
  is not a clean full regression suite, real-provider musical acceptance or
  broader roadmap completion; see the [engineering record](../agent-native-architecture.md#opulenz-calibration-and-full-native-process-verification-2026-10-15).
  No gate status changes.


### 2026-10-15 — Existing unit-suite stabilization

- Preserved persisted clip/inline controls during actual native file writing,
  while still removing temporary journals; retained empty automation custom names.
- Corrected configuration ownership and LV2 registration cleanup across engine
  lifetimes. Fixtures now use the existing replay/plan contracts, one-shot
  recoverable faults and acknowledged checkpoint baselines.
- The complete configured native build passed. The next complete suite improved
  from 120/193 to **155/193 passing**, with **38 failures** retained. Both native
  executable process tests passed (94.31 seconds and 5.89 seconds).
- Feature development remains frozen. This is not a clean suite or a gate change;
  see the [engineering record](../agent-native-architecture.md#unit-suite-stabilization-native-files-and-engine-restarts-2026-10-15).


### 2026-10-15 — Native fixture baseline corrections

- Corrected durable setup, native sample offset timing, export-state setup and
  recoverable fault injection in existing tests. Module fixtures use existing
  track creation and instrument loading operations separately.
- The 14 affected executables rebuilt; the focused run passed 9 and failed 5.
  The last complete suite remains 155 passing and 38 failing out of 193.
- Feature development and gate statuses remain frozen; see the
  [engineering record](../agent-native-architecture.md#unit-fixture-baselines-and-native-timing-2026-10-15).


### 2026-10-15 — Native module test boundary and GIG fixtures

- Corrected native test-host class visibility, saved bank fields and split GIG
  wave-pool layout; retained actual audio, program, portability and recovery checks.
- Removed two fixture double frees and corrected snapshot admission, rename
  notification, parameter paging and existing runtime persistence assertions.
- The latest nine-executable focused run passed 5 and failed 4. Import lifecycle,
  MIDI import, Hydrogen import, resource snapshots and Carla now pass; the last
  complete suite remains 155/193 passing. Remaining failures stay explicit in the
  [engineering record](../agent-native-architecture.md#native-unit-hosts-and-playable-bank-fixture-corrections-2026-10-15).
- Feature development remains frozen and no gate status changes.


### 2026-10-15 — Raw OpulenZ MIDI bounds

- Existing note/pressure range checks now inspect raw input before seven-bit
  masking. Six malformed-message cases and native held-note audio passed in the
  isolated emulator test after a successful rebuild.
- A separate full runtime attempt timed out at 300.04 seconds with a captured
  pending-worker/model-lock wait. That failure remains open despite ten passing
  nine-worker batch-test repetitions; see the
  [engineering record](../agent-native-architecture.md#raw-opulenz-midi-bounds-and-observed-worker-wait-2026-10-15).
- No feature or gate advanced. The last complete suite remains 155/193 passing.
