# Active Roadmap Actions & Open Gates

<!-- docs-scope: current-guide -->

Generated from [register.json](register.json). Open gates and hardening actions identify current implementation priorities. Dates are review targets, not promises of delivery.

---

## In-Flight Actions (Horizon 1 & 2)

| Gate ID | Horizon | Owner Role | Target Date | Description & Closure Criterion |
| :---: | :---: | :--- | :---: | :--- |
| **GATE-014** | H2 | AI Inference Maintainers | 2026-10-25 | **Live Multimodal Audio Critique & Revision Loop**: Validate `audio.listen` submitting rendered audio to a multimodal model, receiving qualitative aesthetic critique, and initiating an automated revision. Closure: Retain end-to-end trace with audio file and verified revised `.mmp`. |
| **GATE-015** | H2 | Hardware Maintainers | 2026-10-30 | **Hardware Audio/MIDI Hot-Swapping & Fault Recovery**: Verify live backend switching (`audio.backend.select`, `midi.backend.select`) between PortAudio, ALSA, and JACK without crashing, dropping transport state, or corrupting timelines. Closure: Pass dynamic backend switch tests under load. |
| **GATE-016** | H2 | Audio DSP Maintainers | 2026-11-05 | **Multi-Track Stem Export & Automated Loudness Compliance**: Implement simultaneous multi-track stem rendering and automated EBU R128 / ITU-R BS.1770 LUFS integrated loudness and true-peak measurement. Closure: C++ test asserting LUFS accuracy against reference audio. |
| **GATE-017** | H2 | Plugin Host Maintainers | 2026-11-15 | **Third-Party VST3 & CLAP Hosting via Carla Bridge**: Enable headless scanning, parameter discovery, and knob automation for external VST3 and CLAP instruments. Closure: Headless Carla VST3 test pass with audio verification. |
| **GATE-018** | H2 | Agent Systems Maintainers | 2026-11-20 | **Cross-Session Musical Preference & Palette Memory**: Validate `session.memory.set` for storing custom sound palettes, scale preferences, and stylistic heuristics across studio restarts. Closure: Multi-session restart test asserting recalled memory. |

---

## Strategic Backlog (Horizon 3)

| Gate ID | Horizon | Title | Status |
| :---: | :---: | :--- | :---: |
| **GATE-019** | H3 | Multi-Agent Jam Session & Stems Collaboration Mesh | `deferred` |
| **GATE-020** | H3 | Real-Time Neural Timbre Diffusion Synthesis | `deferred` |
| **GATE-021** | H3 | Zero-Latency WebSocket Remote Studio Relay | `deferred` |
