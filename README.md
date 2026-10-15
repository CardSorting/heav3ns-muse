# HEAV3NS-MUSE

**An agent-native digital audio workstation, built on LMMS.**

HEAV3NS-MUSE is a fork of [LMMS](https://github.com/LMMS/lmms) being developed into a persistent, conversational music production environment. The objective is to make an agent a native participant in the studio: able to understand musical intent, work on the actual project, render and evaluate its decisions, and continue a composition across sessions.

The work extends the application's C++ runtime, project models, instruments, plugin hosts, audio engine, and persistence. LMMS's native project and DSP remain authoritative. Compositions should stay editable, their resources should remain traceable, and the studio should retain enough context to continue developing them.

**Development status:** substantial source changes and test fixtures are present. Full native build and runtime validation, real inference workflows, hardware testing, and the broader compatibility matrix remain outstanding. This is an active development fork. The [engineering record](doc/agent-native-architecture.md) documents implementation progress, limitations, and completion gates.

## The objective

A request such as:

> Continue this composition with a quieter second section. Keep the lead's character, revise the arrangement, render it, and evaluate the result before saving.

should become a continuing studio workflow:

```text
Interpret -> Plan -> Compose -> Render -> Listen -> Evaluate -> Revise -> Preserve
```

The target is an agent that can inspect what exists, choose native operations, observe their outcomes, recover from failures, and remember the decisions that shaped the music. Project state, conversation, plans, and rendered evidence belong to that continuing session.

## Work in the source tree

The fork includes implementation work across the following areas. These describe source coverage; runtime acceptance is tracked separately in the engineering record.

| Area | Work present |
| --- | --- |
| Persistent studio | An in-process `StudioRuntime`, session inspection, conversation history, native project checkpoints, and restart/recovery paths. |
| Agent execution | Configurable inference adapters, capability discovery, durable multi-step jobs, tool results, cancellation, resumption, and resource budgets. |
| Composition and arrangement | Native project, track, clip, note, selection, and beat-step operations, with project inspection and parameter discovery. |
| Instruments and resources | Instrument and preset loading, sample and sound-bank resources, tuning, and changes across bundled instruments and plugin hosts. |
| Mixing and automation | Native mixer channels, sends, ordered effects, parameter edits, automation, controllers, and linked controls. |
| Audio and MIDI | Backend selection and lifecycle work, transport, recording paths, MIDI handling, and device failure reporting. |
| Rendering and evaluation | Native mix/stem rendering, technical audio analysis, and a separate provider interface for perceptual audio evaluation. |
| Persistence and integrity | Atomic session manifests, request receipts, replay handling, revision checks, undo/redo, asset ownership, and storage maintenance. |
| Verification | C++/Qt tests, diagnostic fixtures, and process-level tests that exercise the native executable without GUI automation. |

The LMMS foundation supplies the song and pattern editors, piano roll, mixer, synthesis, effects, automation, MIDI, project formats, and plugin infrastructure. HEAV3NS-MUSE's compatibility work aims to retain those capabilities as they become available to the native agent runtime. Individual hosts, formats, and backends depend on the platform and compiled dependencies.

## How the studio is structured

The studio runtime lives inside the application. Its capability layer validates requests, reads the current native state, and applies operations to LMMS's existing objects. Inference providers propose actions through those capabilities; the runtime owns execution and persistence.

Session durability combines native `.mmp` checkpoints with an atomic manifest and retained conversation, job, and operation records. A single-writer lock protects a session directory. Revision checks and request receipts support recovery and replay handling.

Studio mode uses `QCoreApplication` for operation without a display. The graphical application remains part of the project, and the build still links Qt Widgets. The design gives the studio a native execution path with optional graphical presentation.

Useful starting points in the code:

| Component | Source |
| --- | --- |
| Application startup and studio mode | [main.cpp](src/core/main.cpp) |
| Session lifecycle and persistence | [StudioRuntime.cpp](src/core/StudioRuntime.cpp) |
| Native operation schemas and dispatch | [StudioCapabilities.cpp](src/core/StudioCapabilities.cpp) |
| Agent state and tool declarations | [StudioAgent.cpp](src/core/StudioAgent.cpp) |
| Inference and audio-provider adapters | [StudioInference.cpp](src/core/StudioInference.cpp) |
| Rendering and technical audio observations | [StudioAudio.cpp](src/core/StudioAudio.cpp) |
| Session storage maintenance | [StudioStorage.cpp](src/core/StudioStorage.cpp) |

## Build from source

The executable and CMake project currently retain the `lmms` name.

Clone this fork with its pinned dependencies:

```sh
git clone --recurse-submodules https://github.com/CardSorting/heav3ns-muse.git
cd heav3ns-muse
```

For an existing checkout:

```sh
git submodule update --init --recursive
```

The build requires a C++20 toolchain, CMake, Qt development libraries, libsndfile, and the dependencies for the selected instruments, plugin hosts, and platform backends. Qt 5.15 is the default baseline; the existing `WANT_QT6` option enables experimental Qt 6 support. Native tests also use Qt Test, and the process harnesses require Python 3.

See [INSTALL.txt](INSTALL.txt) and the [upstream compilation guide](https://github.com/LMMS/lmms/wiki/Compiling) for platform dependencies and toolchain setup. The fork also requires Qt Network for inference. Keep the full intended plugin configuration enabled when assessing compatibility.

Once dependencies are installed:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
```

Executable locations vary by platform and build generator. The commands below use `lmms` to mean the executable built from this fork; on Windows it is `lmms.exe`.

## Start a studio session

```sh
lmms studio /path/to/session --config /path/to/lmms-config.xml
```

The session directory holds the durable studio state. Use a configuration appropriate to your installation. Default studio startup uses dummy audio and MIDI devices, allowing native project work and offline rendering without opening hardware devices.

The interface accepts one JSON object per input line and returns JSON frames on stdout. Native diagnostics go to stderr; keep the two streams separate. Ordinary text lines become persisted conversational requests once a reasoning provider is configured.

For example, these requests discover capabilities and create a small native project:

```json
{"id":"discover","op":"studio.capabilities","args":{}}
{"id":"project","op":"project.new","args":{"empty":true}}
{"id":"track","op":"track.add","args":{"type":"instrument","name":"Lead"}}
{"id":"synth","op":"instrument.load","args":{"track":0,"instrument":"tripleoscillator"}}
{"id":"clip","op":"clip.add","args":{"track":0,"position":0}}
{"id":"note","op":"note.add","args":{"track":0,"clip":0,"position":0,"length":48,"key":60}}
{"id":"save","op":"project.save","args":{"path":"/absolute/path/to/song.mmp"}}
{"id":"render","op":"audio.render","args":{"path":"/absolute/path/to/song.wav"}}
```

Replace example paths with paths valid on your system. Timing uses native LMMS ticks. Discover current operation schemas and parameter bounds through `studio.capabilities`; the [studio protocol](doc/native-studio-protocol.md) describes the request and response contracts.

To open configured audio and MIDI devices:

```sh
lmms studio /path/to/session --config /path/to/lmms-config.xml --studio-live
```

`--studio-audio <name>` and `--studio-midi <name>` select compiled backends when used with `--studio-live`. Discover backend names with `audio.backend.list`. Check the reported actual backend and failure state: device startup can fall back to dummy devices. Hardware behavior still needs native validation.

## Configure conversational reasoning

Choose a provider endpoint and model explicitly through `session.provider.set`. The source includes adapters for Responses and Chat Completions protocols, including compatible local endpoints. Authenticated configurations reference a credential environment variable through `keyEnv`; the session stores its name, and the adapter reads the credential when making a request.

Then submit musical intent through `session.chat` or an ordinary text line. Jobs expose budgets and lifecycle controls for inspecting, cancelling, and resuming work. See the [provider configuration and conversation examples](doc/native-studio-protocol.md#configuring-reasoning-and-audio-understanding) for the full setup.

Reasoning and audio understanding have separate provider roles. Technical analysis reports measured properties of rendered audio. Perceptual listening requires an explicitly configured, compatible audio provider; provider compatibility and musical judgment need live evaluation.

## Verification and remaining work

Run the native test suite after building:

```sh
ctest --test-dir build --output-on-failure
```

Run the process harness against the actual compiled executable, adjusting its path for your build:

```sh
python tests/studio_process_test.py --lmms build/lmms --output build/studio-evidence
```

The harness exercises studio startup, protocol behavior, native edits, persistence, and audio-related paths without a display. Additional host-specific harnesses are in [tests/](tests/). Authored fixtures and scripted provider responses establish test intent; passing results require execution against the native application.

The main acceptance priorities are:

- Compile the full application and run the native and process-level suites.
- Validate real conversational composition, rendered audio evaluation, revision, and session continuation.
- Exercise audio/MIDI hardware, recording, backend failures, and shutdown across supported platforms.
- Verify bundled instruments, external plugin hosts, project and preset round trips, and asset recovery.
- Measure resource use and responsiveness during sustained studio work, including cancellation and recovery.

The [engineering record](doc/agent-native-architecture.md) and [studio protocol](doc/native-studio-protocol.md) contain the detailed evidence requirements and known limitations.

## Contributing

Development of this fork is tracked in [CardSorting/heav3ns-muse](https://github.com/CardSorting/heav3ns-muse). Use this repository's [issues](https://github.com/CardSorting/heav3ns-muse/issues) and pull requests for HEAV3NS-MUSE changes.

Contributions should connect a studio capability to its native implementation and observable outcome. Include reproduction steps, build configuration, relevant tests, and any project or audio evidence needed to assess the change. Document platform and dependency limits alongside results.

Start with the architecture record when extending agent behavior and the protocol document when changing operations. Preserve project editability and native ownership, and record what was actually compiled, executed, and verified.

## Lineage and license

HEAV3NS-MUSE builds on the work of the LMMS developers and contributors. The fork's agent-native development began from upstream commit `a2f57e70c`.

The application follows LMMS's **GPL-2.0-or-later** licensing. See [LICENSE.txt](LICENSE.txt) and the notices in individual source files. Bundled assets and third-party dependencies retain their respective copyright and license notices.
