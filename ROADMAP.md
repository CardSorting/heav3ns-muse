# 🗺️ HEAV3NS-MUSE Auto-Rolling Roadmap

<!-- docs-scope: current-guide -->

*An evidence-backed, auto-rolling milestone roadmap for the agent-native digital audio workstation initiative.*

Active horizon: **H2** • Last synchronized: **2026-10-16**

Machine authority: [register.json](doc/roadmap/register.json) • [Open actions](doc/roadmap/OPEN_ITEMS.md) • [Engineering evidence](doc/agent-native-architecture.md) • [Event log](doc/roadmap/events.jsonl)

## Current rolling horizon

```text
[████████████████████] 100%  H0: Foundation & Headless Wire Protocol (completed)
[████████████████████] 100%  H1: DAW Capability Coverage & Compositional Agency (completed)
[████░░░░░░░░░░░░░░░░]  20%  H2: Production Hardening, Multimodal Audio & Hardware (in-progress)
[░░░░░░░░░░░░░░░░░░░░]   0%  H3: Autonomous Studio Mesh & Timbral Synthesis (deferred)
```

## Horizon breakdown

### H0: Foundation & Headless Wire Protocol

**Status: completed (100% complete)**

Establish in-process C++ StudioRuntime, headless QCoreApplication initialization, single-writer file locking, line-delimited JSON IPC, and request idempotency.

### H1: DAW Capability Coverage & Compositional Agency

**Status: completed (100% complete)**

Expose over 60 native LMMS C++ operations covering piano roll, beat sequencing, synth inspection, Scala microtuning, mixer sends, and offline audio rendering.

### H2: Production Hardening, Multimodal Audio & Hardware

**Status: in-progress (20% complete)**

Real-time multimodal audio critique loop, hardware audio/MIDI hot-swapping, VST3/CLAP hosting via Carla, and automated mastering compliance.

### H3: Autonomous Studio Mesh & Timbral Synthesis

**Status: deferred (0% complete)**

Multi-agent stem jam sessions, zero-latency WebSocket studio relays, and neural timbre diffusion models.

## Milestone gate register

| Gate | Horizon | Title | Status | Verification command | Implementation / retained receipt |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `GATE-001` | H0 | In-Process StudioRuntime & QCoreApplication Headless Mode | `completed` | python3 tests/studio_process_test.py | [src/core/StudioRuntime.cpp](src/core/StudioRuntime.cpp) |
| `GATE-002` | H0 | Line-Delimited JSON-Lines Wire Protocol | `completed` | python3 tests/studio_process_test.py | [src/core/StudioStdio.cpp](src/core/StudioStdio.cpp) |
| `GATE-003` | H0 | Atomic Manifests and Single-Writer File Locking | `completed` | python3 tests/studio_process_test.py | [src/core/StudioStorage.cpp](src/core/StudioStorage.cpp) |
| `GATE-004` | H0 | Request Idempotency & SHA-256 Receipt Caching | `completed` | python3 tests/studio_process_test.py | [src/core/StudioRuntime.cpp](src/core/StudioRuntime.cpp) |
| `GATE-005` | H1 | Arrangement & Track Organization Capabilities | `completed` | python3 tests/studio_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-006` | H1 | Piano Roll Batch Editing & Microtonal Detuning | `completed` | python3 tests/studio_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-007` | H1 | BeatMatrix Step Sequencer Operations | `completed` | python3 tests/studio_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-008` | H1 | Bundled Instrument Loading & Slicer Integration | `completed` | python3 tests/studio_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-009` | H1 | Scala Microtonal Tuning (.scl) & Keyboard Mapping (.kbm) | `completed` | python3 tests/studio_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-010` | H1 | Mixer Routing, Sends & Cycle Detection | `completed` | python3 tests/studio_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-011` | H1 | Automation & Controller Rack Modulation | `completed` | python3 tests/studio_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-012` | H1 | Offline Multi-Codec Audio Rendering & Technical DSP Analysis | `completed` | python3 tests/studio_process_test.py | [src/core/StudioAudio.cpp](src/core/StudioAudio.cpp) |
| `GATE-013` | H1 | End-to-End Process Integration Test Harness | `completed` | python3 tests/studio_process_test.py | [tests/studio_process_test.py](tests/studio_process_test.py) |
| `GATE-014` | H2 | Live Multimodal Audio Critique & Revision Loop | `in-progress` | python3 tests/studio_process_test.py | [src/core/StudioInference.cpp](src/core/StudioInference.cpp) |
| `GATE-015` | H2 | Hardware Audio/MIDI Backend Hot-Swapping & Fault Recovery | `in-progress` | python3 tests/studio_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-016` | H2 | Multi-Track Stem Export & Automated Loudness Compliance | `open` | python3 tests/studio_process_test.py | [src/core/StudioAudio.cpp](src/core/StudioAudio.cpp) |
| `GATE-017` | H2 | Third-Party VST3 & CLAP Hosting via Carla Bridge | `open` | python3 tests/studio_vst_process_test.py | [src/core/StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| `GATE-018` | H2 | Cross-Session Musical Preference & Palette Memory | `completed` | python3 tests/studio_memory_process_test.py --lmms build/native/lmms --output build/studio-memory-evidence | [doc/evidence/test-runs/20261016-gate018-memory-restart.json](doc/evidence/test-runs/20261016-gate018-memory-restart.json) |
| `GATE-019` | H3 | Multi-Agent Jam Session & Stems Collaboration Mesh | `deferred` | N/A | Future planning |
| `GATE-020` | H3 | Real-Time Neural Timbre Diffusion Synthesis | `deferred` | N/A | Future planning |
| `GATE-021` | H3 | Zero-Latency WebSocket Remote Studio Relay | `deferred` | N/A | Future planning |

## Auto-rolling governance

1. New completions require a registered, hash-checked execution receipt and retained output, plus a maintainer's criterion review. Source files alone do not close gates.
2. Pre-2026-10-16 completion claims remain dated history. Their original events and dates are preserved; synchronization does not reverify them.
3. Synchronization derives horizon status, percentages and the first incomplete horizon from gate state. Horizon changes append an event; repeated synchronization without changes writes nothing.
4. `python3 scripts/roadmap-sync.py check` validates register consistency, generated views and referenced completion receipts. It does not execute verification commands or establish continuous runtime coverage.
5. Use `python3 scripts/roadmap-sync.py sync` after register edits. [Completion procedure](doc/how-to/change-with-documentation.md) and [append-only rolling log](doc/roadmap/ROLLING_LOG.md).
