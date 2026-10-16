# Active Roadmap Actions & Open Gates

<!-- docs-scope: current-guide -->

Generated from [register.json](register.json). Dates are review targets, not delivery promises.

[Engineering record](../agent-native-architecture.md) describes observed results and current limitations.

| Gate | Horizon | Status | Owner | Target date | Description and closure criterion |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **GATE-014** | H2 | in-progress | AI Inference Maintainers | 2026-10-25 | **Live Multimodal Audio Critique & Revision Loop**: audio.listen submits rendered audio to a real multimodal audio model, receives qualitative aesthetic critique and incorporates it into an automated composition revision. Retain an end-to-end trace, the submitted audio file and a verified revised .mmp project. |
| **GATE-015** | H2 | in-progress | Hardware Maintainers | 2026-10-30 | **Hardware Audio/MIDI Backend Hot-Swapping & Fault Recovery**: Dynamic audio.backend.select and midi.backend.select switching between PortAudio, ALSA and JACK under load without crashes, dropped transport state or timeline corruption. Retain passing dynamic backend switch tests on the observed hardware and platforms. |
| **GATE-016** | H2 | open | Audio DSP Maintainers | 2026-11-05 | **Multi-Track Stem Export & Automated Loudness Compliance**: Simultaneous multi-track stem rendering with EBU R128 / ITU-R BS.1770 integrated LUFS loudness and true-peak compliance measurement. Retain a passing C++ accuracy test against reference audio. |
| **GATE-017** | H2 | open | Plugin Host Maintainers | 2026-11-15 | **Third-Party VST3 & CLAP Hosting via Carla Bridge**: Headless Carla scanning and hosting of external VST3 and CLAP instruments with parameter discovery and automation modulation. Retain a passing headless Carla VST3 process test with decoded audio verification and explicit CLAP coverage. |
| **GATE-018** | H2 | open | Agent Systems Maintainers | 2026-11-20 | **Cross-Session Musical Preference & Palette Memory**: session.memory.set preserves compositional rules, favorite instruments, sound palettes, scale preferences and stylistic heuristics across studio restarts. Retain a multi-session restart test asserting recalled memory. |
| **GATE-019** | H3 | deferred | Agent Systems Maintainers | Unscheduled | **Multi-Agent Jam Session & Stems Collaboration Mesh**: Multiple autonomous agents collaborate on separate tracks with shared tempo clock and stem listening. |
| **GATE-020** | H3 | deferred | Audio DSP Maintainers | Unscheduled | **Real-Time Neural Timbre Diffusion Synthesis**: Native C++ bridge to real-time neural diffusion models for acoustic timbre transformation. |
| **GATE-021** | H3 | deferred | Studio Runtime Maintainers | Unscheduled | **Zero-Latency WebSocket Remote Studio Relay**: WebSocket transport allowing browser and mobile agents to drive in-process studio runtime across networks. |
