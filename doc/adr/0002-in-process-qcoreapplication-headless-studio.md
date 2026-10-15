# ADR 0002: In-Process QCoreApplication for Headless Studio Execution

<!-- docs-scope: decision-record -->

- **Status**: accepted
- **Date**: 2026-10-12
- **Domain**: domain-studio-runtime
- **Owner**: Studio Runtime Maintainers

---

## Context

Running digital audio workstations in cloud containers, automated test runners, or headless developer workstations usually requires dummy X11 servers (e.g. `xvfb-run`), simulated mouse clicks, or screen-scraping hacks. These methods are brittle, resource-intensive, and prone to silent failures.

## Decision

We introduce an in-process studio mode selected by the `studio` command line argument. When `studio` is invoked, `src/core/main.cpp` initializes `QCoreApplication` instead of `QApplication` (or constructs widgets lazily without mapping them to a display server). `Engine::init(true)` boots the native engine with dummy audio/MIDI backends by default, isolating all machine communications to line-delimited JSON stdio streams.

## Consequences

- **Positive**: Zero display server dependencies. The studio runs in minimal headless Docker containers, CI pipelines, and remote SSH sessions.
- **Positive**: Strict stdio stream isolation: protocol JSON frames are written to `stdout`, while C++ logs and warnings are directed to `stderr`.
- **Trade-off**: Real audio playback requires explicitly providing `--studio-live` and a hardware driver name.
