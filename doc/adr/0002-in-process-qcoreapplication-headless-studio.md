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

## Configuration lifetime clarification — 2026-10-15

**Supersedes:** deleting the process configuration singleton during each
`Engine::destroy()` call. The in-process studio decision above is unchanged.

Configuration is owned by the application QObject lifetime. Engine teardown
still saves configuration, but leaves it available for a subsequent engine
initialization in the same application. Application destruction releases the
singleton and clears its pointer. This avoids use of a freed configuration
object and preserves the selected configuration across engine lifetimes.
LV2 supported option registrations are cleared when their manager is destroyed,
so a new manager registers the new engine's URIDs without retaining the previous
manager's entries.

## macOS native event integration clarification — 2026-10-15

**Supersedes:** relying on Qt's default Unix event dispatcher for macOS core-only
startup. The QCoreApplication decision and absence of GUI initialization remain
unchanged.

CoreMIDI setup notifications use the run loop where its process client was
created. The macOS core-only executable selects Qt's Core Foundation dispatcher
before application construction, then restores the inherited selector before
creating later Qt workers. The main dispatcher remains integrated with the
native run loop. This integrates native setup changes while
retaining the existing Qt owner-thread delivery and coalesced device refresh.
The native CoreMIDI fixture uses the same startup choice and passed virtual
source discovery, input delivery, retirement cleanup and backend reconstruction.
It does not establish physical MIDI receipt or cross-platform behavior. See
[Qt's dispatcher selection](https://github.com/qt/qtbase/blob/5.15/src/corelib/thread/qthread_unix.cpp)
and [Apple's notification contract](https://developer.apple.com/documentation/coremidi/midinotifyproc).
