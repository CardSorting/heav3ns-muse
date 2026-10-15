# Maintainer Glossary & Lexicon

<!-- docs-scope: domain-reference -->

Authoritative technical glossary and lexicon for HEAV3NS-MUSE maintainers, developers, and autonomous agents. Defines architectural concepts, audio DSP terms, and governance nomenclature.

---

## 1. Engine & Audio DSP

| Term | Technical Definition |
| :--- | :--- |
| **Headless Studio Host** | An in-process LMMS runtime initialized via `QCoreApplication` without a graphical windowing system (no X11, Wayland, or Cocoa display server requirement), exposing socket-based JSON-RPC control. |
| **Track Model Authority** | The foundational invariant establishing that native C++ engine classes (`Song`, `TrackContainer`, `InstrumentTrack`, `Mixer`) hold the single, authoritative project state; agents never maintain external shadow models. |
| **Tick Determinism** | Audio sequencing where note positions, durations, and automation points are resolved against discrete integer ticks (192 ticks per quarter note / 48 ticks per 16th step), ensuring sample-accurate reproducibility. |
| **AudioEngine Buffering** | The real-time DSP cycle where the `Mixer` requests fixed-size audio buffers (typically 64, 128, 256, or 512 frames) from instrument plugins and applies channel effects chains before outputting to hardware or null sinks. |
| **Lock-Free Ringbuffer** | A single-producer single-consumer circular memory buffer used to exchange audio frames and MIDI events between the main event dispatch thread and the real-time audio thread without mutex contention. |
| **Dynamic Audio Backend** | The pluggable hardware driver abstraction (`AudioAlsa`, `AudioCoreAudio`, `AudioPulseAudio`, `AudioJack`, `AudioSdl`) switchable at runtime without terminating the running studio session. |
| **LUFS Integrated Loudness** | Standardized loudness measurement (ITU-R BS.1770-4) computed over the entire duration of a rendered track or stem to verify acoustic compliance (-14 LUFS for streaming targets). |
| **True-Peak Limiting** | Inter-sample peak detection utilizing 4x oversampling interpolation to guarantee rendered audio does not clip DAC reconstruction filters (-1.0 dBFS ceiling). |

---

## 2. Agent & Studio Protocol

| Term | Technical Definition |
| :--- | :--- |
| **Native Studio Protocol** | The synchronous JSON-RPC 2.0 command-response interface operating over Unix domain sockets or loopback TCP, allowing agents and client SDKs to query and manipulate the DAW. |
| **Atomic Mutation Lease** | A concurrency guarantee ensuring that a sequence of DAW mutations (e.g., adding a track, inserting a clip, programming notes) executes as a single synchronized transaction on the engine's main thread. |
| **Perception Pipeline** | A two-stage audio understanding system: Stage 1 extracts deterministic acoustic features (Mel spectrograms, RMS energy, spectral centroid); Stage 2 passes structured summaries to multimodal LLMs for musical critique. |
| **Idempotency Key** | A unique caller-supplied token attached to mutating commands to ensure network retries or agent re-executions do not create duplicate tracks, clips, or notes. |
| **Subagent Handoff Packet** | A machine-readable payload emitted by a delegated subagent detailing task state, inspected files, physical test outputs, and verified evidence. |

---

## 3. Governance & Quality Assurance

| Term | Technical Definition |
| :--- | :--- |
| **Evidentiary Gate** | A milestone checkpoint in `ROADMAP.md` whose completion cannot be marked true without physical execution of an automated verification command and recording of dated test evidence. |
| **Cryptographic Domain Review** | A maintainer attestation recorded in `doc/knowledge/reviews.json` binding a reviewer name, date, and note to the exact SHA-256 hashes of all files in that domain. |
| **Source Drift** | A condition detected by `python3 scripts/docs-kb.py check` where C++ source files or documentation guides have changed without an updated maintainer review attestation. |
| **Scope Marker** | An HTML comment (`<!-- docs-scope: ... -->`) placed at the top of every documentation file strictly categorizing its authority level (`current-guide`, `domain-reference`, `decision-record`, `operational-record`, `research-reference`). |
