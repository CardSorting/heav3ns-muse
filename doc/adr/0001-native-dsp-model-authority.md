# ADR 0001: Native LMMS C++ Models as Authoritative State

<!-- docs-scope: decision-record -->

- **Status**: accepted
- **Date**: 2026-10-12
- **Domain**: domain-capabilities
- **Owner**: Studio Runtime Maintainers

---

## Context

AI music production tools typically operate by generating external MIDI files or isolated audio waveforms that are subsequently imported into a DAW. This creates a disconnect: the agent cannot inspect synthesizer settings, modify mixer effect chains, or understand why a clip sounds clipping or muddy. Alternatively, some tools maintain an external JSON representation of a project that is lazily exported to DAW formats, resulting in model drift and incomplete feature coverage.

## Decision

We establish that LMMS's native C++ project models (`Engine::getSong()`, `PatternStore`, `Mixer`, `TrackContainer`) are the sole authoritative representation of the song. `StudioCapabilities` operates directly on these in-memory C++ objects under engine mutex locks. No second AST, parallel JSON graph, or external proxy model is permitted.

## Consequences

- **Positive**: Projects are 100% native `.mmp` files, fully editable in the LMMS Qt GUI at any point.
- **Positive**: Full access to LMMS's 60+ native synthesizers, effects, and mixer routing graphs.
- **Trade-off**: Capability methods must execute on the Qt application thread and conform strictly to LMMS object lifecycles.
