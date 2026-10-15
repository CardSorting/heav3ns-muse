# HEAV3NS LMMS engineering record

The deliverable is the complete LMMS application with persistent conversational
agency and optional graphical presentation. Native project models and DSP remain
authoritative. Structured commands, a rendered song, or an interoperability server
alone do not satisfy this commission.

## Investigation baseline

Inspected source: upstream commit `a2f57e70c`. The initial checkout was clean and
contained no agent runtime. `src/core/main.cpp` already selected `QCoreApplication`
for offline rendering and `Engine::init(true)` initializes the native engine.
Qt Widgets is still linked; linking a presentation library differs from requiring
a display, constructing widgets, or operating them. All runtime verification must
use no display server and no GUI automation.

| Subsystem | Native authority | Presentation dependencies and work required |
| --- | --- | --- |
| Lifecycle and configuration | `Engine`, `AudioEngine`, `ConfigManager`, `main.cpp` | GUI startup owns normal session recovery; render mode has no persistent command lifecycle. Headless configuration must avoid device and setup dialogs. |
| Arrangement and track organization | `Song`, `TrackContainer`, `Track`, `Clip`, `Timeline` | Views are mostly separate. `Clip::copyStateTo` dereferences the automation editor; Song loads/saves timeline only inside GUI branches. Preserve project metadata on headless round trips. |
| Piano roll and MIDI notes | `InstrumentTrack`, `MidiClip`, `Note`, `StepRecorder` | `MidiClip::addNote` obtains GUI quantization by default. Native operations must pass explicit quantization or disable it. Expose note properties, detuning, editing, clip splitting, and MIDI import/export. |
| Beat and bassline sequencing | `PatternStore`, `PatternTrack`, `PatternClip`, step notes in `MidiClip` | PatternStore owns native pattern indices and clip positioning. Editor combo state must not become an alternative pattern model. Verify nested pattern save/load and rearrangement. |
| Synthesis and sound design | `Instrument`, `InstrumentTrack`, `InstrumentFunctions`, `InstrumentSoundShaping`, envelope/LFO models, `plugins/` | Native controls generally inherit Model and AutomatableModel. Instrument views and pixmap resources must remain lazy. Audit every bundled instrument, presets, samples, soundfonts, patches, waveforms, microtuning and keymaps. |
| Plugin discovery and hosting | `PluginFactory`, `Plugin`, `Ladspa2LMMS`, `Lv2Manager`, `RemotePlugin`, VST and Carla hosts | Subplugin identity needs descriptor plus attributes. Carla dialog/completion paths now separate presentation in source; actual Carla and third-party plugin compatibility remains unverified. Preserve supported hosts and document concrete plugin-specific incompatibilities. |
| Mixer and effects | `Mixer`, `MixerChannel`, `MixerRoute`, `AudioBusHandle`, `EffectChain`, `Effect`, effect controls | Routing graph and cycle detection already exist. EffectRackView has privileged access to chain state. Expose native ordered effect operations without synthesizing a second graph. |
| Automation and controllers | `AutomatableModel`, `AutomationClip`, `AutomationNode`, `Controller`, `ControllerConnection`, `LfoController`, `PeakController` | GUI dragging selects model IDs but execution is native. IDs must be rediscovered after project replacement. Preserve connections, interpolation, tangents, tempo automation, MIDI controller links and native controller DSP. |
| Samples and audio recording | `SampleTrack`, `SampleClip`, `Sample`, `SampleBuffer`, decoder and record/play handles | Sample errors have some GUI guards; audio recording and clip fades require native controls and backend verification. Keep asset-relative paths, project bundles and dependency reporting. |
| Audio processing and transport | `AudioEngine`, worker threads, `Mixer`, `Song`, audio/MIDI backends | Existing render-only mode suppresses hardware output; real-time headless playback/recording needs backend selection and non-modal failures. Mutations must follow native model locks and main-thread affinity. |
| Rendering and exports | `ProjectRenderer`, `RenderManager`, `OutputSettings`, audio file devices, MIDI exporter | Rendering is native but reports limited output failure information. Verify full mix, stems, loops, rate/depth, codecs, automation and plugin output. Export is not listening. |
| Project compatibility and history | `DataFile`, `Song`, `SerializingObject`, `JournallingObject`, `ProjectJournal`, `PathUtil` | DataFile owns format upgrades and bundle checks. Failed loads can leave loading/journalling flags set. GUI-only nodes are discarded on headless save. Session-level revisions must capture the entire native project, including mixer and pattern state. |
| Existing tools and visual analyzers | `ToolPlugin`, LadspaBrowser, TapTempo, Oscilloscope, SpectrumAnalyzer, Vectorscope | Preserve useful model/DSP functions; provide analysis observations without opening views. Pure display interactions need optional presentation, not simulated clicks. |
| Build, tests and packaging | root/src/plugin CMake, `cmake/modules/PluginList.cmake`, platform packaging, QtTest tests | Default full plugin set must remain. Minimal builds can diagnose failures but cannot prove full capability. Add no-display integration coverage alongside upstream tests and retain normal GUI startup. |

## Target architecture and invariants

1. LMMS owns a StudioRuntime inside its executable. The native engine is initialized
   once per session. All capabilities operate on `Engine::getSong()`, PatternStore,
   Mixer, native instruments and effects on the application thread.
2. A capability layer validates typed arguments, exposes current native state,
   applies native operations under engine model guards, and returns actual outcomes.
   It must grow to cover the whole subsystem map above. Revision-scoped indices
   are initially explicit; persistent symbolic handles must not retain deleted
   QObject pointers across restores.
3. Session persistence uses native `.mmp` checkpoints plus an atomic manifest,
   transcript, preferences, plans, operation results and recovery state. A committed
   manifest is the session commit point; orphan snapshots are not applied during
   recovery. A single-writer lock prevents concurrent session corruption. Agent
   revisions complement the existing GUI journal.
4. Conversational planning belongs to this runtime. Pluggable remote/local inference
   supplies reasoning, not a separate project generator or controller. Planning
   consumes capability schemas and current project observations. Tool results,
   intent, rationale and preferences are retained across sessions.
5. The runtime executes durable, bounded multi-step plans with explicit statuses,
   cancellation, failure recovery, dependency/asset checks, CPU/memory/render budgets
   and resumption. It owns Interpret → Plan → Compose → Render → Listen → Evaluate
   → Revise → Preserve. Neither fixed keyword parsing nor a fixed song template
   establishes general conversational agency.
6. Audio evaluation separates measured DSP facts (clipping, silence, spectrum,
   loudness, duration) from subjective judgments. An available audio-understanding
   provider receives rendered audio and returns attributable judgments. If absent,
   the runtime must report that perceptual listening is unavailable.
7. Text transports and optional MCP adapt the same runtime. No transport replaces
   the studio, no GUI automation is allowed, and no parallel composition/DSP model
   is authoritative. GUI operation stays supported.

## Completion gates (all remain required)

| Commission requirement | Authoritative completion evidence | Current status |
| --- | --- | --- |
| Headless startup/shutdown | Full build launched without display; clean device/thread/plugin teardown, signal and EOF tests | Pending runtime execution |
| Persistent natural-language studio | Multi-session chat traces with current composition, preference and decision recall | Source implementation present; real inference and execution unverified |
| Native project creation/editing | Live model observations and `.mmp` round trips, including undo/redo and load failures | Source implementation in progress; unverified |
| Full sequencing/arrangement | Model-level and end-to-end operations for all supported track/clip/note/pattern features and musical time | Incomplete |
| Instruments/plugins | Full bundled instrument/effect and supported-host matrix, parameter/preset/asset round trips and real audio | Incomplete |
| Mixing/routing/automation | Rendered evidence for ordered effects, arbitrary supported routing, cycle rejection, parameter and controller automation | Incomplete |
| Render/export | Valid decoded mix/stems in enabled codecs, MIDI export, loop/tempo/depth/rate and regression comparisons | Incomplete |
| Listen/evaluate/revise | Attributable technical and perceptual assessments followed by native revisions and a new evaluation | Provider audio submission implemented in source; real evaluation unverified |
| Autonomous workflows | Durable interpret-to-preserve traces, multi-step continuation, budget/cancel/failure tests | Durable job loop implemented in source; musical workflows unverified |
| Memory/checkpoints/recovery | Restart, hard-kill at commit boundaries, corrupt manifest, single-writer, idempotency and revision tests | Source implementation in progress; unverified |
| Continue existing compositions | Reopened real projects changed through conversation with native editability retained | Incomplete |
| Ecosystem compatibility | Bundled and historical project/preset corpus, assets/bundles, all native metadata preserved; explicit evidenced exceptions | Incomplete |
| Engineering evidence | Source diffs, tested operation inventory, audio correctness, resource metrics, regressions and unsupported cases | This record started; full evidence pending |

## Environment and verification record

2026-10-12: Filesystem and source inspection succeeded. Docker engine access and
WSL enumeration return access denied in the managed sandbox. An elevated Docker
inspection was automatically rejected because sandbox escalation is disabled.
No CMake, Ninja, C++ compiler or Qt development installation was found by initial
PATH and targeted installation searches. Downloading development dependencies
from the shell failed with `WinError 10013` (network sockets denied). These facts
limit native build verification in this environment; they do not establish any
application capability as tested or remove any completion gate.

Current `git submodule status` also reports all 17 pinned submodules uninitialized,
including AdPlug, ZynAddSubFx, Carla and the bundled synthesis/effect libraries.
Those sources must be restored at their pinned commits for a complete native build;
checking upstream API text does not replace a dependency checkout or compilation.

Work began from a verified clean baseline and continues as source implementation.
The following record includes consecutive implementation passes; no native build
process is assumed to be running.

## Source changes (runtime evidence pending)

- Added `StudioRuntime`, `StudioCapabilities` and `StudioAudio` to the application's
  core build and added `lmms studio <session-directory>` to native startup. The
  model, plugin and DSP implementations remain LMMS's existing implementations.
  Capability schemas and actual limitations are recorded in
  [native-studio-protocol.md](native-studio-protocol.md).
- Added native complete-project checkpoints, atomic manifests, a single-writer
  lock, bounded request replay/memory, undo/redo, optimistic revision checks and
  rollback. Added native offline render and libsndfile technical observations,
  with no claim of perceptual listening.
- Validated persisted request hashes/IDs/responses and all checkpoint references
  before recovery. Failed undo/redo loads restore the current native checkpoint.
  Request errors retain their IDs, and startup exposes its actual application type.
- Changed native Song loading to return a usable result and restore lifecycle
  flags on early rejection. Retained GUI/extension nodes and project notes on
  headless save, and moved timeline persistence out of GUI-only branches. Added
  a native empty-project reset and guarded clip copying and note quantization.
  Loader traversal now advances past track-container comments/extension elements;
  loading restores the caller's journal state and clears its loading flag on exit.
- Replaced unchecked file publication in DataFile with QSaveFile atomic commit
  and checked backups. Initialized failed DataFile reads to Unknown. Expanded
  native resource handling for bundled instruments and made resource permission
  checks attribute-specific. Native checkpoints resolve original bundle-local
  asset paths rather than rebasing them accidentally to the session directory.
- Used native preset replacement, which retains mute/solo/routing. Exposed the
  native ordered effect chain and detached audio-bus effect controls through
  capability discovery, rather than introducing a separate chain/control model.
- Guarded legacy LADSPA upgrade and PeakController diagnostics and LADSPA load
  failure dialogs for headless execution. ImportFilter now returns its actual
  result and reports headless errors without widgets. MIDI import constructs its
  progress dialog only for GUI sessions and writes diagnostics to stderr. MIDI
  export returns its actual file commit outcome, owns its plugin lifetime, uses
  an atomic output file and excludes non-instrument pattern tracks from MIDI
  instrument casts/counts.
- Removed process exit on headless audio-output open failure. Added encoder write
  and finalization status, safe OGG/MP3 partial-initialization cleanup, FLAC open
  failure propagation and native Unicode file-descriptor use. Native rendering
  checks encoding before publishing audio; render cancellation/progress flags now
  use atomics. Added failure-survival, Unicode lossless codec, finalization and
  invalid-replay/undo regression cases, which still require native execution.
- Added `StudioRuntimeTest` to QtTest/CTest with native model, metadata, loop,
  note, routing, backup, asset, decoded-audio, rollback, replay, history, lock and
  corrupt-recovery assertions. Added `StudioProcessTest` for full-plugin builds
  when Python is available. Its instrumentation launches LMMS with no display,
  checks real rendering against an independent WAV decode, exercises MIDI I/O,
  restart, abrupt-kill recovery and continued native edits, and saves traces.
- Added `StudioInference`, `StudioAgent` and `StudioStdio` inside the LMMS core.
  Qt Network sends configurable Responses or Chat requests without storing API
  credentials. Function declarations come from native capability schemas; the
  adapter validates provider decisions and preserves stateless reasoning history.
  Windows Qt 6 install rules also include available TLS backend plugin targets;
  TLS availability is checked before HTTPS transport. Packaging remains untested.
- Added durable conversation, musical plans, explicit per-task outcomes and
  autonomously scheduled native tool execution. Each successful edit and its job
  cursor/result share a manifest commit. Budget reservation, cancellation, failure
  replanning, restart handle invalidation and archived prior jobs are explicit.
  Native stdin polling allows control requests during render/network event loops.
- Added scoped, hashed PCM WAV submission to a separately configured audio model.
  Technical measurements remain separate from provider judgments. No chosen
  provider, real audio evaluation or musical-quality result has been established.
- Expanded native tests for provider decisions driving live LMMS edits, checkpoint
  continuity, cancellation, budgets, raw conversation preservation, dependent-call
  failures, explicit outcome evidence and actual waveform submission. Added
  `StudioInferenceTest` for protocol shapes, encrypted reasoning preservation,
  malformed calls and real loopback HTTP deadlines/cancellation/limits. Expanded
  process instrumentation with ordinary Unicode conversation, unsolicited events,
  native render/evaluate calls and cancellation during inference. All fixture
  behavior is test instrumentation, not production reasoning or quality evidence.

Verification actually performed: Python integration-harness syntax compilation
and its CLI help succeed; `git diff --check` succeeds. Source-only delimiter and
whitespace checks cover new studio files and delimiters were checked across
116 studio and modified native C++ headers, implementations and test files;
the inventory confirms 86 native operations plus four agent tools and core build
entries. The Python loopback provider fixture was exercised over actual local
HTTP, including WAV decoding; this validates the test fixture only. CMake configuration was
attempted and failed before configuration because `cmake` is not installed on
PATH (exit 127). The process harness was invoked against the expected native
binary path and correctly refused to run because no compiled executable exists.
No native C++ test, render, compatibility result, CPU/memory benchmark or runtime
success is claimed. Live conversational inference, perceptual evaluation, complete
capability coverage, plugin compatibility, sustained resource supervision and
full-system verification remain outstanding; the commission is active.

The current source also extends native sample and controller operation:

- Sample tracks use playback snapshots sharing immutable decoded buffers. Render
  startup no longer overwrites a SampleClip's editable source frame boundaries.
  Native project XML persists source bounds, gain, reversal and clip name, while
  legacy projects retain full-buffer defaults. Sample clones preserve native
  length and offset. Embedded sample rates and source bounds are validated.
- Studio capabilities expose native sample decoding, source trim/gain/reversal,
  signed clip offsets, automatic resizing and non-destructive clip splitting.
  The GUI knife and reverse actions delegate to those same native model methods.
  Waveform drawing handles empty/tiny buffers, selected ranges and reversal;
  automation ghost lengths use the selected source duration and its sample rate.
- Rack LFO controllers are discoverable, creatable, connectable, renameable and
  removable. Existing peak controllers can be connected through the same native
  mechanism; their effects retain ownership. Connection rewiring disconnects old
  destruction signals, keeps native counts consistent and respects MIDI ownership.
  Dependency checks terminate on malformed cycles and include linked parameters;
  finalization removes cyclic controller edges before native DSP evaluates them.
  Tempo-sync modes and custom positive meters use existing native models;
  switching modes does not accumulate duplicate custom-meter connections.
- Native QtTest coverage was authored for snapshot DSP gain, source immutability,
  sample XML and legacy restoration, splitting, empty detuning copies, controller
  rewiring/deletion/cycles, tempo-sync recovery and waveform ranges. The process
  harness additionally authors checks using known PCM at 22,050 Hz and actual
  native renders at 44,100 Hz, independently decoded for gain, reversal and split
  comparisons, followed by session restart. These native tests have not run.

The sample DSP also avoids dereferencing an empty resampler input span and checks
source indices before reading decoded frames. This source progress does not prove
live runtime behavior. MIDI controller setup, waveform coverage across all plugins, complete automation
and detuning verification, recording/backend work and full plugin compatibility
remain incomplete, alongside the other completion gates above.

Native curve and expression operations now also exist in source:

- Arrangement automation can be inspected, evaluated, disconnected, cleared and
  edited at exact source ticks. Points expose native in/out discontinuities,
  discrete/linear/cubic Hermite interpolation, tension, generated tangents and
  locked manual tangents. Target bounds and all curve arguments are validated
  before writing a point. Disconnect retains raw curve values.
- Existing hidden global automation is inspectable without allocating a curve.
  Native global point, interpolation, removal and clearing operations are
  available for discovered project parameters. Song now saves and restores the
  existing hidden track using its native XML representation; project clearing
  removes stale hidden targets and journal IDs. Empty connected curves retain
  target model IDs on save. This format change requires compatibility-corpus
  verification, including legacy upgrades and GUI round trips.
- Per-note detuning uses Note's existing DetuningHelper and AutomationClip.
  Positions are relative to note start and values are semitones within native
  bounds. Inspection avoids creating the lazy curve. Native note cloning keeps
  independent detuning data and retargets its curve to the copied helper.
- Native target disconnection removes destruction callbacks; copies connect their
  own callbacks. Restoration avoids duplicate bindings, and curve previews safely
  handle empty/end iterators. Agent observations summarize curve point counts
  while explicit native inspection returns full curve data.
- Seven additional QtTest cases were authored for native spline/jump values,
  target lifetime and restoration, read-only global inspection, actual
  NotePlayHandle frequency ratios, independent detuning clones, recovery/undo,
  empty binding persistence and observation summaries. The process harness now
  also compares actual rendered PCM under arrangement/global volume automation
  and measures an octave detuning ratio from native oscillator output.

Those native checks remain unrun. The source diagnostic checks all 106 modified
C++ files for balanced delimiters and verifies the schema inventory, required
fields, core build entries, Python syntax and diff whitespace. It is not a
compiler or evidence of native DSP, provider, musical-quality or performance
success. All full-system completion gates remain required.

Native instrument and waveform access now also exists in source:

- Instrument inspection exposes current native parameters, model paths, control
  types, bounds, named combo choices, file resources and native chord intervals.
  Parameter listing can be scoped to a song/pattern track or a rack controller.
  Integer/boolean controls reject fractional inputs. Generic discovery reaches
  native arpeggio enable, the microtuner, all MIDI CC models and TripleOscillator's
  anti-alias controls because those models have their native owners as parents.
- A checked native plugin resource interface reports failure explicitly while
  retaining the legacy file-loading method. AudioFileProcessor sample loading and
  all three TripleOscillator user waveforms use native decoding; LFO waveform
  loading operates on the existing controller. Empty paths clear resources and
  optional selection activates the native user-wave parameter. Decoder failure
  retains the previous resource. GUI waveform actions delegate to the same native
  setter; native XML retains its existing resource attributes. Empty restoration
  clears stale resources. Other plugin resources remain unmapped.
- The libsndfile decoder validates frame/channel/rate bounds, complete reads and
  finite samples, and closes its handle on early failure. Optional decoder lists
  use their actual length, preventing a null fallback call without Vorbis support.
- Five additional QtTests were authored for discovered controls and choices,
  instrument-function/microtuner recovery, empty/nonfinite decoder input, native
  LFO sample values and preservation on failure, and waveform/selector recovery.
  The full-plugin process harness now requires AudioFileProcessor as well as
  TripleOscillator, compares their actual WAV output, checks native arpeggio
  changes, and tests resource clearing, failed loading and restart restoration.
  Those native tests remain unrun. Source delimiter checks cover 106 changed C++
  files; the schema inventory is 86 native operations plus four agent tools.

Changing the native plugin and model interfaces requires rebuilding native LMMS
plugins with the application. Existing native plugin binaries, hosted plugins,
project/preset compatibility, live musical inference and all full-system gates
still require execution evidence. Remaining resource adapters and complete
parameter labels remain capability gaps.

Native tuning-library operation now also exists in source:

- Scale and keyboard-map parsing/file exchange moved out of MicrotunerConfig
  into native core TuningFile methods. Both the GUI and studio use native
  Scale/Interval/Keymap objects, bounded parsing, checked atomic UTF-8 writes and
  exact native float round trips. Ten studio operations inspect/edit the library,
  import/export SCL/KBM and inspect instrument frequencies. Their schemas are
  automatically available to the application's reasoning provider.
- Keyboard maps distinguish a valid signed degree -1 from an unmapped x and
  retain the formal octave degree. Native microtuning uses signed degree arithmetic
  and the existing scale's period, including negative degrees and formal periods.
  Zero formal period retains the final-scale-degree behavior. Scala zero-sized
  maps use their middle key; legacy native maps retain absolute-key semantics.
  Exporting a legacy empty map normalizes the Scala middle field to zero to
  preserve frequencies. New optional native XML fields retain these distinctions;
  older-application compatibility remains untested.
- Frequency inspection and native note playback share InstrumentTrack's native
  nominal calculation. Library and tuning-control changes invalidate active
  NotePlayHandles through the existing frequency-update mechanism. Invalid or
  unmapped frequencies skip MIDI note-on, instrument DSP and automatic beat-length
  calculation. Native PlayHandle processing clears buffers before each period.
  Constant-pitch scales keep a nonzero piano-roll key stride. The optional GUI
  exposes the formal degree and no longer clamps reference frequency to its old
  display bounds or rounds imported frequencies to a short decimal representation.
- Song clears the complete tuning library on project replacement. Restoration
  ignores unrelated XML nodes and validates native objects before atomic publication;
  invalid tuning objects leave safe defaults and visible native project warnings.
  This does not constitute exhaustive validation of malformed project XML.
- Six additional QtTest cases cover parsing, precision, rejected definitions,
  signed and formal mappings, silence for unmapped automatic-length notes,
  active-note retuning, native/Scala serialization, recovery/undo and loading an
  older project without tuning data. The full-plugin process harness additionally
  checks actual oscillator WAV pitch ratios, complete silence for an unmapped key,
  import/export, rejected imports and tuning recovery/replacement.

Source diagnostics pass for 106 changed C++ files, 86 unique native operation
schemas, required fields, core build entries, Python syntax and diff whitespace.
The native tuning tests and rendered-audio comparisons remain unrun. No compiler,
CMake, Qt development tools or compiled LMMS executable are available on PATH.
Full tuning/plugin compatibility, live inference/evaluation, resource supervision
and every full-system completion gate still require execution evidence.

Native sound-bank loading and program access now also exists in source:

- Native instruments deliberately have no track QObject parent. Discovery now
  includes each current instrument as an explicit root and resolves its actual
  song/pattern track owner. This repairs access to instrument-owned controls that
  track-only discovery missed, including the previously authored waveform checks.
- The native Instrument interface exposes cached bank/program names and checked
  selection. SF2 Player and GIG Player populate this cache through their existing
  FluidSynth/libgig loaders. `instrument.program.list` provides bounded pagination,
  bank filtering and case-insensitive name search; `instrument.program.select`
  selects the complete pair. Instrument inspection reports selection availability.
  Both bank models now cover 0 through 16383. Structured parameter writes to their
  bank/patch controls use checked pair selection. Legacy GUI/MIDI model writes
  retain their existing semantics and still require compatibility testing.
- SF2 Player exposes the `soundfont` resource and GIG Player the `bank` resource.
  They stage new native banks and reject unreadable, invalid or programless files
  before replacing the previous resource. Empty paths clear native state. GUI and
  XML loading share these native loaders; structured loading preserves the track
  name. SF2 candidate cleanup also covers exceptions during program discovery.
  FluidSynth loading uses reset-presets false and selection uses its checked native
  program API. LMMS already requires FluidSynth 2.2, whose Windows loader supports
  UTF-8 paths. These are source/API properties, not executed compatibility results.
  See [FluidSynth SoundFont management](https://www.fluidsynth.org/api/group__soundfont__management.html)
  and [native program selection](https://www.fluidsynth.org/api/group__midi__messages.html).
- SF2 tuning-table construction uses the inverse of its native MIDI base-note
  shift when requesting InstrumentTrack's nominal frequency. This keeps a tuned
  key's intended pitch when its base note changes. Render/sample-rate changes and
  bank replacement clear the cached resampler output. Held-note voice lifetimes,
  all rates, other SoundFonts and valid GIG playback still require native evidence.
- Five QtTest cases were authored for unsupported instruments, detached control
  discovery, SF2 loading/selection/failure preservation, recovery/undo/presets and
  invalid/empty GIG loading. The full-plugin process harness now additionally
  requires SF2 Player and GIG Player and measures actual SF2 WAV pitch, octave
  program changes, microtuning/base-note behavior, unmapped/cleared silence and
  restart restoration. A small original PCM SoundFont is a test asset only.
  Its byte regeneration, RIFF/table bounds and original 440 Hz source PCM are
  checked independently; native FluidSynth acceptance remains unrun. Valid GIG
  asset/audio coverage has not been authored. Optional-plugin QtTest skips do not
  establish complete instrument coverage.

Source diagnostics still cover 106 changed C++ files and 86 native operations plus
four agent tools. They do not compile C++, load a bank or render native audio.
All new native tests remain unrun. Full builds must rebuild the changed native
plugin interfaces, retain the complete plugin set, and verify GUI/project/preset
compatibility, hosted plugins, live inference/evaluation and resource supervision.
The complete LMMS commission remains active.

Native GUS patch loading now also uses a checked source implementation:

- Patman exposes its existing patch through the native `patch` file resource.
  Studio requests, the GUI's setFile/loadFile methods and native XML restoration
  share the checked loader. A failed load retains the old path and complete sample
  collection; GUI naming happens only after a successful load, while structured
  resource loading preserves the track name. Empty paths clear the collection and
  emit its native change signal. Errors enter the native project's diagnostics.
- QFile replaces narrow fopen so native Unicode paths use Qt's file handling.
  Candidate decoding validates complete headers, supported instrument/layer counts,
  nonempty samples, remaining file bytes, PCM alignment, positive source rates/root
  frequencies, native signed frame/vector capacity and active loop bounds before
  each sample allocation. Truncated later samples discard the entire candidate.
  The loader decodes bounded blocks into native SampleBuffers, removing the old
  temporary mono array and extra stereo copy. Its handle and candidate samples are
  owned during early exits; allocation failure reports a load error.
- Eight- and sixteen-bit signed/unsigned PCM use explicit numeric conversion,
  avoiding platform-dependent signed-char interpretation. Native nearest-root
  sample selection, tuning, loop playback and held-note Sample/PlaybackState
  ownership remain authoritative. Loop/tune controls now have discoverable names.
  The existing GUS subset remains: at most one instrument/layer, nearest-root
  selection and forward loops. Patman's upstream parser did not use GUS frequency
  zones, fractional loop positions, envelopes, tremolo, vibrato or reverse/ping-pong
  mode flags; this change does not implement those format features. These limits
  are visible in Patman's native parser/playback code, and the layout/encodings can
  be compared with [SDL_mixer's native TiMidity GUS reader](https://raw.githubusercontent.com/libsdl-org/SDL_mixer/SDL2/src/codecs/timidity/instrum.c).
- Four QtTest cases were authored for all PCM encodings and Unicode paths,
  malformed/truncated metadata including a later sample, active-note sample
  retention during successful replacement, and presets/recovery/undo/empty XML.
  The full-plugin process harness now requires Patman and additionally measures
  native WAV output for every encoding, nearest-sample choice, loop/tune behavior,
  failed/empty loading, presets and audio after restart. Its replay assertion uses
  a recent resource request inside the existing bounded idempotency window.
- Four original GUS assets contain two diagnostic samples. Their byte regeneration,
  bounds and source pitches pass independent fixture-only checks; signed/unsigned
  variants decode to the same original integer PCM. The second sample intentionally
  declares an 880 Hz root while containing 660 Hz PCM, distinguishing native sample
  selection from simply transposing the first sample. These are test data, not
  production generation or native execution evidence.

Source checks pass for 106 changed C++ files, 86 native operations, Python syntax
and diff whitespace. All new native Patman tests remain unrun: no CMake/compiler,
Qt development tools or compiled LMMS executable are available on PATH. Complete
GUS corpus/project/GUI compatibility, native DSP correctness, resource supervision,
remaining hosted-resource access, and every commission completion gate
still need implementation or execution evidence. The full goal remains active.

Native SlicerT access and playback now have a shared source implementation:

- SlicerT exposes a checked `sample` resource. Complete finite native decoding,
  duration-based tempo estimation and candidate slice analysis precede publication.
  Invalid loading retains the previous path, sample and boundaries; empty paths
  clear all three. Structured loading retains the track name. GUI loaders and
  native XML restoration use the same native buffers and validation.
- Five operations expose inspection, onset analysis, boundary edits, read-only MIDI
  preview and insertion into existing native MIDI clips. Generic Instrument defaults
  explicitly reject unsupported slicing. Boundaries have fixed 0/1 anchors, at most
  128 slices, finite strictly ordered values and at least one source frame per slice.
  Inspection reports which slices fit above the base key within MIDI 0–127. Native
  MIDI export rejects inaccessible keys, collapsed integer ticks and timeline
  overflow before changing a target. Insertion may target another song/pattern
  track; replacement validates the entire source and target before clearing notes.
- The original spectral-flux detector remains native. Silence and negative-only
  samples no longer divide by a zero/negative maximum. FFT input/output and prior
  magnitudes use bounded arrays; there is no additional whole-sample mono or
  zero-crossing array. Plans are checked for allocation failure and destroyed on
  every exit; `FFTW_ESTIMATE` avoids planner writes to analysis input, as documented
  in [FFTW's real-data transform API](https://www.fftw.org/fftw3_doc/Real_002ddata-DFTs.html).
  The engine guard excludes audio processing during analysis. It does not establish
  process-wide planner serialization: other native FFT users and hosted plugins
  still require concurrency review against [FFTW's thread-safety requirements](https://www.fftw.org/fftw3_doc/Thread-safety.html).
- Each held voice owns a native Sample/PlaybackState snapshot sharing decoded PCM.
  Its start/end trim prevents resampler input from bleeding into another slice.
  Progress and fade duration use the source/output sample-rate ratio. Zero fade
  never divides by zero. MIDI timing uses source rate, tempo sync and instrument
  pitch, with contiguous rounded boundaries rather than the prior extra overlap
  tick. Native duration, endpoints, resampling and held-voice behavior require DSP
  execution; this record describes source changes, not measured audio correctness.
- Existing version-1 source/data and slicing fields remain, with optional embedded
  `sampleRate` and `snap` persistence and full native float boundary precision.
  Legacy embedded data retains the engine-rate default. Malformed stored points
  fall back to the complete sample with a native diagnostic; empty XML clears
  stale audio. GUI boundary add/delete/drag/reset now call the checked setter and
  preserve endpoints. Parameter changes do not automatically run FFT analysis;
  callers invoke analysis explicitly and the optional GUI provides reset.
- Six QtTest cases were authored for default rejection/schema types, silence and
  failed loading, finite zero-fade playback/endpoints and held-sample replacement,
  source-rate/tempo/pitch MIDI timing, keyboard bounds under master pitch and atomic
  insertion, XML precision/embedded rate, and preset/history/
  recovery. The full-plugin process harness requires SlicerT and measures actual
  native WAV pitch/silence before and after restart, tempo sync, generated MIDI,
  failed/empty loading and preset restore. The source WAV is original diagnostic
  fixture data. These native tests remain unrun. A separate fixture-only check of
  the Python WAV measurement helper passed five known 44.1 kHz pitch/silence cases;
  this verifies the measurement reader, not SlicerT DSP or native rendering.

Source diagnostics pass for 106 changed C++ files, 86 native operations plus four
agent tools, Python syntax and diff whitespace. No compiler, CMake, Qt development
tools or compiled LMMS executable is available on PATH. Slicer analysis is still
synchronous: cancellation/time limits and large-resource supervision need native
implementation and benchmarks. Full plugin rebuilds, historical project/preset
and GUI compatibility, the FFT concurrency audit, hosted-resource coverage,
real inference/listening, native audio verification and every commission completion
gate remain required. The complete LMMS goal remains active.

Native OpulenZ patch configuration now participates in the same source architecture:

- The instrument exposes checked SBI import through the existing `patch` resource
  interface and exposes its native 128 GM patches through existing program tools.
  GM names currently identify numeric table indices. The native table remains
  [AdPlug's FM instrument data](https://raw.githubusercontent.com/adplug/adplug/master/src/mididata.h),
  already used by this LMMS plugin; no replacement synthesizer or patch bank was
  introduced. Both GM selection and SBI import populate the actual native models,
  including levels/envelopes which the previous GM path bypassed. Waveform controls
  expose existing sine/half/absolute/quarter names and Patch exposes all 128 choices.
- The SBI reader stages its complete header and eleven register bytes before any
  model change. It checks all four signature bytes, complete reads and readable
  Qt/Unicode paths; its read is bounded to 52 bytes. Native acceptance of omitted
  reserved tail bytes (47-byte minimum), trailing data and masked OPL2 register
  fields is retained. Latin-1 names stop at NUL within the original 32-byte field.
  Failed loads leave controls, source and track name unchanged. GUI loading shares
  the setter and its prior default-name rule; structured loading preserves naming.
- Patch updates suppress intermediate native model notifications and publish all
  controls under the engine guard, followed by one complete emulator update.
  Program selection is stored separately from the custom patch state: manual
  control changes clear that marker. Optional version-1 XML fields store the
  selector, checked GM identity and import provenance. Native control values remain
  authoritative when stored identity is inconsistent. Restoring a project/preset
  does not reopen an imported SBI file. Resource inspection reports
  `requiredForRestore: false` for such imports; provenance is excluded from external
  asset dependencies. Empty imports restore the constructor's defaults rather than
  the loaded project values which native model reset() may have adopted.
- Emulator ownership and render storage are now RAII; reload/destruction and all
  register writes use the existing shared emulator mutex. Audio periods resize
  the native render buffer as necessary. The prior always-false active-voice
  condition is replaced by a flag check, so level updates reach held notes.
  Velocities start initialized, invalid MIDI keys/velocities fail before indexing,
  invalid controller/pitch-bend data is rejected, and zero-velocity note-on uses
  the native note-off path. Debug messages use Qt diagnostics so they do not enter
  the JSONL response stream. Equal-tempered tuning,
  existing pitch-bend behavior, two-operator synthesis and nine-voice capacity are
  retained. Threading, resampling/reload discontinuities, held-voice changes and
  musical compatibility still require execution and regression evidence.
- Five native QtTest cases were authored for register import/Unicode/choice metadata,
  malformed input and clearing, GM selection and model/preset persistence, live
  emulator levels/MIDI bounds, and history/recovery after removal of the original
  import file. The full-plugin process harness additionally requires OpulenZ and
  measures real WAV pitch/level changes, failure preservation, independent presets,
  GM sound restoration and both custom/GM instances after restart. Recent-resource
  replay must preserve later manual control edits. These native tests remain unrun.
- Three original SBI register assets regenerate identically and pass fixture-only
  header/size/register checks. Five known WAV cases pass independent checks of the
  Python measurement reader's pitch, RMS and silence handling. These checks execute
  no native compiler, emulator, plugin or LMMS renderer.

Source diagnostics cover 106 changed C++ files, 86 native operations plus four
agent tools and changed Python syntax. The toolchain/Qt/native executable remains
absent; no native test or render is running. Full plugin and GUI builds, actual
DSP verification, historical projects/presets, hosted resources, valid GIG sound,
SF2 held-voice lifetime/reload, FFT planner concurrency and cancellation/resource
supervision, real conversational inference/listening and every completion gate
remain required. The complete application commission remains active.

Native remote-host waiting and export failures now have a shared source path:

- RemotePlugin retains its native child/watcher architecture. Startup has a
  30000 ms bound; parent sends and reply waits default to 120000 ms, configurable
  through `LMMS_REMOTE_PLUGIN_TIMEOUT_MS` within 1000..600000. One audio-processing
  callback shares its deadline across send/reply, and nested operations preserve
  the earliest active deadline. The watcher checks monotonic time every 25 ms,
  invalidates failed communication and requests native child termination. Idle
  hosts do not have an active request deadline. This follows the existing
  [Qt process lifecycle and channel APIs](https://doc.qt.io/archives/qt-5.15/qprocess.html);
  timing and actual process termination remain unverified.
- Shared-memory invalidation is atomic. FIFO recursion now distinguishes owning
  threads through a local recursive mutex; bounded semaphore attempts observe
  invalidation rather than blocking forever on an abandoned lock. Windows uses
  [finite WaitForSingleObject calls](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject);
  portable POSIX waits use [sem_trywait](https://man7.org/linux/man-pages/man3/sem_wait.3.html)
  with monotonic polling. FIFO backpressure releases the writer's lock and permits
  the reader to drain capacity. Complete serialized packets preserve the native
  message wire layout and embedded NUL fields. Negative/oversized field counts,
  lengths, aggregate sizes and invalid shared pointers fail before copying.
- Unix socket invalidation uses
  [shutdown](https://man7.org/linux/man-pages/man2/shutdown.2.html) to wake I/O.
  Interrupted reads/writes retry; writes suppress SIGPIPE where supported.
  Socket descriptors/validity, watcher stop/running flags and main-thread wait
  depth use atomics. Connection reset closes old sockets. Watcher exit no longer
  drains a possibly abandoned FIFO, and teardown retires failed children too.
  Explicit retirement is marked before IdQuit to distinguish it from crashes.
  Restart clears old channels and audio storage before establishing a fresh host.
- Messages retain the FIFO's native 512 KiB capacity; Unix socket messages and
  shared audio storage each have a 64 MiB bound. Channel counts are checked before
  size arithmetic/allocation. Host output is copied only after a matching native
  processing completion. Failure zeros the caller's output and returns false;
  subsequent attempts with that failed host still notify the renderer. Child
  stdout diagnostics are captured and written to stderr, while child stderr is
  forwarded, keeping these child diagnostics out of the studio JSON stream.
- ProjectRenderer captures a native host failure counter before device setup,
  observes it during processing and refuses finalization after a host failure.
  The counter covers the whole process, so a simultaneous failure from another
  remote host conservatively fails the export too. Studio does not publish its
  temporary output. RenderManager now finalizes codecs before continuing stems,
  reports failed creation/encoding/hosting, stops remaining stems, restores mute
  state and closes the file device before removing partial output. Earlier
  successful stems remain. CLI completion now returns a failure exit status and
  deletes its manager; the optional GUI reports the same export error. Legacy
  destination writes retain their prior direct-write behavior.
- Twelve native QtTest cases were authored and registered with a 90-second CTest
  limit. They use the actual parent/child IPC code, with fixtures for nine
  processing failures (crash, hang, malformed count/length variants, partial
  payload, reply flood and bad channels), stalled initialization, abandoned data
  locks, FIFO capacity/compaction, binary fields, healthy processing and restart,
  semaphore counts/time bounds, output acquisition and host export failures,
  removal of partial output, and child diagnostics. All native cases are unrun.
  The helpers are fault instrumentation, not replacement hosts or plugin-matrix
  evidence. Actual CLI/GUI regressions and native VST/Zyn audio remain required.

Source diagnostics cover 116 changed C++ files, 86 native operations plus four
agent tools, Python syntax, native fault-test build registration and diff
whitespace. No CMake/compiler/Qt tools or native LMMS executable is available for
execution; no native build, IPC test or render has been run. Per-message packet
allocation, polling/watchdog CPU, peak heap, platform-specific shutdown and host
compatibility need native measurement. Immediate render cancellation still waits
for cooperative unwinding, and in-process DSP cannot be forcibly interrupted by
this host watcher. Full resource supervision, automatic plugin recovery, the
remaining instrument issues, real inference/listening and all commission gates
remain required. The complete LMMS goal remains active.

Native render cancellation and export ownership now have additional source paths:

- ProjectRenderer reserves a monotonically identified export context before
  stopping/changing the audio device. A monotonic render deadline and explicit
  cancellation are available to the native host watcher without main-thread event
  processing. Active RPCs inherit the context; a hardware-period RPC that began
  before export setup can adopt it. Process startup and Unix connection waits
  poll cancellation between bounded attempts. Interrupted requests invalidate
  transport and request child termination instead of waiting for the configured
  host timeout. Restart still requires fresh native host initialization.
- Context IDs and deadline snapshots are synchronized, and cancelling a retired
  ID cannot cancel a later export. Worker completion stops assigning new RPCs to
  render execution, while retaining the export reservation for finalization and
  cleanup. This permits healthy hosts to receive restoration messages after
  cancellation. Existing outstanding requests retain their original context.
  Idle hosts without requests are not targeted by the watcher. The shared manager
  and studio also check an outstanding audio-device backup before opening another
  destination, covering preflight failure and device restoration. Same-manager
  reentry is rejected while it owns the device or renderer. These guards retain
  the native single Song/AudioEngine model, not concurrent independent projects.
- The renderer now reports whether startup succeeded. It checks cancellation
  around native periods, skips writing a cancelled period and refuses cancelled
  finalization. Its destructor waits for an active worker and retires its context.
  A codec opened before device setup has explicit ownership until the engine
  installs it; cancellation before installation still closes the descriptor.
  Setup exceptions preserve the engine's ownership when installation already
  happened. Studio starts timers before setup and does not enter a completion
  loop for a worker that never started. Remaining-budget checks also cover
  cooperative finalization and temporary-file publication before commit.
- RenderManager records its startup phase. The optional export dialog defers
  rejection/destruction while device setup is inside a nested RPC event loop,
  then closes after that call unwinds. Normal completion keeps the renderer's
  reservation through file finalization and device restoration. Cancelled export
  reports cancellation and removes partial legacy output; studio preserves its
  existing destination through temporary output and atomic publication. Thread
  ownership and waiting follow the source application's QThread architecture;
  [Qt's thread lifecycle API](https://doc.qt.io/qt-6/qthread.html) is a reference,
  not evidence of an executed Qt 5 or Qt 6 application.
- Ten additional native QtTest cases bring RemotePluginTest to twenty-two cases:
  interruption of an active processing wait while preserving an idle host, stale
  cancellation versus a later context, a deadline without main-thread events,
  adoption of an already pending RPC, cancellation during device setup and host
  startup, closure of an uninstalled encoder, manager retention during setup,
  reservation after worker completion, destination rejection before opening, and
  studio destination preservation plus fresh-host initialization/native WAV
  rerender. The last two cases have two data rows each; processing failures retain
  nine rows. Cleanup checks both native context and device-backup release. The
  90-second CTest limit remains. All native cases are unrun. Fixtures use actual
  LMMS IPC and native test arrangements; they do not establish hosted-plugin DSP,
  production musical quality or optional GUI compatibility.

Source diagnostics cover 118 changed C++ files, 86 native operations plus four
agent tools, Python syntax, test registration, schema inventory and whitespace.
No CMake/compiler/Qt tools or native LMMS executable is available, and the pinned
submodules remain uninitialized. No native compilation, IPC test, encoder or
render has run. Cancellation latency, watcher polling CPU/heap, host compatibility,
thread races and GUI destruction/restoration need actual execution. In-process
DSP, codecs, disk I/O and restoration remain cooperative; a blocked diagnostics
sink can delay the watcher. There is no whole-application hard termination or
automatic plugin/project recovery guarantee. Full builds, native regressions,
remaining instrument issues, real inference/listening, resource supervision and
every completion gate remain required. The complete application goal stays active.

SF2 native voice lifetime and layered expression have further source changes:

- Note data now records the synth generation before caching FluidSynth voices.
  Rebuilding the native synth or explicitly clearing the bank retires the old
  generation, resets note counters and discards queued events. A surviving held
  note clears old pointers without dereferencing them before starting in the new
  generation. Deletion of an older note cannot decrement current counters or
  send a MIDI note-off into a new same-key note. Native note-off and plugin-data
  deletion are idempotent. Voice-pool access, note queues, counters, rendering,
  gain/effect changes and synth replacement share a recursive mutex, in addition
  to the engine's model-change guard for replacement and tuning.
- The native voice list follows configured polyphony, replacing the four-voice
  container. Initial detuning is applied immediately after note-on, and live
  detuning and panning update every captured voice whose ID remains valid in the
  current pool. This follows the distinction between pooled voice IDs and synth
  lifetime in [FluidSynth's voice API](https://www.fluidsynth.org/api/group__voices.html)
  and the synthesis-thread restriction in its
  [voice control API](https://www.fluidsynth.org/api/group__voice__control.html).
  Those contracts inform the source design; race freedom and DSP behavior need
  actual native execution. Per-note storage still allocates and locking costs
  remain unmeasured.
- The optional SF2 patch dialog no longer retains a raw synth pointer while its
  event loop runs. It browses a value snapshot, closes when the instrument or
  bank changes, and previews through a checked native API. Accept requires a
  selected visible program and commits its bank/program controls. Cancel restores
  their current selection, including changes made elsewhere. This removes the
  former native-pointer lifetime dependency without requiring an editor in
  studio mode. GUI behavior on Qt 5 and Qt 6 remains unrun.
- Three native QtTest cases cover held-note synth rebuild after deleting the
  source file, stale note deletion beside a fresh same-key note, and six-layer
  initial/live detuning plus stereo panning. Stale deletion has two rows covering
  rebuild and explicit clear/reload. Assertions measure actual native PCM when
  executed. The native process harness also checks six-layer exported pitch and
  left/right attenuation before restoring the original bank. All native cases
  and process renders remain unrun.

Fixture-only checks passed for byte-identical regeneration, RIFF bounds, Hydra
record sizes, the original 440 Hz source PCM and six shared-sample zones in the
new test bank. The original calibration SoundFont remains unchanged. Separate
known-PCM checks validate the WAV reader's stereo frequency/RMS measurements and
silence rejection; they do not invoke LMMS or FluidSynth. These calibration assets
are test instrumentation, not production instruments or song templates.

Source diagnostics cover 120 changed C++ files, 86 native operations plus four
agent tools, Python syntax, schema inventory, build registration and whitespace.
No native compiler, Qt test, bank loader, DSP or render has run. At this stage,
synth recreation still deleted the old synth before allocating its replacement;
the following update addresses replacement preparation, while detached-bank
ownership failures still lack a complete transaction/recovery path. Settings
remain restricted to one native synth at a time, as documented in
the [minimum supported FluidSynth 2.2 source](https://github.com/FluidSynth/fluidsynth/blob/v2.2.0/src/synth/fluid_synth.c).
Full bank/plugin corpus compatibility, sanitizers, contention/heap measurement,
GUI regression, valid GIG playback, full builds, real inference/listening and all
commission gates remain required. The complete application goal stays active.

SF2 replacement preparation now keeps the working native synth through allocation:

- `Sf2SynthState` owns an unpublished synth and independent settings, with synth
  destruction preceding settings destruction. It copies existing numeric,
  integer and string values through native APIs, including long and nullable
  strings. Copied strings use `fluid_free`, preserving the native DLL allocator
  boundary. Move assignment is disabled so replacing an owned pair cannot free
  settings before its synth. Independent settings avoid the callback invalidation
  caused by sharing one settings object between live synths. See the
  [native settings API](https://www.fluidsynth.org/api/group__settings.html) and
  [allocator contract](https://www.fluidsynth.org/api/group__misc.html).
- Reload prepares the settings copy, native synth and polyphony-sized scratch
  storage before detaching the bank, retiring note generations or deleting the
  working synth. Native allocation/setup failures return without retiring its
  pool, bank, counters or queued notes. Scratch `bad_alloc` is caught before
  mutation. Bank detachment is checked and a failed detachment restores program
  selection. Replacement preparation temporarily retains two native synths and
  settings objects; the loaded sample bank is transferred, not duplicated.
  Peak memory and preflight duration need actual measurement.
- The existing libsamplerate conversion is synchronized with the engine output
  clock even when the old synth is retained after preflight failure. A successful
  replacement updates its native rate and conversion state. Native versions that
  reject an engine rate retain their existing configured native rate and use
  LMMS's original linear resampling pipeline. Actual engine-level failure and
  sample-rate-transition behavior remain unverified.
- Four additional QtTest cases have eight executions: typed/long/nullable setting
  preservation; settings-allocation, synth-allocation and invalid-rate failures
  beside a real native held voice after removal of its source file; callback
  independence after prior-synth destruction; and real bank PCM at 44100, 48000
  and the native rate selected for a 192000 request. The two allocator hooks only
  inject failed preparation, while all bank parsing and DSP use FluidSynth.
  They do not simulate musical output or prove real operating-system memory
  exhaustion. CMake registers this suite when FluidSynth is available, links its
  native library and sets a 60-second test timeout. All cases remain unrun, as do
  the existing full Instrument/StudioRuntime reload and export regressions.

Source diagnostics now cover 122 changed C++ files, the unchanged 86 native
operations plus four agent tools, Python syntax, schema inventory, test/plugin
build registration and whitespace. No native compiler or Qt tools is available,
and there is no native LMMS executable to run. No new bank loader, DSP, Qt test or
render execution is claimed. These changes addressed preparation failure; at
this stage bank transfer still occurred after old-synth destruction and lacked
complete rollback/ownership recovery. The following update replaces that path.
Native fault unwinding, actual
resampled PCM, sanitizers, full bank/plugin compatibility, CPU/heap evidence,
complete builds, real inference/listening and every commission gate remain
required. The complete LMMS application goal stays active.

SF2 reload now prepares its native bank before retiring the working synth:

- Loading captures the source into a private temporary directory through a
  64 KiB buffer, checking complete reads/writes, size and flush. The native loader
  accepts that captured file; production PCM remains FluidSynth's output. The
  copy is not held in a second whole-file byte array. Original path/name handling
  remains in project metadata. A regular QFile closes its descriptor after
  capture, avoiding the internally retained descriptor of QTemporaryFile's
  `close()`. [Qt's temporary-directory lifecycle](https://doc.qt.io/qt-6/qtemporarydir.html)
  governs normal cleanup. Streaming capture does not promise an atomic snapshot
  against external concurrent writers.
- Replacement loads a separate native bank from the accepted snapshot and checks
  selection of the current bank/program before incrementing note generation,
  clearing queues/counters or deleting the working synth. Preparation failure
  therefore does not detach its bank. Each native bank stays owned by its synth;
  production no longer uses remove_sfont/add_sfont for transfer. The new path
  avoids the caller-ownership and preset-reference problem described by
  [FluidSynth's SoundFont API](https://www.fluidsynth.org/api/group__soundfont__management.html).
  A live reload can use the captured input after the original path is removed or
  replaced, while each load still runs the native decoder.
- Rejected and replaced bank snapshots retain their native IDs until they can
  be reclaimed. Control operations prune only exclusively owned snapshots whose
  bank is absent from the synth and whose synthesis has no active voices. Failed
  filesystem removal remains eligible for a later retry. Native-synth destruction
  precedes clearing its remaining retired files; instrument destruction closes
  the current bank before the current snapshot. Cleanup stays outside play().
  These lifetime rules, lazy sample loading and Windows mappings need actual
  native execution. Retired files can remain until a later control operation or
  synth destruction, and abrupt process exit can leave temporary directories.
- Three added QtTest cases bring Sf2SynthStateTest to seven cases and fourteen
  executions. They check capture beyond one copy chunk, source replacement/removal
  and normal cleanup; native rejection of a missing file, invalid bank bytes,
  missing bank and missing program beside a still-playing original voice; and
  actual octave PCM from a prepared bank after prior-synth destruction and original
  source removal. The old and replacement banks must be distinct native objects.
  All cases remain unrun. Existing full Instrument/StudioRuntime tests continue to
  cover held-note reload and export behavior when a native build is available.

Source diagnostics cover 123 changed C++ files and verify test/plugin registration,
source removal of native bank detachment, the unchanged 86-operation inventory,
Python syntax and whitespace. This is not compilation or execution evidence.
No native compiler, Qt tests, bank load or render has run. Replacement preparation
now also holds a candidate native bank and may increase heap and decode work;
snapshot storage adds disk I/O and retention. Actual peak heap/disk/descriptors,
latency, cache failures, sanitizers, lazy/compressed banks and the complete native
compatibility corpus remain required. These caches preserve live loaded input;
they are not a persistent project asset bundle. Original external paths remain
required for later-process restoration, and full durable asset management and
crash cleanup still need work. Native resource supervision, real inference/audio
evaluation, complete builds and every commission gate remain outstanding. The
complete LMMS application goal stays active.

Native bundle saving now supports accepted SF2 input and fixes resource copying:

- DataFile has a resource-save scope and per-node accepted-source bindings.
  Registration requires the active document, the same owner document and a
  whitelisted media attribute. Nested and disabled scopes restore the previous
  thread-local context. This uses native DOM node identity, rather than treating
  equal path strings as equal loaded state. Two SF2 instruments can therefore
  preserve different accepted versions of the same original filename. See
  [Qt DOM identity and sharing](https://doc.qt.io/qt-6/qdomnode.html).
- Song resource-enabled serialization activates the scope. SF2 registers its
  accepted private snapshot while retaining its original XML path/name until the
  native bundle copier runs. An explicit bundle can thus retain the accepted bank
  after original-file replacement or removal. Plain project saves keep existing
  references. Song saving state and deterministic hash-seed reset now have scoped
  cleanup when serialization exits, including exceptions. GUI behavior remains
  unrun, as does the exported native interface on all supported platforms.
- The native copier skips empty paths, reuses repeated canonical source paths,
  and avoids collision chains against both selected and existing filenames.
  Extensionless names use their own basename instead of deriving an extension
  from a parent directory. A loaded DataFile resolves local references using its
  own origin, including when an unrelated Song is open. XML rebasing is deferred
  until copying succeeds. Physical copied files can still remain after a later
  failure; this change does not provide atomic publication of a whole bundle.
- Native LFO user-wave files join the existing media resource table for bundle
  copying, local-path validation and checkpoint path resolution. Unrelated plugin
  attributes remain outside that whitelist. Project.save already returned the
  native file inside a bundle directory; that handling was inspected and retained.
- Six added QtTest cases have seven executions: empty/shared/LFO resources;
  chained and extensionless names; failed-copy DOM preservation; standalone origin
  resolution beside another open project; distinct accepted versions and scoped
  document ownership; and explicit SF2 bundle loading after original removal or
  replacement, checked by copied bytes, program/name and real native octave PCM.
  The process harness also saves accepted bank bytes and renders while its original
  file has been replaced, restoring the fixture afterward. All native cases and
  process execution remain unrun. The authored byte-copy fixtures do not replace
  any native decoder, synth or application runtime.

Source diagnostics passed for 123 changed C++ files, the unchanged 86 native
operations, four agent tools, Python syntax, registration/inventory and whitespace.
No native compilation, Qt case, bundle writer, decoder or render has executed.
These changes advance explicit native bundle preservation. Studio checkpoints
still contain path references to external assets; they do not automatically own
accepted source bytes. A shared durable asset store, per-checkpoint integrity
records, quotas, failure cleanup, accepted-input adapters for all relevant plugins,
existing project/bundle corpus tests and resource measurements remain required.
Full native builds, real conversational/audio evaluation and every commission
gate remain outstanding. The complete LMMS application goal stays active.

Native checkpoints now own declared media resources in a shared asset store:

- StudioAssetStore stages source bytes through a 64 KiB buffer, hashes the exact
  copied stream, checks size/read/write/flush and publishes a same-directory
  temporary file under its SHA-256 and normalized extension. An existing file is
  reused only when its size and hash match. Published content is checked again
  before recording its reference. Altered existing files are rejected rather than
  overwritten. The implementation follows
  [Qt's temporary-file publication API](https://doc.qt.io/qt-6/qtemporaryfile.html)
  and [hashing API](https://doc.qt.io/qt-6/qcryptographichash.html); those documents
  are contracts, not native execution evidence.
- Song checkpoint serialization enables native accepted-source bindings, copies
  known media to a shared `checkpoints/assets` directory, and writes normal MMP
  documents with `local:assets` references. SF2 uses its accepted snapshot. The
  DataFile resource table covers sample clips, AFP, SlicerT, SF2, GIG, Patman,
  TripleOscillator and LFO wave files. Other resources still require adapters;
  for declared resources without an accepted snapshot, the current source file
  is copied and may differ from a previously decoded in-memory version.
- Each new history entry has an assets array of relative paths, hashes and exact
  byte sizes. Validation checks path/hash structure, duplicate references,
  numeric bounds, owned directory/file identity, file content and exact coverage
  of declared resource paths in the native document. Recovery validates before
  loading; native mutation validates its rollback checkpoint before execution.
  Undo/redo distinguish preflight rejection from a native load that may already
  have changed objects, avoiding an unnecessary rollback on bad asset metadata.
  Older two-field checkpoint entries remain readable with their original external
  dependencies. Native binary interface changes require a full rebuild.
- Three unit cases cover byte reuse across sources, same-size corruption and
  preservation of altered evidence; unsafe/mismatched/duplicate references and
  invalid sizes; and complete native-document reference coverage. Two integration
  cases cover SF2 source removal, undo/redo, further edits, process-owned object
  restart, session-directory relocation and actual octave PCM; and corrupt sample
  assets rejected before edit/recovery changes the current Song. These five native
  cases are authored but unrun. The process harness now compares owned-resource
  bytes/hashes and all remaining media properties after rollback/restart instead
  of requiring original filenames, which legitimately change on rebasing.

Source diagnostics cover 126 changed C++ files and the unchanged 86 native
operations plus four agent tools. Source registration, Python syntax, inventories
and whitespace are checked; no native compilation, asset operation, Qt test,
recovery process or render has executed. Checkpoints and orphan assets are not
garbage-collected, and quotas remain unfinished. Asset hashing/staging and
verification are synchronous and can be expensive with large banks; cancellation,
peak disk/heap/descriptors, hard-kill/power-loss boundaries, filesystem races and
native compatibility need actual measurement and further implementation. Full
accepted-input coverage, remaining application features, real inference/listening
and every commission gate remain required. The complete LMMS goal stays active.

Accepted audio inputs now follow the native sample-buffer lifetime in source:

- FileResourceSnapshot moves the prior SoundFont capture implementation into the
  native core. It streams through 64 KiB storage into a unique owned temporary
  directory, checks complete I/O and finalization, preserves usable extensions,
  and closes the actual descriptor before the native loader runs. SF2's existing
  current/retired snapshot leases retain their native bank lifetime handling.
  The shared implementation follows [Qt file I/O](https://doc.qt.io/qt-6/qfile.html)
  and [temporary-directory ownership](https://doc.qt.io/qt-6/qtemporarydir.html).
- SampleBuffer.fromFile now passes that captured file to the existing native
  SampleDecoder and retains it alongside the immutable decoded PCM. Original
  paths remain metadata. Buffer copies and swaps retain the matching snapshot.
  Failure to capture does not fall back to decoding an uncaptured original.
  Embedded/generated native buffers retain their existing serialization.
- Sample clips, AudioFileProcessor, SlicerT, TripleOscillator user waves and LFO
  user waves bind each buffer's accepted source to its exact DOM resource node.
  Native bundles and shared checkpoints can preserve different loaded versions
  of one filename after original replacement or removal. Normal project saving
  keeps existing references. GIG, Patman and other native/hosted resources still
  require accepted-input adapters and compatibility evidence.
- A held audio voice can release the last buffer reference. Snapshots have
  application-thread affinity, and last releases from another thread use
  [Qt deferred deletion](https://doc.qt.io/qt-6/qobject.html#deleteLater), moving
  temporary-file cleanup out of the releasing worker while the event loop runs.
  This does not prove hard realtime bounds: posting still uses Qt synchronization
  and allocation. Deferred cleanup after the event loop stops and crash cleanup
  remain unfinished, as do all native lifecycle/performance measurements.
- Six added native cases have sixteen executions: multi-chunk exact-byte copy and
  last-owner lifetime; worker release deferred until application-thread cleanup;
  missing/directory sources; native decoded buffer ownership/copy/swap with two
  versions of one filename; ten bundle rows spanning all five consumers and
  removed/replaced originals, native reloading and subsequent accepted-input
  serialization; and two shared-checkpoint rows with both loaded versions before
  the first media checkpoint, byte/hash/node checks, deduplication, undo/redo,
  object restart, session relocation and recovered sample/LFO values. They are
  registered but unrun. Optional plugin skips are not full application proof.
- The process harness additionally bundles sample clips, AFP, TripleOscillator
  and LFO input bytes while originals are removed/replaced and checks a real
  native render against the previous output. Fixture originals are restored in
  a finally block. A rollback assertion now checks owned bytes/hash rather than
  expecting the original filename. Process execution remains unrun.

Source diagnostics passed across 131 changed C++ files, the unchanged 86 native
operations plus four agent tools, Python syntax, build/test registration and
whitespace. Six fixture-only checks exercise the harness's path/hash/metadata
assertions without launching LMMS. No native compilation, decoder, Qt case,
bank load, bundle/checkpoint writer, recovery process or render has executed.
Capture adds a temporary copy per loaded input and synchronous disk work; quotas,
cancellation, repeated-load costs, complete publication/cleanup, filesystem races,
full application compatibility, real inference/listening and every commissioning
gate remain required. The complete LMMS application goal stays active.

Native Patman accepted-input persistence and snapshot shutdown cleanup now have
source implementations:

- Patman captures the original GUS file before its existing checked decoder runs.
  Only a complete decoded candidate publishes both Sample objects and its matching
  source snapshot under the engine model guard. A failed capture or decode retains
  the previous samples, source and provenance. Old snapshot release occurs after
  the publication guard. Clearing/unloading releases the snapshot; held notes
  retain their existing decoded Sample objects and require no lazy file reads.
  Bundle/checkpoint serialization binds the accepted snapshot to its native
  `patman.src` node while retaining original path metadata for plain saves.
- Two new Qt integration cases have four rows for removed/replaced originals.
  Two instruments load sounding and silenced versions of the same filename before
  persistence. Tests check distinct copied bytes, retained state after a failed
  reload, track names and exact native PCM after bundle loading. Checkpoint rows
  additionally cover hash/node references, two shared owned files, undo/redo,
  further revisions, session relocation, recovery and exact native PCM. The older
  Patman recovery test now checks owned file location and bytes for rebased
  resources rather than requiring original basenames; ordinary preset restoration
  retains its original-reference assertion. These native cases remain unrun.
- The process harness saves native Patman bundles and renders its actual current
  instrument with originals replaced/removed, verifies copied accepted bytes and
  compares the known native pitch/peak/tail. It restores test originals in finally
  blocks. The full process test remains unrun. A fixture-only check verifies that
  the two authored 88,631-byte patch versions differ only in first-sample PCM:
  signed silence replaces a peak-16,000 waveform, with both headers and the second
  sample unchanged. Hashes differ despite identical file sizes. This check does
  not demonstrate native decoding or synthesis.
- FileResourceSnapshot now keeps worker-released objects in an intrusive queue
  protected by a short mutex. Qt deferred deletion still handles ordinary runtime
  cleanup. Queue publication and event posting share one lock, and destruction
  unlinks before file cleanup begins. Engine.destroy drains the released queue
  after stopping/deleting the native audio engine, without relying on the event
  loop to restart. Active shared owners never enter the released queue. Direct
  deletion cancels the object's pending Qt event under the
  [QObject lifetime contract](https://doc.qt.io/qt-6/qobject.html#dtor.QObject).
  Qt's [deferred deletion contract](https://doc.qt.io/qt-6/qobject.html#deleteLater)
  explains why relying only on deleteLater after event-loop exit is insufficient.
- Two additional unit cases exercise ordinary deletion from the middle of the
  queue followed by a shutdown drain, correct destruction thread, active-owner
  preservation, repeated drain/posted-event cleanup and worker-thread drain
  rejection. The snapshot unit now has five registered cases, all unrun. Queue
  mutex/posting bounds, real engine shutdown and any workers releasing resources
  after the shutdown drain still require native lifecycle/performance evidence.

At this earlier stage, GIG dependency handling still needed a group implementation. The official
[libgig extension-file description](https://doc.linuxsampler.org/Release_Notes/LinuxSampler_2_1_0/)
documents adjacent `.gx01`/`.gx02` companion files. Current LMMS GigInstance opens
its native RIFF/gig objects on the original bank and streams samples during DSP;
DataFile/StudioAssetStore currently preserve only the declared root resource.
The missing group must preserve companion naming, all accepted input bytes and
per-file integrity through bundles, checkpoints and relocation. Applying the
single-file snapshot to this loader would disconnect companions. The native GIG
loader was retained; root-only persistence is not complete bank compatibility or
durable recovery evidence. Valid single/multi-file GIG fixtures and playback are
still required, along with accepted-input adapters for other native/hosted resources.
The later GIG group section records the subsequent source implementation and
its outstanding native/extended-format verification.

Source diagnostics passed for 132 changed C++ files, the unchanged 86 operations
plus four agent tools, Python syntax, build/test registration and whitespace.
The new patch-fixture semantic check and six fixture-only harness path/hash checks
passed. No native C++ compilation, Qt case, loader, DSP, bundle/checkpoint operation,
recovery/shutdown process or render has executed. Private captured files do not
yet carry a retained capture digest; corruption before serialization and filesystem
races remain unverified. Quotas, cancellation, publication/crash cleanup, full
application/project/plugin compatibility, real reasoning/listening, resource
measurements and every commissioning gate remain outstanding. The complete LMMS
application objective stays active.

Captured native inputs now retain a fingerprint from before native loading:

- FileResourceSnapshot hashes the exact 64 KiB capture stream and retains its
  SHA-256 and byte size. The completed private file is verified before the native
  caller receives it. Verification of a later file/copy checks regular-file
  identity, size, successful complete hashing and equality with that original
  fingerprint. Altering or deleting the private file does not update the retained
  fingerprint. The implementation uses the
  [Qt hashing contract](https://doc.qt.io/qt-6/qcryptographichash.html).
- DataFile resource bindings now have a typed snapshot variant. SampleBuffer
  (sample clips, AFP, SlicerT, TripleOscillator and LFO waves), SF2 and Patman use
  this variant. The native document keeps a shared lease through copying, retaining
  both the captured file lifetime and its expected fingerprint if its original
  instrument/buffer owner is released. Existing path-only bindings remain available
  for callers without captured-input metadata and do not acquire this guarantee.
- StudioAssetStore compares input size and the checksum of the actual staged
  stream with the expected captured input before publication/reference creation.
  An altered same-size stream is rejected even when an older correct asset exists.
  A bundle verifies its newly copied file before rebasing XML and attempts to
  remove that newly created file on verification failure. Reused canonical source
  paths also verify the saved copy, so a prior untyped copy cannot bypass a later
  typed binding. DOM updates and returned checkpoint references remain deferred
  until all copies succeed. Other files from a failed bundle/checkpoint operation
  can still remain as orphans; complete publication/cleanup is unfinished.
- SF2 checks its current captured bank before allocating a replacement synth.
  Fingerprint failure retains the current native pool and uses the existing
  resampling conversion for the new engine clock. Actual reload behavior, lazy
  native readers under external filesystem corruption and held-voice audio remain
  unverified. This adds synchronous hashing during bank rebuilds and needs native
  resource/cancellation measurements.
- Five added native cases have eleven executions. Snapshot fingerprint tests
  cover original replacement, unchanged valid copies, same-size private changes,
  truncation, removal and restoration of the original bytes. Three store rows
  reject changed/truncated/grown copied streams before new publication and preserve
  an older correct asset. Four document rows cover typed lease lifetime and
  valid/corrupt input in bundle/checkpoint modes. Two reused-path rows verify that
  an untyped first copy cannot bypass the fingerprint on the following typed node.
  A native sample integration case corrupts a private valid WAV while keeping its
  decoded PCM, rejects bundle saving and a mutation's checkpoint, restores the
  prior complete native checkpoint and accepts a subsequent edit. Musical history
  stays unchanged on rollback; revision advances because native object/model
  handles were replaced. All these native cases are authored but unrun.

Source diagnostics passed across 132 changed C++ files, the unchanged 86 native
operations plus four agent tools, Python syntax, registration/inventories and
whitespace. Snapshot and asset-store units now contain six cases each, with six
and twelve executions respectively. No native compilation, hash/copy operation,
Qt case, decoder, synth reload, bundle/checkpoint writer, recovery or render has
executed in this environment. Fingerprint checks are synchronous and filesystem
races remain unverified. GIG companion dependency groups, other native/hosted
resource adapters, quotas/cancellation, publication/crash cleanup, full project
and plugin compatibility, actual reasoning/listening and every commission gate
remain required. The complete LMMS objective stays active.

Native asset copying and hashing now share cooperative execution checks in source:

- FileResourceIO owns thread-local, nested operation scopes and fixed 64 KiB
  transfer/hash loops. It checks inherited cancellation before work, between
  chunks and at completion, and rejects incomplete reads/writes or changed input
  sizes. Its file copier creates an exclusive new destination, preserves native
  file permissions, closes output and removes its own partial file on failure;
  an existing destination is retained. GUI callers without a scope receive no
  added job policy. Scope state is not automatically inherited by worker threads.
- Snapshot capture/fingerprint checks, bundle copying, checkpoint asset staging/
  verification and runtime checkpoint/archive digests use the shared loops.
  StudioRuntime binds its existing stopping/job-status/wall-time callback to the
  whole request, including asset validation and checkpoint creation. StudioAudio
  adds the render deadline during startup-related file work. No native capability
  or agent tool was removed or replaced; their inventory remains 86 plus four.
- Scoped application-thread polling is throttled to 10 ms and excludes desktop
  input events while keeping timer/socket processing available under
  [Qt's event flags](https://doc.qt.io/qt-6/qeventloop.html). Recursive polling is
  suppressed. The existing StudioRuntime busy guard permits only session/job
  inspection, cancellation and shutdown during an executing request, and existing
  nested-request merging preserves those newer control records. This is source
  behavior, not native responsiveness or reentrancy proof. Qt's
  [event-processing contract](https://doc.qt.io/qt-6/qcoreapplication.html#processEvents)
  also documents callback timing and deferred-deletion limits; a short requested
  polling interval is not a hard wall-time bound. Worker snapshot cleanup can
  remain queued until ordinary event-loop dispatch or the explicit shutdown drain.
- Mandatory native rollback uses a separate recovery helper with I/O cancellation
  suspended. Native audio-device restoration does the same. A cancelled request
  stops new transfers without poisoning restoration of the previous checkpoint
  or output clock. Undo/redo's requested restore remains cancellable; rollback
  after an attempted load is mandatory recovery. Actual lifecycle effects and
  resource/cancellation bounds remain unverified.
- Six new registered unit cases cover exact multi-chunk hash/copy, preservation of
  an existing destination, interruption after partial copying with source retained,
  cancelled snapshot/staging with no published reference, inherited stops and
  recovery suspension, event-delivered cancellation with recursive polling
  suppressed, and one-chunk stream interruption/empty unacknowledged hash output.
  A native integration case latches cancellation after a tempo edit, rejects its
  checkpoint, restores the prior native project and PCM from owned assets after
  original removal, preserves history, invalidates replaced model handles through
  the revision and accepts the next edit. These seven native cases are authored
  but unrun; no native I/O, Qt callback, checkpoint or rollback ran here.

Source diagnostics passed across 135 changed C++ files, Python syntax, unchanged
operation/schema inventory, build/test registration and whitespace. Six fixture-
only harness path/hash assertions passed. No native toolchain or LMMS executable
is available, and no C++ compilation, loader, DSP, render or recovery/shutdown
process has executed. Synchronous OS calls, native decoder/plugin execution and
XML parsing remain outside these between-chunk checks. Disk/memory quotas,
cancelled-operation orphan cleanup, measured CPU/heap/I/O/descriptors, hard-kill/
power-loss behavior, GIG companion groups, complete plugin/project coverage, real
reasoning/listening and every commissioning gate remain outstanding. The complete
LMMS transformation objective stays active.

Native sample decoding now advances resource handling and Vorbis fallback
correctness in source:

- Libsndfile decoding reads/converts 4,096-frame blocks with FileResourceIO checks
  around allocation and between reads. A full-file interleaved scratch allocation
  is replaced by one block per input channel, while the native stereo output
  buffer remains complete. Mono duplication and the existing first-two-channel
  selection are retained. Every decoded channel is checked for finite values,
  including unused later channels and the last short block. Incomplete reads,
  codec errors and close errors reject the candidate. The native
  [libsndfile frame-read API](https://libsndfile.github.io/libsndfile/api.html)
  remains the decoder.
- The Vorbis fallback previously copied only one channel's planar floats and then
  interpreted them as interleaved samples, reducing stereo frame count and losing
  the right channel. It now consumes the native planes directly, reads at most
  4,096 frames per call, preserves mono/stereo channel behavior, validates finite
  output and complete declared frame totals, and always clears initialized decoder
  state on exit. This follows
  [Vorbis float decoding](https://xiph.org/vorbis/doc/vorbisfile/ov_read_float.html)
  and [PCM totals](https://xiph.org/vorbis/doc/vorbisfile/ov_pcm_total.html).
- Qt callbacks guard size multiplication, return complete read items, report
  seek failures and signed-offset overflow, and reject positions not representable
  by the callback's native long. QFile retains descriptor ownership through a null
  close callback under the
  [Vorbis callback contract](https://xiph.org/vorbis/doc/vorbisfile/ov_callbacks.html).
  OGG is advertised when the native fallback exists, independently of libsndfile's
  format discovery. Cancellation stops further backend attempts; root decoding
  catches allocation/length failures and returns no partial result. DrumSynth's
  existing generator remains authoritative, with cancellation checks before/after
  generation and during PCM conversion.
- The internal fallback entry point is callable by native compatibility tests so
  libsndfile's earlier success cannot mask a broken Vorbis fallback. Four new cases
  have seven executions: mono/stereo/six-channel multi-block exact float decoding;
  late nonfinite unused-channel rejection, interrupted decoding and a subsequent
  successful decode; mono/stereo Vorbis frame count/channel accuracy from actual
  native encoder fixtures; and corrupted/interrupted fallback decoding with
  subsequent success and file release. Lossy channel comparison uses an error
  tolerance; a disabled Vorbis build explicitly skips its rows and cannot prove
  full compatibility. All native cases remain unrun.

The native SampleBuffer stores one source sample rate. The fallback now rejects
mixed-rate chained Vorbis rather than assigning one rate to unequal-rate PCM;
native resampling of those chains remains required. Same-rate channel-changing
chains are handled per link in source but still need corpus/native tests. Native
library calls, initial output allocation and DrumSynth generation remain outside
hard execution bounds. Channel mixdown beyond the retained native first-two-
channel behavior also needs an explicit full-application design and verification.

Source diagnostics passed across 136 changed C++ files, unchanged 86 operations
plus four agent tools, Python syntax, registration/inventories and whitespace.
No native compilation, fixture encoding, decoder, Qt case, DSP or render has run.
Memory quotas, actual decode/cleanup/cancellation measurements, complete format
and project/plugin compatibility, GIG groups, remaining application features,
actual inference/listening and every commission gate remain outstanding. The
complete LMMS transformation goal stays active.

Mixed-rate Vorbis chains now use native rate conversion in source:

- AudioResampler retains its original two-argument streaming API and adds an
  end-of-input overload. It passes completion to the same native src_process
  implementation, allowing offline/file callers to drain converter state. Native
  frame argument/result bounds are checked before exposing unsigned frame counts.
  Header documentation now describes retained converter state and caller-owned
  buffers. This follows the existing library's
  [streaming and end-of-input API](https://libsndfile.github.io/libsamplerate/api_full.html).
- Vorbis decoding plans every logical link's PCM length, channels and rate. It
  verifies aggregate input frames, native ratio limits, overflow-safe normalized
  output bounds and allocation capacity. Each link is sought and decoded through
  libvorbisfile in at most 4,096-frame blocks under cooperative stop checks. A
  same-rate link retains its decoded PCM directly. A differing-rate link uses a
  fresh native SincBest AudioResampler at the first link's output rate, consumes
  partial inputs correctly and drains buffered output before the next link. Mono
  duplication/first-two-channel behavior follows each link's own layout. Actual
  native output length may round a link duration by one frame; no padding or
  substitute waveform is generated. Empty links and sub-frame duration rounding
  are handled without returning an empty overall sample.
- An Ogg magic probe routes recognized Vorbis through this complete reader before
  accepting libsndfile output. The latter's native implementation can otherwise
  report success on only the first logical link. A recognized stream's failed
  conversion does not fall back to that partial result. Other Ogg codecs and
  other audio formats retain ordinary backend attempts. The internal one-argument
  fallback entry point remains available for direct native tests.
- Two new cases have four executions. Three chain rows use actual native encoder
  fixtures with distinct stream serials, changing mono/stereo layouts and equal,
  lower and higher source rates. Assertions cover first-link identity, complete
  normalized duration, second-link channel/sine accuracy, identical automatic
  routing, interruption, subsequent successful decoding and file release. The
  serial behavior is supported by the upstream
  [native Vorbis encoder source](https://raw.githubusercontent.com/libsndfile/libsndfile/master/src/ogg_vorbis.c).
  A resampler case uses small output blocks, its existing streaming overload and
  explicit finishing to assert consumption, buffered tail, complete duration and
  native converted audio. These cases are authored but unrun. Vorbis-disabled
  builds explicitly skip chain rows and do not prove full application coverage.

Source diagnostics passed across 138 changed C++ files, unchanged 86 operations
plus four agent tools, Python syntax, registration/inventories and whitespace.
No native C++ compilation, fixture encoding, codec/resampler, Qt case, complete
project load or render has executed. Full chain/corpus tests, endpoint behavior,
large/32-bit file callbacks, allocation and cancellation limits, actual CPU/heap/
I/O measurements, GIG groups, full plugin/project compatibility, remaining
application capabilities and real inference/listening remain outstanding. The
complete LMMS application transformation objective stays active.

### DrumSynth cooperative generation and isolated native working state in source

DrumSynth previously reused global envelopes, scratch buffers and output counters
across instances. It reopened/read the complete profile for each parameter and
published its raw output pointer before the native generation loop finished.
Its eight-byte envelope token buffer and unbounded point indexing could overrun
on malformed input; length and several native numeric conversions lacked checks.

- `DrumSynth` now owns and resets its envelope/parameter/scratch state per decode.
  It captures the profile through the native checked file-transfer helper once,
  closes the file, and reads parameters from those accepted bytes. Profile line
  parsing uses complete lines and instance-local strings instead of a 200-byte
  line buffer and shared `strtok` state. It retains case-insensitive sections/keys,
  parameter defaults and native v1/v2 handling. Comments retain display truncation;
  overlong parameter values are rejected. No new synthesizer or production song
  template has been introduced.
- Envelope parsing accepts finite, nonnegative, ordered times and finite levels,
  reserves the native sentinel slot, and rejects more than 31 points or values
  exceeding the existing 255-byte capacity. The native final-segment interpolation
  is retained; exhausted sentinel indexing is bounded. Frame planning checks
  finite scaled lengths and native integer/allocation limits. Level multiplication,
  noise step conversion, distortion rate/quantization and final PCM conversion
  avoid unsafe integer overflow, nonfinite casts and out-of-bounds block tails.
  Unsafe files fail explicitly. Native waveform, tone, noise, overtone, filter and
  distortion formulas still generate the accepted PCM.
- Synthesis checks the inherited native cancellation/budget scope between its
  original 1,200-frame blocks. An RAII array owns output until generation and its
  final cancellation check succeed, then the caller receives the complete buffer.
  Cancellation, malformed input and allocation failures return zero without
  publishing partial output. Existing caller-owned output is preserved. This is
  cooperative interruption, not a hard execution or memory bound; blocked I/O,
  allocations, callbacks and an individual block are not forcibly interrupted.
- Six new native unit cases have 24 executions, including three rate/channel/version
  rows, mid-generation cancellation/reuse, nested decoding that overwrites the old
  global state, 14 malformed-profile rows, the entire shipped corpus, and four
  invalid-argument rows. The corpus case loads all 764 original DS files using the
  real native generator and checks that no pointer appears before completion.
  A separate native runtime case compares generated integer PCM with automatic
  decoding and immutable SampleBuffer output, then deletes the original and checks
  accepted-input decoding/cancellation. CTest registrations include corpus paths
  and a 120-second DrumSynth timeout. All these native cases remain unrun.
- Repeatable fixture metadata inspection reads/hashes all 764 shipped files.
  It found no conflicts with the checked envelope and distortion restrictions;
  envelope values reach 245 bytes and 26 points. This inspection executes no
  generator and proves no PCM compatibility. Its input hashes and results are in
  `build/validation/drumsynth-corpus-metadata-20261012.json`.

Source diagnostics pass across 141 changed C++ files with the unchanged 86 native
operations plus four agent tools. No native C++ compilation, Qt case, DrumSynth
generation, sample loading, DSP or render has run: the native toolchain and LMMS
executable remain absent. Actual corpus/legacy PCM comparison, sanitizers,
allocation/CPU/heap measurements, interactive lifecycle/concurrency behavior,
GIG dependency groups and the remaining full application verification are still
required. The complete application transformation objective remains active.

### Checked embedded samples and incomplete native load rejection in source

The previous turn made authoritative source/test changes and was progress. This
turn re-read all five commissioning attachments, inspected the current worktree
and checked the continuing absence of a native compiler/toolchain and executable.
Embedded sample loading was still one unchecked path around native file-decoder
validation: permissive Base64 decoding could drop invalid characters, accept
incomplete input as empty data, and copy nonfinite binary floats into the DAW.
The studio also accepted a native `loadProject` return of true even when the GUI
loader had collected resource errors; presets and checkpoint restoration similarly
did not reject those errors.

- `SampleBuffer` retains its original public two-argument Base64 factory and adds
  an explicit checked overload with an error result. The checked path returns
  nullptr on failure and a valid buffer, including an empty one, on success. It
  scans the existing QString directly, permits standard Base64 and ASCII wrapping
  whitespace, keeps valid unpadded input, and rejects illegal characters, misplaced
  or excess padding and impossible final group lengths. The existing native Qt
  decoder is called with explicit failure status in at most 16,384-character
  blocks. Partial frame bytes are retained only in bounded scratch storage until
  a complete SampleFrame exists. Native signed frame/vector limits and finite PCM
  are checked before publishing the immutable buffer. Cancellation checks apply
  during scanning, around block decoding and before publication; allocation
  failures are reported. The native binary frame layout and byte order are retained.
  Qt's checked API is present in the existing Qt 5.15 minimum and Qt 6; its
  [result contract](https://doc.qt.io/qt-6/qbytearray-frombase64result.html) and
  [Qt 5.15 implementation](https://raw.githubusercontent.com/qt/qtbase/5.15/src/corelib/text/qbytearray.cpp)
  were inspected. An explicit impossible-group-length guard is necessary because
  that older implementation can otherwise decode a one-character unpadded tail
  without reporting failure.
- Native SampleClip, AudioFileProcessor and Slicer use the checked factory and
  collect load errors without publishing a partial candidate. Newly saved embedded
  AFP state includes its source `sampleRate`; checked loading preserves it, while
  older data without this attribute uses the existing engine-rate default. Invalid
  AFP rate/data keeps the previous buffer and reports the error. The compatibility
  wrapper continues returning an empty buffer plus the native warning on failure.
- Studio project/preset load now rejects collected native load errors; preset
  loading clears the previous load's errors first. The owning session then performs
  its existing mandatory checkpoint rollback and invalidates replaced handles
  through the revision counter. Checkpoint restoration checks native load errors
  after its existing document digest and asset preflight. A matching digest alone
  no longer makes an incomplete native restoration successful. GUI Song loading
  retains its existing tolerant error-report behavior. These are source changes,
  not a claim that every plugin's loader now reports every failure.
- Five new native unit cases have 20 executions. Their actual Qt decoding covers
  canonical/wrapped/unpadded multi-block buffers and exact binary bits including
  signed zero, denormals and finite amplitudes beyond unity; 12 malformed encoding/
  late NaN/infinity rows; invalid rates; empty input; and mid-work cancellation of
  data and long whitespace scans. Three native runtime cases have six executions:
  all three native consumers load a valid embedded project, reject corrupted data
  and retain checkpoint PCM/rate/history, with AFP/Slicer preset rejection also
  exercised; AFP explicit and legacy rates survive serialization and a rejected
  reload; and reauthenticated but nonfinite checkpoint data rejects recovery
  without committing a new manifest. Native cases use the real project/instrument
  models and decoder; missing-plugin skips do not prove full build coverage. CTest
  registers SampleBufferTest with a 60-second timeout. All these cases remain unrun.
- A separate XML metadata inventory parsed all 216 shipped project/preset/template
  files and found no nonempty declared embedded samples in these three consumers.
  It therefore supplies no existing embedded PCM corpus coverage; that coverage
  must come from actual native fixtures and additional real projects. This scan
  did not load LMMS or execute a codec. Its result is recorded in
  `build/validation/embedded-sample-corpus-inventory-20261012.json`.

Source diagnostics pass across 142 changed C++ files, the unchanged 86 native
operations plus four agent tools, build/test inventories, Python syntax and
whitespace. No native C++ compilation, Qt codec case, project/preset load,
checkpoint recovery, DSP or render has executed. Complete existing-project corpus
comparison, failure semantics for remaining native/hosted plugins, GUI lifecycle,
whole-DOM parsing and native allocation limits, toBase64's existing large-buffer
narrowing/serialization path, measured CPU/heap use, GIG dependency groups and the
remaining complete application transformation/verification are still required.
The full objective remains active.

### Checked embedded encoding and native serialization failure propagation in source

The previous turn changed authoritative source/test state and was progress. This
turn re-read the commission and inspected SampleBuffer encoding, DataFile writers,
native sample serializers and the project journal. Encoding still narrowed the
entire binary buffer length to int and copied/encoded it in one call; a failed
sample serializer had no result path through the native void saveSettings API.

- SampleBuffer adds a checked encoding overload and an embedded DOM writer while
  retaining the existing no-argument encoding method. Native frame/rate limits,
  size multiplication and Qt/host text capacity are checked before encoding.
  Actual Qt encoding uses immutable raw views of at most 1,536 native frames
  (12,288 bytes in this ABI); every complete block ends on a three-byte Base64
  boundary, so concatenation keeps the existing PCM bytes and native canonical
  encoding. Each block validates finite audio and the inherited stop/budget scope.
  Only a complete final QString is returned; failed/cancelled work clears the
  caller's output. Native allocation errors become explicit failures. The original
  unchecked whole-buffer int cast and raw-byte copy have been removed. The complete
  encoded string remains in memory; this is bounded scratch work and cooperative
  cancellation, not a memory quota or hard timing guarantee. This follows Qt's
  [non-owning raw-data view contract](https://doc.qt.io/qt-6/qbytearray.html#fromRawData).
- SampleClip, AudioFileProcessor and Slicer serialize embedded data through the
  checked writer. Failed encoding preserves the previous DOM attribute and records
  the first transient serialization diagnostic as a document-level processing
  instruction. The diagnostic follows normal DOM copies/clones, requires no global
  registry or document-lifetime scope, and is checked before native publication.
  DataFile's string output refuses a failed document, stream output sets WriteFailed,
  file/resource output rejects it before folder/file changes, and parsing such a
  diagnostic document leaves its native type Unknown. Successful files contain no
  diagnostic instruction. Direct inherited QDomDocument serialization can bypass
  DataFile's guard, so callers writing through that base API must still inspect the
  diagnostic; a regression case intentionally uses it to verify rejection on load.
- DataFile file output now flushes/checks the actual QTextStream, verifies complete
  compressed writes and device status, and checks cooperative cancellation before
  publication. Backup copying checks cancellation between chunks and verifies the
  source error before committing the backup. QSaveFile still owns atomic native
  publication. A failed embedded serializer cannot replace an accepted file or its
  backup or begin bundle construction. A cancellation after a complete backup has
  committed may retain that complete backup while refusing the new main file;
  bundle resources already copied can remain orphaned after later failures.
- Native journal capture refuses incomplete documents before clearing redo history.
  Failed undo/redo capture restores the popped checkpoint entry and reports a native
  error instead of installing incomplete state. Its existing void checkpoint API
  does not prevent every subsequent GUI edit after a failed capture; transactional
  editing and restoration errors in remaining journal/model callers still need
  review. The owning studio's existing mandatory recovery suspends cancellation
  and restores its prior complete embedded checkpoint.
- Four added native unit cases have 17 executions: eight frame/group/block boundaries
  compare exact Qt encoding and decoded PCM bits; late nonfinite/cancelled encoding
  publishes no result; project/preset/journal diagnostics survive copies and stop
  string/stream/load output; and MMP/MMPZ/XPF failure preserves existing files,
  backups and an uncreated bundle. SampleBufferTest now has nine cases/37 executions
  in total. Two native runtime cases cover cancelled embedded checkpoint rollback
  with PCM/rate/history/revision checks, and preservation/reuse of native undo/redo
  after failed journal capture. All 19 added executions are authored but unrun.

Source diagnostics pass across 143 changed C++ files, unchanged 86 native operations
plus four agent tools, registrations/inventories, Python syntax and whitespace.
The latest source-only record is `build/validation/native-studio-source-check-20261014.json`.
No native C++ compilation, Qt encoding/file/journal case, checkpoint operation,
DSP or render has executed; the native toolchain and executable remain absent.
Actual large/32-bit allocation limits, PCM/project corpus compatibility, GUI/clipboard
and journal lifecycles, whole-DOM/XML/compression execution, native and hosted plugin
coverage, CPU/heap/I/O measurements, GIG groups and the remaining application-wide
transformation and verification are still required. The full objective remains active.

### Native recording input ownership, completion and fault handling in source

The previous goal turn changed authoritative source/test state and was progress.
This turn re-read the commission, checked current build readiness beyond PATH and
inspected native transport/capture before continuing source work. The read-only
`audit_native_build.py` inventory found none of CMake/Ninja/cl/clang++/g++/qmake on
PATH or in the inspected CMake/LLVM/Visual Studio/Qt/MSYS/local-program locations.
All 17 pinned submodules remain uninitialized and no native executable exists.
This is targeted discovery, not an exhaustive machine inventory or a compilation
result. Exact locations, pinned commits and observations are recorded in
`build/validation/native-build-readiness-20261013.json`. No denied install/network/
Docker action was retried and no repository metadata was changed.

Inspection confirmed that studio startup uses Engine::init(true), selecting the
native render-only dummy audio/MIDI devices; AudioDummy still runs the native DSP
clock but provides no physical input/output. The 86-operation interface does not
yet expose live transport, audio/MIDI backend setup, capture arming or recording
session finalization. Song::record retains its existing record-only TODO. These
required capabilities remain in scope; native capture fixes do not supply them.

- The existing SampleRecordHandle kept raw allocated blocks, accumulated frames
  without checked bounds, flattened with an int copy offset, and assigned the
  engine's current input rate on completion. Its destructor allocated and wrote
  the clip directly even when an audio thread removed it. Its constructor omitted
  the audio-bus binding required by AudioEngine::addPlayHandle; PlayHandle's bus
  pointer was itself uninitialized. These are source findings, not executed fault
  reproductions.
- New native SampleRecordingBuffer owns the actual input blocks with RAII, checks
  signed frame/vector limits before pointer arithmetic/allocation, validates finite
  stereo PCM, and retains the first nonempty block's input rate. A rate change or
  invalid block makes failure sticky and prevents prefix publication. Empty input
  invents no waveform; a completed take flattens all accepted blocks with size_t
  indexing and creates the existing native SampleBuffer. Allocation failures return
  an error. The complete take and its final copy still require full memory; this
  does not provide a capture quota, spool-to-disk path or real-time allocation bound.
- SampleRecordHandle attaches to its native SampleTrack audio bus and does not
  request an output play buffer. It consumes Engine's existing input buffer through
  the checked native capture store, publishes its count atomically for observers,
  and ends a failed take. Destruction transfers capture ownership to a completion
  callback; complete buffer creation, clip mutation and native error reporting run
  on the clip's thread, directly or through the existing Qt 5.15-compatible queued
  functor API. A guarded clip and context-owned callback handle deletion after
  queuing. The native engine/model guard must keep the clip alive during enqueue;
  QPointer is not ownership or a substitute for that synchronization. The relevant
  [Qt context invocation](https://doc.qt.io/qt-6/qmetaobject.html#invokeMethod)
  and [guarded pointer](https://doc.qt.io/qt-6/qpointer.html) contracts were inspected,
  including the Qt 5.15 header. No new musical engine or external controller exists.
- AudioEngine input append now uses its native model guard with exception-safe
  allocation, checked frame arithmetic and finite PCM validation. Failed input
  clears the pending frame batch, marks its period faulty and increments an atomic
  failure epoch. Recorders reject faults during capture and before completion,
  including a rejected final block with no subsequent period. Per-period flags
  prevent a recorder born after a fault from treating a late-starting remainder as
  a complete take. PlayHandle initializes its bus pointer and AudioEngine refuses
  null/unattached handles while retaining ownership cleanup. Song::stop clears its
  recording flag in active and already-stopped modes.
- Native play-handle retirement now detaches each handle before destruction.
  Recording completion emits SampleClip dataChanged, whose native slot removes
  track play handles again. The old delete-before-erase order could therefore
  rediscover the same handle or invalidate the outer iterator. Track/type removal
  rescans after callbacks; queued removal consumes its pending entry first; finished
  processing uses an active-membership-checked snapshot. Clearing new handles now
  releases their bus/pool/owned objects rather than only freeing the queue cells.
  Three authored lifecycle rows exercise destructor-driven recursive removal of
  another handle through these real engine paths. This source finding and fix
  remain unverified by native execution.
- Five native capture-store cases have nine executions for exact multi-block PCM,
  independent input ownership/rate, five failure rows, overflow planning without
  enormous allocations, empty input and ownership transfer. Eight runtime cases
  have thirteen executions. They use the native input bus, actual audio-engine jobs,
  recorder and clip to cover direct/queued completion, two input appends with real
  buffer growth, rejected PCM, failed final/first periods, clip deletion before
  event delivery, empty capture, stop state and unattached handles. Diagnostic PCM
  is test input, not production capture or a substitute generator. The new CTest
  target has a 60-second timeout. All 22 executions remain authored but unrun.

Source diagnostics pass across 150 changed C++ files with unchanged 86 operations
plus four agent tools, registration/inventory checks, Python syntax and whitespace.
No native configure/build, Qt case, capture store/engine job, thread callback,
physical device, checkpoint, DSP or render has executed. The complete recording
workflow still needs explicit native device/transport/arming controls, input and
MIDI routing, latency/start-offset behavior, a completion barrier before save or
shutdown, bounded resources, lost-device/overrun reporting and GUI/cancellation
lifecycle verification. Queued completion can be lost on clip destruction and
must run before a dependent checkpoint; event queuing alone does not prove durable
recording. Full native/plugin/project/agent verification and the entire commission
remain outstanding. The full objective stays active.

### Native capture completion before serialization in source

The previous turn made source/test changes and was progress. This turn re-read the
commission and inspected the recorder destructor, native stop/save/serialization,
engine shutdown and the existing capture tests. A queued callback owned the only
copy of a completed take; serialization before its delivery could save the older
clip buffer, while removing the notification could discard that take even if the
clip remained alive.

- SampleClip now owns completed capture states separately from its published
  immutable sample. Native record handles register their active take, then stage
  completion under the engine model guard when retired. The Qt callback is only
  a notification: captured PCM stays on the clip until finalized or the clip is
  deleted. Completed states retain their input rate and errors through the native
  capture store. Finalization validates and installs complete SampleBuffer data
  on the clip thread, clears the recording arm after completion and reports a
  failed take while retaining its previous sample. The existing queued GUI path
  remains, with an explicit finalization entry point independent of event polling.
- The application-thread persistence entry point scans sample clips in both Song
  and PatternStore, checks active/reentrant publication before consuming any
  pending capture, and finalizes pending clips under the native model guard. Song
  saving/checkpoint serialization calls it before producing a document or touching
  the target file. SampleClip serialization also checks its own capture state, so
  journal/clipboard serializers can report failure through the native document
  diagnostic. An active recording must finish before a coherent native save;
  reentrant saving from a publication callback is rejected. A capture failure
  found by this save rejects that save. A previously reported failed take retains
  its prior musical data and does not imply that a later save is a successful new
  recording.
- Native AudioEngine finishing retires record handles in active and newly queued
  lists before deleting them, preserving the earlier callback-safe retirement
  order. Song::stop performs retirement even when the transport was already
  stopped and completes pending publication when called on the application thread.
  An audio-thread stop leaves publication for the clip thread; the save barrier
  can still finish it synchronously. Shutdown stops processing, retires captures
  and attempts pending publication before project clearing. This does not make
  shutdown save the project automatically or establish crash-safe recording.
- Six authored native cases have seven executions. MMP/MMPZ saving before queued
  event delivery must persist exact captured PCM/rate once; deferred failure keeps
  the existing file/PCM; active saving preserves file/backup and native stop permits
  a subsequent complete save; stopping newly queued handles clears capture state;
  worker publication cannot bypass thread affinity; and removing the posted
  notification before a real runtime checkpoint still retains the completed PCM.
  They use the native input bus, handles, models and serializers, not a production
  waveform substitute. All seven executions remain unrun.

Source diagnostics pass across 150 changed C++ files, unchanged 86 operations plus
four agent tools, build/test inventories, Python syntax and whitespace. Native
build tools/executable remain unavailable; no native compiler, Qt case, completion
callback, save/checkpoint, shutdown, hardware input, DSP or render has executed.
Live backend/transport/arming interfaces, offsets and latency, overlapping-take
semantics, edits during capture, GUI/cancellation lifecycles, resource quotas,
MIDI input, discarded-clip behavior and full application verification are still
required. Completed buffers remain in memory and clip deletion intentionally
discards that clip's pending states. The full DAW transformation objective remains
active and unproven.

## Native transport controls in source

The 2026-10-13 continuation adds five operations inside LMMS's native studio
capability dispatcher: transport inspection, song/pattern/MIDI clip playback,
explicit pause state, stop and tick seeking. They operate on Song, PatternStore,
MidiClip, Timeline and the existing AudioEngine under its native model guard.
There is no replacement clock, external controller or GUI automation.

- Inspection reports the native flags and current/per-mode positions, timeline
  loop and stop state, pattern/MIDI selection, recording state and actual audio
  device kind. Dummy, file, device and unavailable are distinct. The engine's
  reported name is labelled separately because a temporary file device can retain
  another device's name. Running/capture/reservation/rate/period metadata is
  descriptive; it does not establish audible hardware output or working input.
- Playback prevalidates native mode, pattern/clip selection and MIDI timing
  bounds before changing transport. Song and pattern reject MIDI-only arguments.
  MIDI preview retains native looping. Pause is explicit and repeatable, resume
  requires a running device, and seek uses native positionJumped/frame reset.
  Stop retains each timeline's native stop behaviour. Invalid requests preserve
  the previous playback state. Native export/device reservations reject controls.
- These five operations are transient playback controls. They preserve native
  project revision/history, while the runtime retains their durable request
  receipts. Replaying a request returns its original response without repeating
  playback; project recovery stops transport and does not replay a start command.
  Project inspection includes transport and now acquires the native model guard
  itself, including when called directly by runtime observations.
- Playback commands reject song recording, armed sample/automation clips,
  active/pending captures and reentrant publication. Completing capture can edit
  a project and needs a recording transaction, so this addition does not expose
  recording stop/arm or device selection. The default native studio still uses
  render-only dummy audio/MIDI; its clock/DSP can advance without physical output.
- Native MIDI playback now connects the selected clip's destruction signal,
  stops while that clip and its notes are still valid and clears the stored
  pointer. Selecting another clip disconnects the prior hook. MIDI preview also
  updates native VST playback synchronization. Project clearing now stops paused
  transport and recording flags as well as active playback; previously a paused
  song with no MIDI clip could remain paused after native project replacement.
- Eight authored native cases have 26 executions: all three stop behaviours,
  native clock advance and paused stability, four mode/container combinations,
  nine invalid argument paths, five recording states, reentrant publication,
  stopped/reserved backends, deletion during playing/paused MIDI preview with
  superseded-clip preservation, and durable replay/recovery/history semantics.
  The whole-process harness adds actual dummy clock progression and pause/replay
  checks to its required native executable run. These checks remain unrun.

Source diagnostics pass across 150 changed C++ files, 91 distinct native
operations plus four agent tools, test/build inventories, Python syntax and
whitespace. The read-only readiness audit still finds no compiler/CMake/Qt
toolchain or native executable and 17 uninitialized pinned submodules. No native
transport transition, destruction callback, Qt case, DSP period, process harness,
render or physical audio/MIDI device has executed here. Live backend/recording
transactions, latency/routing/quotas, full native compilation and verification,
compound GIG resources and the broader autonomous production commission remain
required. The full objective remains active and unproven.

## Native live backend startup and protocol stream isolation in source

The 2026-10-13 continuation exposes the existing native audio/MIDI device
factories through headless startup. `--studio-live` uses Engine::init(false) with
QCoreApplication. Default studio startup retains Engine::init(true) and dummy
devices. Optional `--studio-audio`/`--studio-midi` select a compiled factory by its
native name, with `auto` preserving native probing. Backend-specific settings
still belong to LMMS's existing configuration, and no presentation widget is
required to choose these startup paths.

- AudioEngine now provides compiled audio/MIDI factory names; existing native
  validation uses these same lists. `audio.backend.list` exposes them without
  probing hardware and reports loaded configuration and actual native state.
  Startup overrides reject unknown/missing/duplicate options before engine or
  session creation. Their temporary ConfigManager values are restored immediately
  after initialization, including original attribute absence, so later saving
  cannot persist a CLI choice. AudioEngine retains the requested startup names
  separately from configured and actual/fallback names. Ready includes transport
  metadata; MIDI dummy/device kind is distinct from port-connectivity claims.
- The native studio reserves an owned protocol FILE stream before second-stage
  argument processing, configuration and engine/plugin initialization. Ordinary
  stdout is redirected to stderr. POSIX uses a close-on-exec duplicate; Windows
  uses a noninheritable handle/CRT descriptor and updates the process's standard
  output handle as well as CRT stdout. The original Windows text mode is retained
  for scoped restoration. Native Runtime writes to the reserved stream and stops
  on missing/failed writes or flushes. CLI teardown closes the private stream and
  keeps diagnostics redirected during subsequent static/plugin destruction;
  scoped callers can restore stdout instead. Combining stdout and stderr in a
  consumer still combines diagnostics and protocol. Stalled output/diagnostic
  sinks remain capable of blocking; these writes have no hard deadline.
- JACK audio restart notices now preserve dialogs only when a GUI exists and
  report headless diagnostics otherwise. JACK MIDI shutdown passes the native
  object to its callback, uses an atomic quit flag, and queues presentation onto
  that object's Qt thread. This removes direct GUI access from the server callback.
  Shared MIDI port/reconnection state, device shutdown races and physical callback
  verification remain unresolved; these edits do not prove JACK health or recovery.
- Four authored native backend cases cover discovery without an engine/config
  mutation, native non-render initialization with explicitly selected dummy
  factories and actual clock advance, render-only bypass of configured factories,
  and refusal to enter input polling when readiness output cannot be delivered.
  One native protocol case has two executions covering scoped restoration and
  process teardown. A native child sends CRT/OS stdout and forwarded-child
  diagnostics; only protocol frames may reach the reserved stream, whose
  inheritance flags are inspected. All six executions remain unrun.
- The full executable harness now exercises live startup with explicitly chosen
  native dummy factories, readiness metadata, native clock progression,
  unchanged configuration backend attributes and ten rejected CLI cases. It
  continues requiring the complete bundled instrument build and native DSP/render
  checks. These additional startup checks are authored but unrun and provide no
  substitute for physical device or autonomous music-production verification.

Source diagnostics pass across 155 changed C++ files, 92 native operations plus
four agent tools, build/test inventories, Python syntax and whitespace. The
read-only readiness audit still finds no native toolchain/executable and 17
uninitialized pinned submodules. No native backend initialization, pipe/handle
separation, child inheritance, JACK callback, Qt test, process harness, physical
audio/MIDI stream or native render has run. Runtime backend switching, device/port
enumeration, capture transactions, latency/routing/quotas, full compilation and
compatibility verification, compound GIG resource preservation and complete
autonomous production remain required. The commission remains active and unproven.

The descriptor ownership and inheritance design follows the native
[Microsoft CRT ownership contract](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/open-osfhandle?view=msvc-170),
[Windows handle duplication](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-duplicatehandle)
and [POSIX descriptor duplication semantics](https://man7.org/linux/man-pages/man2/dup3.2.html).
These references support the implementation approach, not execution evidence.

## Native backend failure state and PortAudio input in source

The 2026-10-13 continuation addresses two native PortAudio omissions: incoming
PCM was ignored and Pa_StartStream's result was not checked. AudioDevice also
previously set its running flag without retaining a backend failure. These paths
matter to the complete native audio engine and capture workflow, independently
of the conversational controls that still need recording transactions.

- AudioDevice now publishes running/failure/native-code fields in one atomic
  numeric snapshot. Backend failure clears running state, preserves the first
  reported cause until a new start and invalidates input on capture-capable
  devices. Publication makes the input-loss epoch visible before the failure
  snapshot. Reporting enters neither Qt nor an allocation path. Control-thread
  start/stop remain serialized by their owner, repeated running starts are
  idempotent, and C++ hook exceptions retain propagation after being reported.
  The primitive does not detect errors that a backend never reports.
- AudioEngine input buffers now retain their producing epoch. Newly queued input
  from before a reported disconnect cannot become a successful late-starting
  take, and new valid input after recovery uses the current epoch. Active takes
  retain their earlier epoch and fail as a whole. Existing finite/bounds/input
  rejection and pending clip publication remain authoritative. An intentional
  normal stop is not a failed-take declaration. Device replacement, capture
  offsets, stream clock continuity and complete recording semantics need further
  work; this does not supply durable autonomous capture by itself.
- PortAudio initializes through its existing library guard, now allowing a failed
  initialization to return through native factory fallback. Device/host info and
  channel selections are checked; a null/unavailable stream is not stopped or
  closed. Start/stop failures and unexpected stream completion report native
  status. Normal stop clears running before the finished callback, so successful
  intentional completion does not become a false disconnect. JACK activation
  failure and shutdown now also report processing failure/input loss. Other
  backend failure paths, shared JACK reconnection, cleanup races and physical
  failure behavior remain incomplete or unverified.
- The PortAudio callback uses the native AudioEngine input bus, DSP and recorder.
  It maps mono to stereo or uses the first two selected input channels, checking
  every supplied channel for finite values. Reported underflow/overflow, missing
  input and nonfinite data reject recording rather than publishing a prefix.
  Input-only callbacks still process the native DSP clock. Output retains native
  mono/stereo/additional-zero-channel behavior. Native view/frame/byte bounds are
  checked before access; conversion uses 256-frame stack blocks. Exceptions do
  not escape the C callback and valid output buffers are silenced on abort. No
  PortAudio API is called inside processing/finished callbacks. Native input
  accumulation, plugin/DSP calls, stop/close and output sinks still lack hard
  execution/resource bounds; this is not a hard real-time guarantee.
- Studio inspection and readiness report processingFailure/nativeError plus the
  input error epoch as a full-width decimal string. Factory fallback and actual
  processing failure are distinct. Playback/resume already require a running
  backend and reject reported failures. Musical playing state may remain set when
  a backend fails; the observation exposes both states without inventing playback
  progress or successful hardware output.
- Three backend health cases have five authored native executions: failure/retry,
  duplicate starts and propagated exceptions; coherent asynchronous snapshots
  with capture/output-only devices; active/late capture rejection and fresh-input
  recovery. Three conditional PortAudio cases have 13 authored executions:
  mono/stereo/wider/input-only/partial callback PCM reaches native recorded clips;
  missing/lost/nonfinite input preserves previous PCM; malformed shapes fail
  before access. These use native LMMS model/input/DSP/recording paths, with a
  test-only status fixture and diagnostic input rather than a replacement engine.
  They do not probe PortAudio hardware or test the physical host lifecycle. All
  18 new executions remain unrun.

Source diagnostics pass across 161 changed C++ files, unchanged 92 native
operations plus four agent tools, updated build/test inventories, Python syntax
and whitespace. The readiness inventory still has no toolchain/native executable
and 17 uninitialized submodules. No compiler, Qt case, real PortAudio callback,
device start/stop/disconnect, native recording publication or render has run.
Recording transactions, all remaining native subsystems and full compatibility,
DSP, resource and autonomous production verification remain required. The full
commission stays active and unproven.

The implementation follows the native
[PortAudio callback, input-loss and stream-finished API contracts](https://files.portaudio.com/docs/v19-doxydocs/portaudio_8h.html).
Documentation is evidence of API semantics; it does not verify these changes.

## Bounded native recording transaction in source

The 2026-10-13 continuation adds audio.record inside LMMS's studio capability
dispatcher and a core StudioRecording implementation. It records the existing
native input bus into an existing SampleClip using SampleRecordHandle, rather
than a replacement generator, external controller or presentation automation.

- Capture requires stopped transport, a running capture-capable native backend,
  disarmed/completed prior recording and a sample-clip destination. Duration,
  frame, PCM-byte and cooperative wall-time bounds are checked before arming.
  The native recorder stops at the requested frame quota, including truncation
  of its final input block. Stale buffered input is discarded before capture;
  device replacement also resets input and invalidates capture epochs.
- SampleClip now reserves pending completion. A reserved take cannot be published
  by an automatic notification, serialized as an older sample or finalized by
  an unrelated save. Default recorders cannot start on that reserved clip. The
  controller accepts one complete take with the exact frame count/native rate
  before clip-thread publication. Cancelled, failed, empty or premature takes
  discard their buffers. Existing ordinary recording retains its normal pending
  completion path. Native record retirement can target a particular clip, even
  if its QObject was removed; its saved identity is compared without dereferencing
  it. Record handles with a removed destination finish rather than continuing
  indefinitely.
- Optional accompaniment uses native Song/Timeline playback and the existing
  record flag to suppress the destination's prior sample, while one reserved
  recorder owns capture. Song looping is temporarily disabled and restored, and
  accompaniment is stopped afterward. Clip/tempo playback refresh removes
  ordinary sample handles while preserving the reserved recorder; explicit
  retirement and track destruction retain their normal semantics. Input-only
  recording uses the backend's existing DSP clock with song transport stopped.
- Studio Runtime's existing mutation/checkpoint transaction now owns accepted
  captures. Success creates a native project revision/history entry; failed
  capture restores the previous checkpoint/history. Failed object restoration
  still increments revision to invalidate handles. Replaying a receipt returns
  the prior result without recording again, and recovery restores captured native
  PCM without restarting playback. Input waits preserve existing nested metadata,
  agent cancellation and shutdown admission. Frame-buffer flattening now checks
  the native cancellation scope between blocks; cleanup suspends cancellation
  while retiring owned handles and releasing reservations.
- Native studio rendering rejects live, pending, reserved or publishing capture
  and active automation recording before changing export/transport state. This
  prevents an ordinary render from finalizing a take outside the recording
  transaction. Save and per-clip serialization also respect the reservation.
- Five authored native cases have 12 executions: exact PCM/rate/quota publication
  without accompaniment, with accompaniment and during tempo refresh; no input,
  cancellation, disconnect, nonfinite input, byte-budget and unsupported-backend
  refusal; competing reservation/default recorder/save/render rejection; native
  history/replay/undo/redo/recovery; and failure rollback with unchanged history.
  A test-only capture adapter supplies diagnostic PCM to the actual native input,
  DSP, recorder, clip and runtime. The whole executable harness adds dummy-input
  refusal, rollback and replay. All native recording executions remain unrun;
  these fixtures do not establish hardware capture or musical production quality.

Source diagnostics pass across 164 changed C++ files, 93 native operations plus
four agent tools, updated core/test registrations, Python syntax and whitespace.
The read-only readiness audit still finds no toolchain/native executable and 17
uninitialized submodules. No native compiler, recording operation, callback,
capture reservation, Qt test, project checkpoint, device input or render has run.
The PCM-byte quota does not bound process memory: block overhead, flattening,
Base64/DOM/checkpoint storage and other allocations remain additional costs. Native
locks, plugins, I/O and allocation still prevent hard deadline guarantees.
Indefinite capture arm/stop, input routing/monitoring/latency, in-flight crash
durability, physical backend verification, GUI coexistence, complete compatibility
and autonomous full-application verification remain required. The full commission
remains active and unproven.

## Native MIDI routing and subscription persistence in source

The 2026-10-13 continuation adds midi.inspect, midi.configure and midi.subscribe
for the existing instrument MidiPort. Inspection includes native channel,
controller, velocity, note, program and base-velocity models, client capability,
available endpoints and requested routing. Project inspection includes these
settings. Configuration prevalidates every field before editing; subscription
selection requires an available non-raw endpoint when enabling. Removing a
retained missing request is supported. Connection verification remains false:
native subscription APIs return no acknowledgment and storing intent does not
prove hardware connectivity. Immediate external MIDI messages cannot be retracted
by project rollback, even though native model configuration has revision/history.

Native subscription persistence now writes structured child entries for exact
comma-containing/Unicode names and missing requests. Legacy comma attributes
remain readable/writable; older readers retain their comma ambiguity. Desired
names survive disappearance and reapply when their exact name reappears. Disabling
a direction clears requests. Reload explicitly deselects old subscriptions;
previously it called subscribe with default true for a deselected saved port.
Name snapshots avoid invalidating implicitly shared map iterators during removal.

Raw parser storage is initialized, SysEx/system-common status cancels running
status, and one-byte voice messages clear unused data. Raw output now uses LMMS's
existing serializer for all seven channel voice types, with an output mutex
around complete messages. Shared serialization rejects invalid channel/data and
output-buffer bounds before access. WinMM now uses it for correct 14-bit pitch
bend instead of packing the value as a plain parameter byte. ALSA endpoint-list
copy/publication has a dedicated snapshot mutex released before signals.

Nine authored native cases have 22 executions: eight voice output rows; parser
initialization/running status/SysEx/realtime/reset; native channel/override/program
models; seven malformed rows; concurrent raw packet integrity; structured names
and hotplug/reload; legacy deselection; studio configuration/replay/recovery;
and prevalidation. Named/raw clients supply diagnostic boundary fixtures; models,
ports, byte parser/serializer and studio state use their native implementations.
The full executable harness adds MIDI model serialization, replay and default
dummy subscription refusal. All new native executions remain unrun.

Source diagnostics pass across 173 changed C++ files, 96 native operations plus
four agent tools, build/test inventories, Python syntax and whitespace. Readiness
still finds no native compiler/toolchain/executable and 17 uninitialized pinned
submodules. No native MIDI operation, parser/serializer, callback, Qt case,
device/connection, project recovery or render has executed. Backend port-map/
callback lifetime synchronization, shutdown/hotplug/delivery errors, blocking
output, raw timestamp scheduling and SysEx/system output, physical MIDI and the
complete compatibility/resource/autonomous DAW verification remain required.
The full commission remains active and unproven.

Message formats follow the
[MIDI Association MIDI 1.0 summary](https://midi.org/summary-of-midi-1-0-messages).
This supports API semantics, not hardware or execution claims.

## Native MIDI callback retirement and bounded delivery in source

Raw MIDI previously iterated a mutable vector of raw MidiPort pointers from a
backend thread. WinMM iterated unsynchronized subscriptions and forwarded a
pointer to its callback's stack-local HMIDIIN. Recovery/track deletion could
retire the processor/port concurrently, and queued detection could retain a
borrowed source address. Source changes now register shared delivery records
under a client mutex, copy scalar events with owned source names, and post one
coalesced callback on each port's QObject thread. Registration is held until
posting finishes; port retirement marks the record inactive and empties it
before member destruction. Client retirement invalidates records and surviving
ports. InstrumentTrack and MidiController detach input processors at the start
of destruction. Native model access uses AudioEngine's existing model guard.

Each port has at most 1024 waiting events; a callback handles at most 64 before
yielding through the Qt queue. Disabled input consumes no queue quota. Routing
disable clears pending input; unsubscribe removes only the affected source's
packets. Address reuse cannot reactivate an old delivery record. Overflow clears
pending events, suspends input for that port, records dropped counts and schedules
native all-sound-off through the owner thread. Processor/backend/enqueue errors
also become observable failures, and exceptions do not unwind through WinMM's C
callback or the queued processor call. Cleanup is best effort: a failed enqueue
may prevent its callback. Native all-sound-off bypasses input mode/channel
filtering and may silence other voices on that instrument. Faults persist until
the native port is recreated; other ports remain independent. No claim of hard
real-time memory/allocation/lock/DSP or output latency bounds follows from the
queue quota.

WinMM guards endpoint/subscription tables and holds a separate output lifetime
mutex. It copies/removes handles under the table mutex, then resets/closes outside
that mutex; callbacks can finish without acquiring a held shutdown lock. Output
sends release the table mutex, allowing concurrent/loopback input. Device
capability/start failures no longer publish failed input entries. Incoming voice
data is validated, pressure/program unused bytes are cleared, and pitch bend is
converted from two checked seven-bit bytes. Hardware output/close acknowledgment,
reconnection and physical devices remain unverified.

MidiEvent owns a source-name QString so copies may outlive callback/native event
storage. This changes the native C++ type's storage/ABI and requires rebuilding
LMMS, native plugins and helper hosts together. Existing prebuilt native plugin
compatibility and added event-copy/cache/allocation costs have not been tested.
Foreign plugin wire formats are not used to establish that compatibility.
Queued input depends on the owner thread's event loop; native channel/velocity
models are evaluated when delivery executes. ALSA and CoreMIDI retain their
direct input paths and unresolved map/callback lifetimes; inspection reports
`queuedDelivery:false` for them. No global backend safety claim is made.

The native MIDI suite now has 18 authored cases: the prior 22 executions plus
14 executions for owned thread/source/order delivery, six retirement/routing
rows, concurrent raw construction/deletion, selective unsubscribe, overflow and
disabled-port isolation, exception handling, borrowed/malformed rejection, and
two conditional WinMM callback cases. Total: 36 executions with WinMM, 34 without.
Fixtures provide diagnostic backend input/handles while native parser, port,
queue, WinMM callback, models and runtime code are exercised by the authored
tests. Fixture WinMM handles never reach reset/close APIs. None has run.

Source diagnostics pass across 177 changed C++ files and 96 native operations
plus four agent tools. Read-only build readiness still finds no compiler/CMake/
Qt toolchain or LMMS executable and 17 uninitialized pinned submodules. No native
compile, callback, Qt case, device, plugin ABI/CPU/memory/latency trace, project
recovery or autonomous audio workflow has executed. ALSA/CoreMIDI lifetimes,
backend shutdown/hotplug/error completion, compatibility and complete DAW
verification remain required. The full commission remains active and unproven.

The dispatch uses the context/functor overload of
[QMetaObject::invokeMethod](https://doc.qt.io/qt-6.5/qmetaobject.html); WinMM input
avoids multimedia API calls as required by Microsoft's
[MidiInProc callback documentation](https://learn.microsoft.com/en-us/previous-versions/dd798460(v=vs.85)).
These sources describe APIs, not execution evidence.

## Native ALSA sequencer delivery and shutdown in source

Inspection found that the ALSA reader unlocked its sequencer mutex before
traversing mutable port IDs. The missing-destination path continued without
relocking, and the fetch-failure path could unlock again at loop exit. Incoming
events entered processors directly while project recovery could destroy tracks.
Shutdown ignored a timed join result, used a volatile stop flag, never woke its
pipe and closed resources only while the thread still reported running.

The native ALSA client now uses the shared owning-thread input dispatcher.
Destination-map access, mutation, subscription and handle access use the
sequencer mutex. Destination selection holds that mutex through registration of
the owned packet; the reader enters no native model/DSP guard. Source names are
captured for external peers before native event storage expires, with numeric
address fallback on failed metadata queries. Pressure uses the native control
value, and wide control/program/bend values are validated before narrowing.
Foreign destination clients/ports and unsupported borrowed payloads are rejected.
Representable tick note timestamps retain their native values; realtime note
stamps map to zero rather than reinterpreting union bytes. The output clock now
uses the native 48 ticks per quarter note as described below; real/musical
timestamp phase alignment and hardware timing remain unverified.

Startup checks open/client/queue/tempo/start/drain/pipe setup. Allocations that
belong to a failed or nonstarted client remain owned and are released during
destruction. Pipe descriptors start at -1, are nonblocking and closed on exec.
The atomic stop flag plus wake pipe allows an idle poll to finish; destruction
joins the reader before closing handles/queues/descriptors. No sequencer mutex
is held during joining. This avoids freed-state access from an ignored timed
wait, but it does not establish a hard deadline for native ioctl/lock/driver
work. Nonblocking reads and 64-event lock batches check shutdown even during
traffic. The reader tracks remaining library-buffer data, checks native poll
revents and treats EINTR/EAGAIN as recoverable. FIFO overrun faults existing
ports through native all-sound-off while the reader remains usable. Fatal input
failure stops it and queues endpoint removal on its QObject thread.

Inspection exposes observed reader activity and the first native error across
ALSA operations; unknown observations on other backends remain explicit.
The native factory retains failed ALSA/raw/JACK/sndio attempts before deletion,
including whether a native error code was observed. These transient records are
available in transport/backend inspection and do not certify a connection.
Input failure code 5 identifies native backend loss. Port recreation can recover
an input queue/FIFO fault if the reader remains active; restarting a stopped
reader still requires recreating the backend. Runtime backend switching is not
implemented. Provider instructions require consulting failure/activity evidence.

Output retains native event conversion/queue scheduling but now uses the
nonblocking sequencer handle. Output/drain failures become first-error evidence;
retained library/kernel output after EAGAIN, bounded retry and acknowledgment
are not implemented. Subscription refresh queries existing connections instead
of repeatedly subscribing; removal of an absent connection is a no-op. A peer
can disappear between query/mutation. Dispatcher backlog removal is scoped to
that source and does not certify empty upstream kernel/library buffers or remove
externally created connections. Hardware hotplug and physical MIDI remain
unverified. CoreMIDI callback/port-map lifetimes still require implementation.

The conditional MidiAlsaSeqTest suite has eight authored cases/27 executions:
seven native channel-voice rows; twelve malformed/foreign/payload rows; pending
port/client retirement; concurrent reader/map creation/deletion; input loss and
first-error preservation; three timestamp rows; failed native configuration
startup; and one Linux kernel sequencer subscription/input/shutdown case.
Boundary fixtures supply native ALSA event structures and port IDs without a
hardware connection. The kernel case uses actual libasound/sequencer subscriptions
and reader lifecycle, requires /dev/snd/seq and skips if it is absent; no physical
MIDI device is needed. None has run. This suite is additional to the 18-case
portable/WinMM MIDI suite; counts must not be treated as executed evidence.

Source diagnostics pass across 178 changed C++ files and 96 native operations
plus four agent tools. Read-only readiness still finds no native compiler/CMake/
Qt toolchain or LMMS executable, and 17 pinned submodules are uninitialized.
No native compile, Qt case, ALSA kernel/physical device, project recovery,
MIDI/DSP compatibility or CPU/memory/latency trace has executed. The changed
MidiEvent/MidiClient/native component interfaces require a coordinated rebuild;
prebuilt native plugin compatibility is not established. Full autonomous music
production and requirement-by-requirement DAW verification remain unproven.
The complete commission remains active.

ALSA's documented
[event input and FIFO-overrun behavior](https://www.alsa-project.org/alsa-doc/alsa-lib/group___seq_event.html),
[nonblocking and poll interface](https://www.alsa-project.org/alsa-doc/alsa-lib/group___sequencer.html),
and [subscription queries](https://www.alsa-project.org/alsa-doc/alsa-lib/group___seq_subscribe.html)
inform the native API handling. The
[kernel subscription implementation](https://raw.githubusercontent.com/torvalds/linux/master/sound/core/seq/seq_ports.c)
identifies missing subscriptions as ENOENT. These API/source references do not
establish runtime verification on any target machine.

## Native CoreMIDI callback and endpoint management in source

The Apple MIDI path directly invoked processors from CoreMIDI's receive thread,
read two data bytes even for short messages, borrowed stack endpoint addresses,
and iterated unsynchronized subscriptions. Its SysEx continuation indexed an
empty packet and could fail to advance after a zero-length message. Names were
converted through ASCII/undersized buffers and leaked. Output always packed three
bytes and connected output ports as sources. Endpoint notifications logged changes
without maintaining the list; retirement disconnected but never disposed ports.

CoreMIDI now registers live clients in a process-lifetime callback registry.
Client and source callback refCons are monotonic opaque uintptr tokens, never
dereferenced or reused. The registry mutex keeps a looked-up client alive until
the callback finishes registration of its owned native events. Destruction erases
the client token before member destruction or CoreMIDI disposal. Source retirement
erases its token before native disconnect; changed endpoint unique IDs create
new source generations. Renames preserve the live token while canceling/faulting
subscribers to the previous label. Token exhaustion fails closed. Native API
calls hold neither registry nor endpoint-table mutex, so callbacks need not wait
on a table lock held across framework disposal/send calls. Output/API lifetime
has a separate mutex. All native processor/DSP work uses the shared QObject-thread
dispatcher and existing model guard.

Each CoreMIDI source owns a subclass of LMMS's native raw byte parser through a
new protected parsed-event hook. Framing therefore shares the existing voice,
running-status, system-common and realtime behavior. No SysEx message buffer is
allocated. Empty/short/split and variable-length packets inspect only the available
bytes and advance with MIDIPacketNext. Input retains source-specific parser state
and copies its Unicode name before native storage expires. CoreMIDI supplies the
valid packet allocation; the callback API provides no separate allocation byte
size for validating arbitrary forged packet pointers. Callback byte-work, mutex
acquisition, Qt posting and event allocation remain on the high-priority receive
thread; bounded queued delivery does not establish a hard real-time callback bound.

System source/destination enumeration now includes virtual endpoints. Unicode
property conversion owns/releases CF strings and preserves explicit byte lengths.
Legacy unique physical Device:Endpoint labels remain; duplicate names use unique
ID/ref suffixes with collision handling. A unique label can change when another
same-named endpoint appears; saved ambiguous requests are retained instead of
silently redirected. Notifications coalesce a refresh on the client's QObject
thread. Endpoint additions/removals update connections; source loss faults its
subscribers, cancels their queued source data and schedules native all-sound-off.
Other ports continue. Known device I/O errors fault only its subscribed inputs;
unknown device identity uses conservative handling. A faulted port requires
recreation; no live input-reset/backend-switch operation exists yet.

Native output now uses an output port and LMMS's checked serializer, including
two-byte program/channel-pressure packets and 14-bit pitch bend. Native API
creation/connect/send/disconnect/dispose errors retain their first OSStatus;
unknown C++ failures use kMIDIUnknownError. Backend activity describes native
input registration, not audible/connected/healthy hardware. The existing factory
falls back when no I/O port is available and records failed startup evidence.
Partial I/O can remain available. Musical output timestamps and input host-time
mapping remain unimplemented; the legacy MIDI 1 APIs are retained for the existing
platform path, and MIDI 2/UMP/SysEx output is not implemented.

One process-owned MIDI client remains until application termination. Backends
dispose their own input/output ports and remove callback registrations, while
reconstruction can reuse the native service client. This follows Apple's advice
about avoiding explicit disposal of the last client when future client creation
may be required. Native server restart, stale service-client recovery, failed
dispose/resource retention, opaque-pointer token portability and physical hotplug
still need verification. The retained registry/service have process lifetime;
retired source/client map entries are removed, and no per-retired-source tombstone
is kept. This source structure is not a measured CPU/memory guarantee.

The conditional MidiAppleTest suite has twelve authored cases/19 executions:
eight voice/output rows; empty/short/split input; a variable-length list with
ignored SysEx; independent source framing; Unicode/duplicate names; retired
source/client tokens; port/source retirement traffic; coalesced notifications
and I/O errors; source-loss isolation; client-destruction traffic; device-error
isolation; and a native virtual-source roundtrip with backend reconstruction.
Boundary fixtures use real CoreMIDI packet structures without creating native
devices. The virtual case requires a macOS MIDI service, skips if unavailable,
and needs no physical MIDI hardware. None has run; these cases are additional to
the existing portable/WinMM and ALSA suites. Actual output delivery is not proven
by packet construction, and no complete-platform compatibility claim is made.

Source diagnostics pass across 181 changed C++ files, 96 native operations and
four agent tools. Read-only readiness still finds no native compiler/CMake/Qt
toolchain or LMMS executable and 17 uninitialized pinned submodules. No native
compile, Qt case, CoreMIDI/macOS execution, callback race, MIDI/DSP/ABI compatibility,
physical device, CPU/memory/latency trace or complete autonomous DAW workflow has
executed. Native component interfaces require rebuilding together. All six
commission domains and full-system verification remain the goal; completion
remains unproven and the goal stays active.

Apple documents the receive thread in
[MIDIReadProc](https://developer.apple.com/documentation/coremidi/midireadproc),
variable packet storage in
[MIDIPacketList](https://developer.apple.com/documentation/coremidi/midipacketlist),
source identities in
[MIDIPortConnectSource](https://developer.apple.com/documentation/coremidi/midiportconnectsource(_:_:_:)),
and the last-client disposal limitation in
[MIDIClientDispose](https://developer.apple.com/documentation/coremidi/midiclientdispose(_:)).
These API references inform implementation; they do not establish native runtime evidence.

## Native MIDI input recovery without rebuilding the project in source

The bounded input queue previously retained faults until a native port was
recreated. Source now exposes `midi.input.reset` through the same native studio
capability registry. It is transient and preserves the instrument track, MIDI
models, subscription intent, project revision and history. Studio admission
requires stopped transport, disarmed recording/automation, no live/pending/
reserved sample take, and no export/device reservation. Healthy enabled queues
return a no-op result; failed queues use native all-sound-off before resuming.
No separate MIDI controller or replacement project model is introduced.

MidiClient recovery runs on the port's owning thread. It marks the existing
delivery record as recovering, suppresses/counts intake and enters native cleanup
without holding registry/queue locks. It rejects callbacks or recovery already
executing. The record tracks a failure version, latest failure cause and successful
recovery count. Backend/callback failures latch an observation requirement for
the whole fault cycle, even if a local overflow was its first cause. Known stopped
backends and unobserved backend faults cannot reset. Native queue/processor faults
can reset with unknown observation, without claiming healthy hardware. Version,
port/client/processor and backend checks repeat after cleanup. A new failure,
backend stop or exception preserves suspension. Exhausted counters require port
recreation. Native MIDI models are unchanged by these queue/counter fields.

The existing coalesced callback remains the scheduling authority during recovery,
so repeated resets do not accumulate retired callback records. A callback consumed
by a nested event loop during cleanup clears its scheduled flag without entering
the processor. Dispatch tracks processing and completes scheduling under the queue
mutex; intake between an empty-loop check and callback completion is posted rather
than stranded. Recovery completion also schedules pending failure cleanup when a
failure races with the final commit. These are source-level lifetime/scheduling
invariants; no native concurrency execution has verified them.

Inspection reports failure/recovery counters, processing/recovering flags, the
backend-observation requirement and reset eligibility. Successful reset clears
the active failure while retaining cause/version/drop history. Eligibility is
a port/backend snapshot; studio transport admission still applies at invocation.
The legacy restart-required field now distinguishes stopped/unregistered/
unsupported/exhausted state from an eligible local queue fault. Counters and fault
state are transient across native project restoration. Provider instructions
require inspecting reset results and routing afterward.

Runtime request replay returns the persisted reset receipt and cannot clear a
later input fault. A new request ID performs a new recovery attempt. Reset adds
no musical revision/checkpoint. Native all-sound-off can affect other voices or
external MIDI and cannot be rolled back by project history. Cleanup may partially
execute before rejection. Shared raw/source framing and upstream native buffers
retain their own state; physical reconnection, stopped-reader recreation, backend
switching, resource bounds and hard DSP/output/lock deadlines remain unfinished.

Nine additional MIDI cases have 15 authored executions: real raw parser resumption
and serialized model preservation; nested dispatch/cleanup recovery refusal;
new backend failure during cleanup; seven admission rows; cleanup exceptions;
first-local/later-backend fault admission; backend stop; native runtime revision,
durable history, receipt replay and restart; and transport/recording/type refusal.
The portable/WinMM suite now has 27 cases/51 executions with WinMM, 49 without;
ALSA and CoreMIDI suites remain separately conditional. The executable harness
adds healthy no-op reset/replay and on-disk revision/history checks. Tests use
diagnostic input/fault boundaries while native ports/parser/queue/runtime/models
remain authoritative. None of these native executions has run.

Source diagnostics pass across 181 changed C++ files, 97 native operations plus
four agent tools. Read-only readiness still finds no native compiler/CMake/Qt
toolchain or LMMS executable, and 17 pinned submodules remain uninitialized.
No native build, Qt recovery/race case, MIDI/DSP/ABI/physical compatibility,
CPU/memory/latency trace or complete autonomous DAW workflow has executed.
The full six-domain commission remains active and completion remains unproven.

## Native WinMM callback retirement and input observation in source

WinMM still gave the driver a raw MidiWinMM pointer after the earlier port-queue
changes. Native reset/close/output errors were ignored. A failed close or late
callback could retain an address after backend destruction; local recovery had
no observed input state. Current source replaces driver instance data with
monotonically allocated per-input DWORD_PTR tokens. A process-lifetime registry
maps live registrations to client/handle pairs. Its lock covers callback access
through owned event registration. Retirement removes registrations before native
reset/close; handle identity must also match. Token exhaustion fails closed.
Tokens are never dereferenced/reused, and late/reused-handle callbacks are ignored.

Registry/table locks are released before multimedia API calls. The output lifetime
mutex still protects sends against handle close and allows input/loopback callback
access. Constructor exception cleanup retires callbacks; scope guards clean
successfully opened handles before device-map ownership. Capability/open/start/
send/reset/close results retain their first MMRESULT; unexpected C++ callback/
startup failure uses MMSYSERR_ERROR. Native close failure can retain a driver
handle, but its retired token cannot call the destroyed application object.
Resource retention, bounded close retry and driver deadlines remain unresolved.

Observed input activity describes successfully started handles. Unexpected close
removes availability, faults subscribers and queues list publication on the QObject
thread. Invalid-input notifications fault subscribers without inventing a native
API return code. Local reset can clear that fault while input remains active;
stopped input cannot reset. Physical health/connectivity remain unverified. The
native factory falls back if neither input nor output is available; output-only
availability remains separate. MIDI_IO_STATUS enables MOREData, whose complete
message uses the checked voice path. Timestamps, automatic hotplug and SysEx
remain incomplete. Callback mutex/Qt allocation and native driver calls have no
measured hard real-time or resource bounds.

Four additional conditional cases are authored: retired token with reused handle;
callback traffic racing backend destruction and later error callbacks; input-error
recovery plus MOREData delivery; and unexpected close, recovery refusal and
retained missing subscription intent. Diagnostic handles are removed before
native reset/close. None has run. The MIDI suite has 31 cases/55 executions with
WinMM (49 portable, six conditional). Separate ALSA/CoreMIDI suites remain unrun.
These fixtures do not prove real close failures, retained driver resources,
platform startup or physical MIDI.

Source diagnostics pass across 181 changed C++ files and 97 native operations
plus four agent tools. Read-only readiness still reports no compiler/CMake/Qt
toolchain or LMMS executable and 17 uninitialized pinned submodules. No native
compile, Qt callback/recovery execution, driver/ABI/DSP compatibility, physical
device, CPU/memory/latency trace or complete autonomous DAW workflow has run.
All six commission domains remain in scope; the goal remains active and unproven.

Microsoft documents
[input-close failure](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midiinclose),
the [MOREData event](https://learn.microsoft.com/en-us/windows/win32/multimedia/mim-moredata),
and [callback restrictions](https://learn.microsoft.com/en-us/previous-versions/dd798460(v=vs.85)).
These API references guide source handling; they are not native execution evidence.

### Native GIG accepted dependency groups in source

GIG loading now owns a private group rather than opening the mutable original
bank. FileResourceSnapshot captures the root first, preserves its original
filename, then discovers `xfil` filenames and the `doxf` root-stem `.gx99` member
from those accepted bytes. All declared files are captured with byte counts and
SHA-256 values. A missing, mismatched or duplicate companion `dlid` is rejected
before libgig parses the group: upstream libgig advances the declaration's ID
fields only when that companion has `dlid`, so accepting an absent identifier
could shift its parsing onto uncaptured filenames.

The source reader follows libgig's automatic 32/64-bit chunk-length selection at
4 GiB independently for each file and accepts RIFF/RIFX byte order. Numeric ID
fields are normalized before comparisons; root and companions may use different
byte orders. Accepted filenames reject traversal, absolute paths, backslashes,
invalid UTF-8, Windows filename hazards and case aliases. Inspection bounds
include 4096 xfil entries and 100000 top level chunks per file. Non-automatic
offset layouts, non-UTF-8 legacy filenames and incomplete containers are refused.
These checks are format restrictions requiring the native compatibility matrix,
not proof of complete historical GIG coverage.

GigInstance owns the snapshot before its RIFF/gig members, so lazy readers close
before group-directory removal. Native GetFirstSample resolves sample/extension
relationships before publishing the replacement instrument. The serializer binds
the accepted owner to DataFile. Unbound gigplayer resource nodes use the same
capture path during copying, so offline document bundling discovers dependencies.

StudioAssetStore now supports a four-field group reference alongside unchanged
three-field single-file references. The group form is `{path, sha256, bytes,
members}`; each member is `{name, sha256, bytes}`. The first member is the original
root; companions follow in case-sensitive name order. A versioned digest covers
all names, sizes and hashes. Files stage together under a private directory, are
verified and renamed into `assets/<group-digest>/`. Existing groups must pass
full verification and are never replaced. Restore checks layout digest, root
metadata, every member's bytes and canonical directory containment before native
mutation. Bundles reuse this group layout under `resources/<group-digest>/`.
DataFile retains the complete relative root path and applies DOM rewrites only
after all copies succeed. Identical root bytes with different companion bytes
therefore occupy different groups.

Seven new native cases/23 executions are authored across snapshot and asset
tests. They cover mixed byte orders, arbitrary/nested Unicode names, convolution
companions, source deletion, owner cleanup, per-member corruption, missing/unsafe/
colliding/mismatched members, copied-root relocation, same-root different-pool
versions, reuse of groups, both save modes, unbound documents and invalid manifest
layouts. Structural RIFF fixtures contain no playable instruments. All cases are
unrun. Large-file branch execution, real libgig instruments/sample streaming,
PCM/render equality, native session restart/undo, crashes, physical disk behavior
and full GIG compatibility remain required. Source copying is sequential across
members, so it does not provide an atomic multi-file view of concurrently edited
originals. Hard disk/time quotas, native decoder cancellation, filesystem races
and application-thread cleanup latency also remain unmeasured.

Source diagnostics pass across 182 changed C++ files, 97 native operations and
four agent tools. Read-only readiness reports no compiler/CMake/qmake or LMMS
executable and 17 uninitialized pinned submodules. No C++ compilation, Qt case,
libgig load, bundle/checkpoint operation, DSP or complete autonomous DAW workflow
has run. All six commissioning domains remain active; this change is source
progress, not completion evidence.

Format behavior was checked against
[upstream gig.cpp](https://svn.linuxsampler.org/cgi-bin/viewvc.cgi/libgig/trunk/src/gig.cpp)
and [RIFF.cpp](https://download.linuxsampler.org/doc/libgig/api/RIFF_8cpp_source.html).

### Native playable GIG verification paths and streaming changes in source

The preceding structural fixtures cannot establish playable bank recovery.
Three additional StudioRuntime cases/11 executions now use libgig's native
writer to create sample data and instrument/region metadata. Eight bundle rows
cover 16/24-bit mono/stereo PCM in single-file and split banks. The small split
fixture moves the libgig-written wave-pool chunk unchanged into a declared
companion and updates the native wave-table file number. Native GigPlayer must
read that companion, produce finite nonzero PCM with independent stereo channels,
keep playing after the originals are deleted and reproduce the measured PCM
after native bundle reload. This is fixture construction, not a DAW replacement.

Two checkpoint rows exercise removed/replaced originals, same-root banks with
different companion PCM, undo/redo, relocation of the session directory and
reopening through StudioRuntime. Recovered native instruments must preserve the
two sample polarities independently. The external-edit fixture deliberately
leaves the root's optional sample checksum table unchanged; acceptance and
persistence evidence must cover actual companion bytes rather than relying on
that table. A failure case requests an unavailable replacement companion and
checks that revision/history and the prior playable bank survive. Conditional
build registration links libgig only when available; missing native plugins are
explicit skips, not compatibility successes. All 11 executions remain unrun.

The streaming review found variable stack storage, uninitialized short loop-read
tails and signed shifts in 24-bit conversion. GigPlayer now uses one fixed,
aligned, initialized scratch block per read. Loop transfer sizes use native
64-bit frame counts and stop when reads make no progress. A shared decoder reads
native-order 16-bit words with memcpy and assembles/sign-extends little-endian
24-bit values using unsigned arithmetic, retaining the existing half-scale gain.
Synth/note locks are scoped, so native read exceptions release them. Null
dimension-region results are checked before sample access. A stalled resampler
no longer erases other voices already mixed into the period.

Each GigInstance also owns a libgig decompression buffer allocated before
publication and freed by a noncopyable member guard. Every native sample read
receives it explicitly. Different GigPlayers therefore do not use libgig's
shared default decompression buffer for their streaming reads; each instance's
existing synth mutex serializes its voices. This does not prove that all libgig
global state or lifecycle calls are safe under parallel execution.

Nonempty referenced native sample metadata is checked for mono/stereo 16/24-bit frame shape
and nonzero rate before replacing the current bank. Referenced first loops must
have storage, nonzero length and fit inside their sample. These checks prevent
the observed decoder-size/zero-divisor paths; they do not validate every native
sampler field. Three independent PCM cases/13 executions cover signed extrema,
mono duplication/stereo independence, unaligned input, short buffers, unsupported
formats and nonfinite gain. They are registered independently of libgig. None
has compiled or run.

Backward/bidirectional loop semantics, multiple loops, sampler dimensions/layers,
compressed sample fixtures, parallel streaming, one-shot/short-read traces,
engine/render recovery after in-process native exceptions and complete GIG
compatibility remain unfinished or unverified. Hard I/O/DSP timing bounds,
CPU/memory/disk measurements and the full autonomous workflow remain required.

Source diagnostics pass across 185 changed C++ files, 97 native operations and
four agent tools. Read-only readiness still finds no compiler/CMake/qmake or
LMMS executable and 17 uninitialized pinned submodules. No native compilation,
Qt test, libgig fixture creation, sample read, save/restore, DSP or render has
executed. The complete six-domain commission remains active and unproven.
The read/decompression API was checked against
[upstream libgig source](https://svn.linuxsampler.org/cgi-bin/viewvc.cgi/libgig/trunk/src/gig.cpp).

### Native DSP failure containment, observation and explicit retry in source

The GIG changes alone did not make in-process failures recoverable. The native
worker queue previously called `job->process()` without catching a returned
exception or completing its counter. ThreadableJob could stay InProgress, and
the render wait could remain below its submitted count. A nested note job could
also retain its processing lock. These paths now have a terminal-state scope
guard, a scoped NotePlayHandle processing lock and a queue failure latch. Returned
standard/nonstandard C++ exceptions mark jobs done, advance completion and
discard remaining queued work. Overflow ends the rejected job and fails the
period. Already executing work still has to return before the queue can finish.

AudioEngine carries an atomic packed status: the low byte records the latched
first cause; a saturating 56-bit count records failure reports for that engine
lifetime. Setup/instrument/effect/mixer/mix failures, overflow, nonfinite output
and corrupted effects are distinguished. Stage transitions observe queue faults
and skip subsequent DSP stages. A failed period discards mixer state, invalidates
capture continuity and zeros both output pipeline buffers. Subsequent periods
return silence without running DSP or native song setup until explicit retry.
They do not change the Song's playing flag; the controller must stop transport.
The rendering-thread flag is scoped during unwinding.

PlayHandle output, bus buffers, effects and master pipeline output now expose
invalid samples. Bus/effect checks occur before silence/sanitization can conceal
them and also work when optional sanitization is disabled. Native corruption
indicators remain observable in that mode. This does not prove complete coverage
of every plugin-owned buffer or resource field.

`audio.processing.inspect` and `audio.processing.reset` add two native operations
and are transient. Inspection is included at `transport.backend.dsp`. It returns
named first cause, failed state, decimal count, retry eligibility and saturation.
Reset checks engine/application thread ownership, stopped/disarmed transport,
sample/automation recording state, unfinished/reserved takes and export/device
reservations under the model guard. It preserves the report count, clears the
latch and permits the next period to retry; it does not rebuild or repair a
plugin. Output buffers are not rewritten during reset while a live backend may
be copying the silent period. Saturation refuses reset. Saved reset receipts
cannot clear a newer failure or create a project revision.

Playback/resume and render/capture admission reject a latched fault. Capture also
checks while waiting, so a failed accompanying/native job discards the take and
preserves prior PCM. ProjectRenderer snapshots the DSP count and checks failure
before startup, between native periods and before/after codec finalization.
Returned processing-body and cleanup exceptions become failed exports. Studio
export keeps its temporary artifact unpublished and preserves an existing target.
The process harness adds healthy inspection, reset and receipt/revision checks.

Nine new native cases/22 executions are authored: nested standard/nonstandard
throws in static/dynamic queues; concurrent claims; overflow; actual native
periods; bus/master effect failures with sanitization on/off; ownership/playback/
recording/export retry guards; native render target preservation; transient replay
and recording rollback. All are unrun. The two process operations/checks are also
unrun. Fixture processors throw or produce invalid PCM through the real queue,
engine, mixer, recording and renderer code; they do not establish production
plugin, device or timing behavior.

The changes contain returned C++ exceptions and observable invalid audio. They
do not interrupt a blocked plugin, native call or job wait, catch signals/SEH,
repair corrupt memory, guarantee private plugin locks/destructors unwind, or
prove setup/restoration/publication failure recovery. Queue scheduling/reset
races, direct legacy export, GPU/backend/platform behavior, complete plugin
repair, CPU/heap/latency measurements, hard termination/resource bounds and the
complete autonomous production cycle remain unfinished or unverified. Native
components/plugins must be rebuilt against the changed headers; prebuilt ABI
compatibility is unproven.

Source diagnostics pass across 195 changed C++ files, 99 native operations and
four agent tools (103 declarations). Read-only readiness still finds no compiler,
CMake, qmake or LMMS executable and 17 uninitialized pinned submodules. No C++
compilation, Qt case, native process, codec/DSP render, device operation or full
workflow has run. All six commission domains remain active; this is source
progress, not completion evidence.

### Native audio-device restoration and publication in source

The prior DSP containment did not make device setup/restoration reliable.
AudioEngine formerly deleted its current device before publishing the replacement,
could delete/reuse the same pointer, and always restarted its prior backend.
Notification exceptions escaped void restoration calls made from cleanup.
Failed restoration could leave ownership/reservation state unclear.

Device switching is now an owner-thread transition with reentry guards. The
candidate is published only after the current backend's stop returns. An
engine-owned fallback stays scoped before adoption; caller-owned candidates stay
with their caller if stop fails. Same-pointer switching preserves ownership.
Stop/join occurs without the model lock; pointer publication/input invalidation
use the existing model guard. The stored prior device also records its running
state and whether setup touched it. An untouched cancelled reservation is released
without notifications or restart.

Restoration returns bool and catches returned standard/nonstandard C++ failures.
Stop failure retains the current device and prior reservation. After installing
the prior pointer, failed notifications/restart retain that owner and reservation
for retry. Successfully completed notifications are remembered. Restart retries
stop any partially started backend before retrying start. An originally stopped
backend stays stopped. Distinct stored/current devices are both owned for engine
teardown; completed restoration clears that extra ownership.

Native `audio.device.restore` takes no arguments and is transient. It deliberately
executes outside the generic capability model guard so stopping a backend can
join a callback that needs the guard to render. Status is exposed at
`transport.backend.restoration` with pending/restoring/in-progress/failure and
retry eligibility. Remote render execution prevents restoration. The external
operation also refuses an owned export context awaiting finalization unless
restoration has already failed. Renderer finalization rejects a retired file
device. Failed setup becomes an execution failure rather than escaping the GUI/
studio caller; retained ownership stays with the engine when adoption happened.

Studio export checks restoration before publishing. Renderer DSP observation
also compares the live engine epoch after restoration and before commit, so a
new DSP report during rate/backend restoration fails publication. Its temporary
output is closed/removed on the tested returned-failure paths and the prior
destination remains authoritative. Legacy RenderManager retains/reports failed
restoration and can be reused after recovery. Its direct destination writes keep
their existing behavior; this is not atomic legacy export publication.

Eight native cases/16 executions are authored for running/stopped preservation,
same-pointer and cancelled setup, stop joining actual native rendering, returned
standard/nonstandard notifications, stop before/after candidate adoption,
restart status/exception failures, owner/execution/finalization guards and native
studio export preservation after notification exceptions or new DSP reports.
Lifecycle adapters represent no physical driver. Process checks add healthy
no-op restoration, receipt replay and unchanged revision. All are unrun. The
previous DSP fixtures now require owner-thread retirement so their temporary
buses cannot outlive deferred fixture handles; those corrections are also unrun.

Opaque callbacks/destructors, hangs/SEH/signals, native shutdown, driver stop/start
guarantees, port/routing continuity, event-loop reentry, concurrent lifetime and
publication races, cleanup under allocation failure, CPU/heap/latency/disk bounds
and full plugin/platform compatibility remain unfinished or unverified. Device
restore is a control-flow retry, not proof of backend or plugin health. Native
components must be rebuilt against the changed headers.

Source diagnostics pass across 196 changed C++ files, 100 native operations and
four agent tools (104 declarations). Read-only readiness still reports no usable
compiler/CMake/qmake or LMMS binary and 17 uninitialized pinned submodules. No
native compilation, Qt case, renderer/codec operation, device transition or full
autonomous workflow has run. The entire six-domain commission remains active.

### Native audio-worker dispatch and shutdown in source

The previous worker loop waited on a different mutex per worker and had no
shared work predicate. Wakeups could be lost between dispatch and wait. Its
volatile quit flag was not synchronization, quit reset the queue, and completion
counters did not establish that all worker scans had returned before the next
reset changed mode/counters. Engine destruction ignored a 500 ms join timeout.

Worker admission, readiness, quit, pending scans and the active batch now share
one scheduler mutex. Dispatch sets each ready worker's pending predicate before
waking it. The coordinator processes jobs too, including when no background
worker is ready. Completion waits for both job counts and every admitted scan to
return. Reset rejects an active batch; dispatch reentry/overlap and roster
construction during dispatch also reject. Quit cancels a pending admission or
lets an active scan finish, without changing queue contents. Destruction requests
quit and joins before removing the worker. Engine teardown joins without the
discarded timeout; its obsolete unstarted worker object is removed. Negative
reported ideal thread counts are clamped to zero background workers.

The dynamic queue retains its native mixer contract: producers publish child
jobs synchronously while an admitted parent is still outstanding. Publication
reserves an index before storing the pointer; other consumers may pass that slot,
but the parent producer remains responsible for a later scan before completion.
This is not an interface for independent producers that can publish after a batch
has finished. Queue reset/lifetime remain caller-serialized. The global wrapper's
scanner barrier supplies that serialization for engine stages.

The native audio-processing inspector now includes a mutex-coherent worker
snapshot: registered, ready, queued, scanning and active batch. The coordinator
is excluded from worker counts. The process harness checks this schema and
invariants without assuming two observations have identical readiness counts.
No additional operation or project mutation was introduced.

AudioEngineWorkerTest registers nine native cases/15 executions with a 120-second
CTest timeout. Cases cover no workers/unstarted workers/early quit, 256 alternating
static/dynamic batches and empty dispatches, 64 dynamic branching batches,
standard/nonstandard exceptions with drain/retry, active reset/reentry/constructor
refusal, quit during actual processing, destruction joining a controlled active
processor, repeated admission/quit races and repeated pool lifetimes. Gates force
actual worker participation in active-lifecycle cases; cleanup releases them
before joining on assertion failure. The race stress permits either scheduling
outcome and does not prove every interleaving occurred. All cases are authored
and unrun; the test timeout is not a runtime shutdown guarantee.

Nonreturning native processors still block dispatch and joins. Safe bounded
recovery requires further process/lifecycle work. Thread destruction on itself,
overlapping engine lifetimes, allocation failures, opaque plugin behavior,
platform Qt scheduling, sanitizer verification and CPU/heap/latency effects remain
unsupported or unverified. Native components require rebuild against the changed
worker declaration. No runtime performance or safety claim follows from the
source diagnostics.

Source diagnostics pass across 197 changed C++ files, 100 native operations and
four agent tools. Git whitespace checks pass. The read-only build inventory still
finds no compiler, CMake, Ninja, qmake or LMMS executable in its checked locations,
and 17 pinned submodules are uninitialized. No native build, Qt case, audio render,
physical driver or autonomous production run has occurred. The complete six-
domain commission remains active and unverified.

### Native mixer graph validation and complete period dispatch in source

Inspection after the scheduler work found native mixer issues independent of
worker wakeups. Muted senders notified downstream channels while those receivers
could still contain the previous period's mute state (or an uninitialized first
state). Master mute skipped dispatch entirely despite queued buses. Native route
creation only rejected self-sends, while cycle checks were optional in callers
and recursively enumerated paths. XML loading indexed unchecked signed values,
treated comments/unknown nodes as channel zero and could construct cycles.

All channel mute snapshots now precede dependency publication. Muted channels
clear their audio/peaks, become done and notify receivers. The dependency threshold
is exact and queue admission uses an atomic flag. One dispatch drains all jobs,
including unrouted buses and periods with muted master. After a healthy drain,
any unfinished channel reports the new `mixer-dependency` DSP cause and discards
period state. The existing engine latch suppresses output until explicit retry;
job exceptions/overflow retain their earlier causes. This detects incomplete
dependency execution; it does not contain processors that fail to return.

Native create/update route calls now hold the model guard across endpoint,
finite-amount and cycle validation. The check iterates reachable channels with a
visited set, preventing recursive/exponential path traversal. Existing sends
retain their model and update in place. All three ownership lists reserve space
before pointer publication. Removal and send lookup reject out-of-range/foreign
inputs. Ownership lists belong to the actual mixer instance rather than the
global singleton. Newly allocated channel ownership survives vector insertion
failure, and channel admission prevents overflow of the native 16-bit count.

Mixer XML preflight validates all channel/send indices and topologically checks
the complete graph before clearing the current mixer. Forward references and
duplicate send updates retain native behavior. Comments/unrelated elements are
ignored. Invalid indices, master/self sends and cycles report native Song errors.
The studio's existing incomplete-load rejection then restores its committed
checkpoint and advances revision because native object identities changed.
The representable bound is 65,535 channels (indices 0 through 65,534), not a new
claim that the 8,192-entry job queue or available memory can render that many.

MixerProcessingTest adds nine native cases/23 executions with a 120-second CTest
timeout. Tests exercise both channel orders across repeated mute transitions,
master mute and unrouted buses, exact stereo gains and once-per-period effects
through a converging graph, validated route ownership/amounts/cycles, a 48-layer
converging search, ten malformed XML shapes, forward-reference/model round trips,
connected/disconnected live dependency corruption with native engine retry, and
failed studio project load preserving checkpoint history/routing. Controlled
effects count real native processing calls; they represent no plugin compatibility
result. All tests remain uncompiled/unrun.

Further inspection identifies a remaining headless mixer gap: MixerView connects
solo-model changes to Mixer::toggledSolo, so model writes without that view do not
yet apply native solo routing. This must move into the core with persistence and
GUI regression coverage. Other channel lifetime/move mutations, opaque effects,
sample-exact routing automation, corpus compatibility, allocation failure and
measured CPU/memory/latency still require implementation or verification. The
changed mixer/engine headers require native components to be rebuilt.

Source diagnostics pass across 198 changed C++ files and 100 native operations
plus four agent tools; Git whitespace checks pass. Current read-only readiness
again finds no compiler/CMake/Ninja/qmake/LMMS binary in the checked locations and
17 uninitialized pinned submodules. No native compilation, Qt case, mixer render,
platform/device experiment or autonomous production workflow has run. The entire
six-domain commission remains active.

### Native mixer solo, lifecycle and persistence in source

The prior turn identified that MixerView owned the signal connection applying
solo. Native `mixer.edit` changed the BoolModel but had no view to enforce the
mask. Saved mixer XML retained effective mutes/solo flags but not the state to
restore on release. Deleting a selected channel also relied on a GUI reset call.

Mixer now owns solo-model connections when each channel is created. Selecting a
channel captures mute states for every channel, including master; switching
selection keeps that baseline; releasing restores it. Direct core writes and
studio operations share this transition. Suppression guards prevent the mixer's
own derived model writes from recursively selecting channels or replacing mute
snapshots. Explicit mute edits during solo update the restoration baseline,
including unchanged-value edits signaled by the native model. Mask changes remain
protected by the engine model guard.

The old recursive unmute helpers are replaced by iterative ancestor and output
traversals. This retains the native rule of including upstream senders and their
other downstream outputs while not traversing backwards through master. Master
solo selects only master. New routes/channels and removals refresh the mask;
batched reset/load/deletion avoid applying it to partially changed graphs. Move
preserves the selected object, and delete/reset of that object releases solo.
MixerView now only refreshes presentation, and its pre-delete native workaround
is removed. No window or UI automation is needed by the new native tests.

Active solo serializes a per-channel `mutedBeforeSolo` boolean in native XML.
Effective `muted` values and `soloed` remain in their existing models, preserving
explicit overrides through session checkpoints. Load suppresses intermediate
solo reactions until graph/model restoration completes. New snapshots restore
both effective and release states; legacy files without the field use saved mute
values as their release baseline and apply the final selection after routing.
An original mute state absent from a legacy file cannot be reconstructed. The
highest saved solo index wins if several flags are set. Invalid snapshot values
report native load errors before replacing an existing mixer.

Project inspection exposes the selected index, per-channel solo flags and mute
restoration state. MixerSoloTest registers ten cases/19 executions with a 120-
second CTest timeout. Coverage includes master/channel selection with initially
muted/unmuted master, upstream/downstream closure, switching and explicit edits,
route and channel lifecycle, new XML with effective overrides, legacy/multiple
flags, invalid XML preservation, native stereo sample values, and session
undo/redo/rollback/reopen/replay. The process harness adds visible state checks
and checkpoint undo/redo/replay through the native executable protocol. These
are authored tests; none have compiled or run.

The normal graphical interface and its per-model undo journal still need native
regression work; session checkpoint restoration does not prove GUI journal
grouping. Linked/controlled boolean models, sample-exact automation, callback
reentry, exceptions during derived writes, opaque effect behavior, older-version
project compatibility and CPU/memory/latency bounds remain unverified. Native
components need rebuilding after the mixer header/helper changes. The prior
headless solo gap is addressed in source, not yet established by execution.

Source diagnostics pass across 201 changed C++ files, 100 native operations and
four agent tools; Git whitespace checks pass. The current read-only inventory
still finds no compiler/CMake/Ninja/qmake/LMMS binary in the checked locations and
17 uninitialized pinned submodules. No native Qt test, GUI test, audio render,
driver experiment or autonomous production workflow has run. The full six-domain
commission remains active and completion remains unproven.

### Native mixer journal grouping and checked restoration in source

The previous solo transition wrote several ordinary BoolModels. With native
journalling enabled, those derived writes created separate undo records, so one
undo could restore only part of a transition. The session checkpoint path did not
address this native journal behavior.

JournallingObject now supplies overridable journal-target, save and checked-restore
hooks. Ordinary objects retain their existing project serialization contract.
MixerControlModel, used by channel mute/solo controls, saves a compact versioned
snapshot of selected model identity, effective mutes and release baselines. The
initiating control remains the record owner. Core-derived writes suppress their
own records, and same-value mute edits capture changes to release semantics.

Mixer restoration validates every model identity, channel membership, selection
and literal boolean before any mutation. It then updates the existing native
controls under the model guard and solo-update suppression. Moving a channel
preserves identity; adding/removing channels rejects an old group snapshot.
Deleting the initiating control still makes its record dead under the journal's
existing skip policy. No effect chain, volume, route or channel object is recreated
by these records; they do not undo topology changes.

ProjectJournal captures the opposite state and reserves destination stack space
before restoration. A checked rejection or exception during serialization or
restoration retains the pending record. Scoped guards restore the previous global
journalling flag and prevent nested undo/redo, checkpoint insertion or clearing
the journal during restoration. History moves only after successful restoration;
the later Song::setModified notification is outside that guarantee. Arbitrary
callbacks can still leave partially changed objects, and legacy void load methods
cannot report checked failure through the default hook. This is not a general
native transaction rollback mechanism.

MixerSoloTest adds six cases/16 executions: whole solo switching and release with
pointer and unrelated-volume preservation; same-value mute undo; moved identities
and changed membership; malformed compact snapshots; refusal and standard or
nonstandard C++ exceptions in both undo and redo; and nested undo/clear attempts.
Together with the earlier solo tests there are 16 cases/35 authored executions.
All remain uncompiled/unrun. GUI regression, linked/controller models, automation,
effect-private state, reentrant callback mutation and native resource bounds still
need execution and broader review. The virtual interface and model layout changes
require rebuilding native components.

Source diagnostics for this journal change pass across 204 changed C++ files and
100 native operations; Git whitespace checks pass. These are lexical, schema and
registration checks, not native C++ verification. The read-only readiness check
still finds no compiler/CMake/Ninja/qmake or LMMS executable in the checked
locations, with 17 pinned submodules uninitialized. No native test or GUI workflow
has run, and the full commission remains active.

### Session exceptions, rollback and durable agent continuation in source

Native session rollback previously called checkpoint loading without an exception
boundary. A throwing native callback during recovery could escape before the
runtime marked itself stopped; during agent work the outer handler could then
pause the job over an inconsistent native project. Nonstandard C++ exceptions
also bypassed request and agent handlers. Parameter initialization temporarily
disabled local journalling without restoring it when a synchronous callback threw.

StudioRuntime now translates both standard and nonstandard C++ exceptions from
checkpoint validation/loading into checked failures. It retains whether native
loading was attempted, so an undo/redo load failure still triggers recovery of
the previous checkpoint. A failed request discards its tentative manifest updates
before rollback and subsequent bookkeeping. Successful rollback retains the prior
history/cursor, advances revision to invalidate native handles, and persists the
failure receipt with the agent cursor. Dependent pending calls receive failed
outputs and are discarded before the next reasoning round.

If rollback fails, the runtime stops accepting requests and preserves the latest
acknowledged manifest. Diagnostics retain both the original operation error and
the recovery error. Commit and bookkeeping failures mark the session stopped
before rollback, preventing even nested observation requests from committing new
receipts while that failed transaction unwinds. Startup reports native creation
or restoration exceptions without replacing a previously committed manifest.
AutomatableModel::setInitValue now restores its local journalling flag with a
scope guard before emitting the initialization notification, including when its
value-change callback throws.

Agent transition handling catches standard and nonstandard exceptions, persists
elapsed time and a paused status in one update, and preserves an explicit nested
cancellation. A stopped runtime does not publish a resumable job over a failed
transaction. This concerns the application's job state, not the engineering goal.

StudioRecoveryTest registers eight cases/25 executions with a 120-second CTest
timeout. The fixtures invoke production session/model/inference paths, injecting
exceptions through direct synchronous tempo callbacks or the existing inference
transport seam. Coverage includes failed-edit receipts across reopening, failed
rollback during direct and agent work, failed manifest writes with nested requests,
failed undo load and model flags, native undo after failed parameter initialization
with journalling enabled or disabled, new/existing session startup, inference pause or
cancellation followed by resume, and native failure followed by autonomous
replanning and completion using scripted provider decisions. All are authored and
uncompiled/unrun; scripted decisions do not prove real musical reasoning.

These changes contain returned C++ exceptions in the named paths. They do not
prove recovery from process signals/SEH, corrupt memory, nonreturning plugins,
throwing destructors, failure while allocating/reporting errors, arbitrary Qt
event-loop exceptions, or hidden native/plugin state. Native build, execution,
full project/plugin compatibility and autonomous production evidence remain
required for the commission.

Source diagnostics pass across 205 changed C++ files and 100 native operations,
including test registration and Python source checks; Git whitespace checks pass.
The current read-only readiness inventory still finds no compiler, CMake, Ninja,
qmake or native LMMS executable in the checked locations and 17 uninitialized
pinned submodules. These observations provide no C++ compilation or native test
evidence. The complete six-domain transformation remains active and unverified.

### Session request identity, thread entry and native engine ownership in source

Allowed nested controls previously checked only the committed request ledger.
During a native edit, a callback could submit an inspection or cancellation with
the same ID as the still-executing outer request. Both receipts could then be
committed under that ID, producing a manifest that validLedger would reject on
reopening. StudioRuntime now reserves executing IDs separately from retained
receipts. An allowed nested request with that ID receives a transient rejection;
it cannot acquire the pending receipt or cancel/shut down the wrong request.
Scoped release handles success and failure. Distinct-ID nested controls keep
their existing metadata merge behavior and committed receipts.

The boolean request-active marker is replaced by the set of active IDs for busy
checks, agent scheduling and protocol-loop completion. Public open/process/advance
and run entry points reject foreign-thread calls before accessing mutable session
state. The structured rejection omits revision because reading it would itself
violate that boundary. These entry checks do not provide concurrent destruction
or permit raw native model calls from arbitrary threads.

Directory locking also left a separate gap: two StudioRuntime objects with
different directories could both replace LMMS's one global Song/Engine. A
process-wide atomic owner reservation now spans initialization through runtime
destruction. Failed opens release only their own reservation, and destruction of
a rejected runtime cannot release another owner's reservation. Shutdown retains
ownership until the runtime is released. A later session can then open normally.
QLockFile continues protecting each directory across processes. This preserves
the existing single authoritative native project; it does not virtualize or clone
the audio engine for multiple simultaneous sessions.

StudioSessionReentryTest registers five cases/15 executions with a 120-second
CTest timeout: four allowed nested control operations sharing an active edit ID
on successful and failing edits; distinct cancellation/shutdown through native
rollback and restart followed by scripted agent completion; foreign-thread public
entry refusal; second-session rejection during opening, normal operation and
shutdown; and failed-open ownership release without stealing a subsequent owner.
They exercise native setters, receipts, manifests, checkpoints and lifecycle paths.
The tests are authored but uncompiled/unrun. Actual queued protocol I/O timing,
cross-process contention, concurrent destruction, GUI/native callers outside the
runtime boundary and platform memory/race diagnostics remain unverified.

Source diagnostics pass across 206 changed C++ files and 100 native operations;
Git whitespace checks pass. The read-only readiness inventory again finds no
compiler/CMake/Ninja/qmake or native LMMS executable in the checked locations and
17 uninitialized pinned submodules. No C++ compilation, native test or full-system
production workflow is established by these source checks. The full commission
remains active.

### Native inference context compaction and preserved exchanges in source

The active provider input previously grew on every round, including repeated
native observations and tool results. Replanning copied it into inline pastInputs,
so it continued consuming the 16 MiB session manifest budget. The runtime had no
operation for a reasoning provider to continue a long task with a shorter active
exchange while retaining its original transcript for audit.

agent.context.compact now accepts a bounded working summary and must be the final
pending call. Its own tool output is appended before the old exchange is sealed
into a UUID-named JSON context archive. The manifest records the protocol, item
count and SHA-256, then clears only active inference input and stores the summary.
The next request includes original intent and a fresh native observation carrying
the summary, plan and memory. It does not replay the large old chat transcript.
Native project state/history/revision, job identity, completed tool records and
execution budgets remain intact; prior successful calls can still support the
job's declared outcome. A retained direct request receipt replays without creating
another archive.

Provider/revision replanning now archives old input instead of copying it inline.
StudioAgent validation accepts existing inline contexts and new archive references;
inference record ranges are checked against the corresponding item counts. Runtime
opening verifies content, digest, protocol and count before loading the project,
including references inside archived completed jobs. These files retain full
exchanges, not generated summaries substituted for the original evidence. Archive
write failure cannot acknowledge compaction or erase the previous persisted input.

Observations supply the serialized history byte count and an advisory 256 KiB
compaction target; prompts tell the reasoning component to retain intent, musical
choices, unfinished work, resource paths and successful evidence IDs. The target
is not a tokenizer or an enforced provider context limit. Actual summary fidelity
and autonomous compaction decisions still require real model execution.

StudioContextTest adds five cases/15 executions with a 120-second CTest timeout:
chat/Responses continuation with large prior text, preserved call/output pairs and
old evidence, optional restart, archive corruption/deletion/count mismatch in both
current and completed jobs, provider changes and legacy inline compatibility,
receipt replay, failed archive writes, and protection of unexecuted calls. Schema
and process-discovery assertions include the fifth agent tool (105 declarations
with the 100 native operations). All native tests remain uncompiled/unrun.

This only reduces active and prior inference-input storage. Full tool records,
global transcript, request receipts and archive-reference lists still grow in the
manifest; disk quotas/cleanup and native retrieval of older exchanges remain
unfinished. It does not establish unbounded autonomous execution, provider context
fit, complete semantic memory or the full commission's completion.

Source diagnostics pass across 207 changed C++ files, 100 native operations and
five agent tools; Git whitespace checks pass. The current read-only inventory
still finds no compiler, CMake, Ninja, qmake or LMMS executable in the checked
locations, with 17 uninitialized pinned submodules. Native compilation, provider
execution and the five new Qt cases remain unverified. The full goal stays active.

### Native discovery and reading of archived conversations in source

Compaction preserved full earlier exchanges, but a reasoning provider could only
see the working summary and current observation. It could not ask the native
runtime to retrieve a detail omitted from that summary or inspect a prior job.
agent.history.list and agent.history.read now provide that path through the same
LMMS-owned runtime and durable request protocol.

Discovery pages current and archived job metadata, then frozen contexts within a
selected job. The current job's mutable input is excluded; an archived job's final
input becomes its last readable context. List pages contain at most 32 entries,
with bounded intent/summary previews. Reading accepts committed job IDs and
context indices, verifies complete archive digests and structure, and returns
UTF-8 JSON text chunks of at most 64 KiB. Byte offsets and chunk ends respect
character boundaries. Callers can retain the content digest across pages. A
whole current job is rejected because retrieving it would itself add tool output
to that job. Legacy inline contexts remain readable.

Fresh reads validate their selected files; receipt replay intentionally returns
the previously acknowledged response. Reading a context in an old job verifies
the parent archive and selected context. Whole-job reads validate child reference
structure without opening all child files; startup continues to verify every
referenced context before loading the native project. File transfers cooperate
with cancellation between 64 KiB chunks and return no partial result on failure.
Every selected file is scanned and buffered up to the existing 16 MiB cap, so
paging bounds output rather than per-request file I/O or memory.

Five added StudioContextTest cases cover 12 executions: chat/Responses and
file/legacy-inline exact reconstruction, Unicode boundaries, final archived input,
reopen and receipt replay, altered/missing parent and context files, invalid
selection/digest/range arguments, cooperative cancellation, and scripted provider
retrieval of a previous tempo followed by a native edit using that retrieved
value. Together with compaction coverage, the target has ten cases/27 executions.
These tests are authored but uncompiled/unrun; scripted decisions do not establish
real autonomous recall, summary fidelity or musical reasoning.

Schema and process-discovery assertions now cover seven agent tools alongside
100 native operations, for 107 declarations. Source diagnostics pass across 207
changed C++ files and Git whitespace checks pass. The read-only readiness audit
still finds no compiler/CMake/Ninja/qmake or native LMMS executable in the checked
locations and 17 uninitialized pinned submodules. Full tool records, transcript,
receipts and archive references still consume manifest space; archive retention,
disk quotas and actual long-horizon production remain unfinished. The commission
remains active and full-system operation remains unverified.

### Compacted native tool records with preserved evidence in source

Context compaction previously removed old provider input from the manifest while
leaving every tool's complete arguments and response in job.records. Large native
results therefore continued consuming manifest space even after a summary was
accepted, eventually threatening the same 16 MiB commit limit.

New context archives also carry the still-inline full tool records. Once that
archive is published, the manifest replaces each with a compact archivedTool
entry containing its call ID, function name, success flag, context index and
record index. Existing indexes and inference metadata remain in place. Explicit
compaction and provider/revision replanning use the same conversion. Failed
publication retains the acknowledged manifest, full records and pending call;
unreferenced files cannot become authoritative implicitly. Neither arguments nor
results are truncated or replaced by generated summaries.

State validation accepts old full records and the new indexes. Opening current
and archived jobs verifies index identities/outcomes against verified context
content before loading the project. agent.finish revalidates cited archived
evidence, with one verified read per cited context in that request. Failed calls
remain unusable as successful evidence. Full payloads are released after each
context is verified; only its index is retained across citations. Duplicate-call
protection includes archived IDs, including after restart. History discovery
reports each context's recordItems;
agent.history.read accepts an optional record selector with its context index and
returns canonical JSON for that exact record under the existing bounded UTF-8
paging and digest rules. Whole contexts and job archives remain accessible.

Observations and agent inspection now distinguish total retainedRecordBytes from
unarchivedToolRecordBytes. Compaction advice uses active input and unarchived
payload size, avoiding repeated compaction merely because evidence indexes and
inference metadata remain large. The 256 KiB recommendation is still advisory.
Compaction is not automatic size enforcement, a token budget or disk retention.
Active input/results, evidence indexes, inference metadata, transcript, persistent
memory, receipts and archive references can still exhaust the manifest budget.

StudioContextTest adds five cases/18 executions covering exact large payloads
across both protocols and optional restart, bounded retrieval of one record,
successful evidence from archived records, inconsistent indexes/content in current
and archived jobs, fresh missing/altered evidence, rejected failed evidence,
duplicate call IDs after restart, and legacy contexts without record arrays.
The failed-publication case now preserves a real native tempo edit and its full
record, and the scripted previous-tempo workflow follows the compact job index to
its archived record before editing. The target now contains fifteen cases/45
executions. These are authored native tests, not compiled or executed evidence;
real provider continuation and full-system autonomous production remain unproven.

### Large request receipts outside the session manifest in source

The retained 128 request receipts previously stored complete request/response
pairs inline, even after the same tool payload had been moved to a context
archive. Several large observations or history reads could therefore exhaust the
16 MiB manifest budget and stop an otherwise compacted conversation.

Native commit now moves receipts above 8 KiB into immutable UUID-named JSON files
under receipts/. The manifest retains the request ID/hash and a digest/byte-count
reference. Inline payload retention is bounded by 128 times 8 KiB plus reference
overhead; the complete archived receipt is capped at 16 MiB. Receipt files publish
before the manifest, and failures follow the existing stop/rollback path. An
orphan receipt never acknowledges an edit. Large legacy inline receipts migrate
before native project replacement, and all referenced receipts are validated at
startup. Small legacy receipts retain their original inline format.

Replay verifies the full archive and its identities, request digest, response
revision and shape. Failure rejects the read without executing the native action
again. Request IDs are reserved before cooperative receipt reads, preserving
collision protection during nested observation/cancellation/shutdown. Receipt
publication does not process events between file and manifest commit. Read I/O
checks cancellation between 64 KiB chunks, but cannot interrupt blocked operating
system calls. Native results and replayed responses retain their exact content.

StudioReceiptTest is registered with a 120-second timeout and contains nine
cases/22 executions. It covers large request and large response replay in process
and after restart, invalid receipt references/content, fresh replay failures,
direct and agent native rollback when receipt publication fails, receipt files
left unreferenced by a failed manifest commit, legacy migration, nested control
during replay, cancelled reads and the retained-ID expiration boundary. Native
tests are authored but uncompiled/unrun. Source checks do not establish actual
filesystem durability, native rollback correctness or cross-platform behavior.

This reduces manifest pressure from retry receipts while preserving the existing
128-ID deduplication window. Expired and orphan files are not replay sources, but
are not automatically deleted. Total archive storage, active inference input,
transcript, memory and evidence/reference indexes remain bounded engineering
work. Full native build, compatibility, resource-use and autonomous production
verification remain outstanding; the complete commission remains active.

### Archived dialogue and agent-accessible conversation history in source

The global transcript was still retained in full even after provider context,
tool results and large request receipts moved out of the manifest. New commits
now archive older dialogue when the recent transcript exceeds 128 messages or
256 KiB. Immutable chunks hold at most 64 complete messages, with contiguous
absolute offsets, message counts and SHA-256 references. After archiving, the
recent window keeps at most 64 messages and targets 256 KiB, except that its newest
message is always kept intact. Full message fields and ordering are preserved.

Startup validates archive ranges and content before loading the native project.
Legacy inline dialogue migrates before native replacement. File publication and
the manifest retain the existing acknowledgement boundary: failed publication
leaves the committed transcript authoritative, and orphan chunks cannot introduce
a message during recovery. Chunk files are capped at 16 MiB and publication is
synchronous, while reads cooperate with cancellation between file transfers.

conversation.history now reads across archived and recent messages using stable
absolute offsets, returns nextOffset/complete, and targets 1 MiB per page while
allowing one larger intact message. It is also available to the reasoning provider
as conversation_history. New provider contexts include a notice about omitted
archived dialogue; explicit compaction still uses original intent and working
summary. Agent inspection reports both total and archived conversation counts.
Fresh reads validate selected files, while receipt replay returns the earlier
acknowledged page. Eight agent tools now accompany 100 native operations.

StudioTranscriptTest adds seven cases/18 executions with a 120-second timeout:
count/byte retention, exact Unicode message ordering, archived/recent boundaries,
restart, corruption and schema/range inconsistency, fresh reads and replay,
archive-write and migration failures, unreferenced chunks after manifest failure,
cooperative cancellation, and scripted retrieval of an earlier tempo preference
followed by native editing in both provider protocols. The tests remain authored
but uncompiled/unrun. They do not establish real model recall or musical judgment.

Transcript payload retention is now bounded by the recent window and its newest
message, but archive indexes and disk usage still grow. Active provider input,
persistent memory, evidence indexes and other metadata retain separate limits;
very large individual messages still encounter archive/receipt size limits.
Cleanup, quotas, native compilation, compatibility and full production evidence
remain outstanding. The full commission remains active and unverified.

## Carla native host separation (source implementation; native execution pending)

Carla instrument construction retains the native engine and parameter models
without creating a QCompleter. The completer belongs to the parameter view.
Open/save callbacks return the native cancellation value when no LMMS GUI is
available or the callback is off the application thread; explicit project state
remains the headless path for restoring resources. View creation similarly
requires graphical presentation. Headless host-idle callbacks do not dispatch
arbitrary nested Qt events while an instrument is loading or being destroyed.

The host descriptor is zero initialized, missing or incomplete native engines
fail explicitly, and failed construction cleans up an instantiated engine.
Host-owned resource strings survive through native cleanup. Exporting is reported
through is_offline, with OFFLINE_CHANGED dispatched before the next audio block
when that state changes. These paths follow the native callback and dispatcher
contracts in [CarlaNative.h](https://github.com/falkTX/Carla/blob/main/source/includes/CarlaNative.h)
and [CarlaEngineNative.cpp](https://github.com/falkTX/Carla/blob/main/source/backend/engine/CarlaEngineNative.cpp).
The pinned vendored headers and installed system versions still need build checks.

Parameter reloads preserve surviving model objects and resize the model list when
the native count changes. Metadata/range/value refresh suppresses incidental
writeback. Native state loads restore LMMS model identity and controller metadata;
older native states without knob settings keep their restored native values.
An explicit saved knob value is applied after native restoration when different.
Parameter updates from other threads queue onto the instrument's owning thread;
invalid indexes and unavailable parameter metadata cannot access stale models.

The generic AutomatableModel contract now exposes value type, writability and
automation support. Carla provides these from native hints, including integer
choice labels. Studio discovery includes those flags; writes and new automation
bindings reject unsupported controls before modifying them. Plugin inspection
has a native refresh hook. Carla pulls values without constructing an editor and
without replacing the stored manual values of automated/controller-bound models.
It also synchronizes native values before serializing LMMS knob state, preventing
stale uncontrolled knob values from overwriting a newer native state on restore.

Carla state saving records native DataFile serialization errors for unavailable
callbacks, missing/malformed/non-Carla XML, and plugin exceptions. Existing files
and checkpoint publication then use the established DataFile failure boundary.
State loading selects a single CARLA-PROJECT or CARLA-PRESET child regardless of
neighboring LMMS metadata, accepts the native format's case-insensitive names,
and rejects missing or ambiguous states before native destruction can begin.
An exception after native restoration begins latches an error that prevents
saving the uncertain state until a later successful restore. The root formats
come from [CarlaEngine.cpp](https://github.com/falkTX/Carla/blob/main/source/backend/engine/CarlaEngine.cpp).
CarlaEngineNative's setState removes existing plugins and does not return the
loadProjectInternal result through its void native API; structural checks and
exception handling cannot establish all hosted-plugin load outcomes.

InstrumentTrack now uses scoped lock release for direct replacement and both
modern/legacy project restoration. A failed constructor leaves its instrument
pointer null rather than dangling. The existing StudioRuntime transaction owns
restoring the prior project; these loader changes make that recovery path usable
after exceptions. A shared protected construction boundary allows the native
track path to be tested with a deterministic Carla descriptor without changing
the production PluginFactory behavior.

The generic PluginHost interface exposes inspection, addition, removal, renaming,
position swapping and activation through the owning plugin. Its constructor and
destructor are defined in LMMS core so plugins can import that interface across
the Windows library boundary. CarlaPluginHost implements it against the same
native engine already instantiated by CarlaInstrument. A header feature probe
enables management only when the required API is declared; the bridge resolves
runtime symbols from Carla's reported library path before attaching a handle.
Unavailable headers, symbols or attachment leave a concrete management error
without replacing the instrument's engine. The management handle is released
before native engine cleanup, and its library remains loaded through release.

The API contract was checked against the vendored Carla commit
`66afe24a08790732cc17d81d4b846a1e0cfa0118`, including the handle declarations in
[CarlaNativePlugin.h](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/includes/CarlaNativePlugin.h),
the management signatures in
[CarlaHost.h](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/CarlaHost.h),
and the existing-engine attachment and wrapper-only release in
[carla-native-plugin.cpp](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/plugin/carla-native-plugin.cpp).
This source review does not establish successful dynamic loading on any platform.

Six studio capabilities expose this interface on the application thread with
native processing suspended. Hosted plugin indices describe the current order;
structural edits and restore invalidate a caller's remembered indices. Metadata
copies Carla-owned strings before another API call can reuse them. Unique IDs
travel as signed 64-bit decimal strings. Inputs are checked before native writes;
native boolean results, plugin counts and activation readback detect reported
failures. Mutations use the existing StudioRuntime checkpoint boundary because
a failed native call can already have changed the engine. Restoration still has
the void set_state limitation described above.

Three additional studio capabilities directly enumerate, inspect and edit native
parameters inside each hosted plugin. The parameter API is resolved separately;
missing optional symbols disable parameter access while retaining management.
Enumeration and scale-point inspection use bounded pages and preserve fractional
choice values. Metadata copies strings before another native call can reuse
storage, exposes native write/type/range/persistence hints, and rejects malformed
counts, ranges and non-finite values. Direct edits validate writable state,
range and discrete values before reaching the native setter.

The setter's void return cannot establish acceptance. Responses include the
requested value, its native float representation, actual readback and whether
the readback matches the native request. Quantized or ignored writes therefore
remain visible. Exceptions or invalid readback enter the existing failure and
checkpoint recovery path. Tests exercise native snapshot serialization and
restoration after direct edits and after a partially changed setter fails.
They do not prove actual third-party persistence or audio results. Signatures
follow the pinned CarlaHost.h parameter API cited above; the test resolver also
asserts the fixture function types against those declarations at compile time.
The value-save hint excludes disabled controls, output controls and parameters
marked not-saved. The enabled/not-saved filtering follows the pinned
[CarlaPlugin.cpp](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/plugin/CarlaPlugin.cpp)
state serialization path; chunk-based plugins can still own opaque state.

This bridge does not yet provide LMMS automation bindings for every nested
parameter, patchbay connections, host-specific bridge/load options
or hosted asset bundling. A native automatable flag is a capability hint; a native
saved flag cannot establish complete third-party state recovery.

The pinned engine's automation boundary needs an additional extension. Its
[CarlaEngineNative.cpp](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/engine/CarlaEngineNative.cpp)
declares 100 input plus ten output parameter slots and maps flattened plugin
parameters into that fixed window. Its native descriptor setter calls the
plugin's real-time parameter setter. The general management setter instead calls
setParameterValue with UI/OSC notification in CarlaStandalone.cpp; substituting
that call into an audio callback is not an established real-time path. Direct
nested inspection/editing therefore does not establish unlimited automation.

Within the existing native window, bridge add/remove/swap operations now carry a
parameter permutation into CarlaInstrument. The owner defers synchronous native
reload notifications during mutation, remaps existing model objects after native
success, and then refreshes metadata. Data-change callbacks find a model's current
slot instead of capturing the slot at construction. Surviving automation and
controller connections retain their model identities, including save/restore
IDs after unequal-size plugin swaps. Removing a plugin detaches its own model
targets. Failed edits retain the prior model arrangement for native checkpoint
recovery. Layout-count unavailability with bound controls, or moving such a
control beyond the exposed window, rejects before mutation. Native changes to
the window during the edit also report failure for checkpoint recovery.
Structural edits originating directly in the Carla editor or hosted plugins
still need identity reconciliation; this implementation covers bridge mutations.
Its format list describes accepted API requests, not plugin/backend availability.
Actual native library and third-party execution remains necessary.

CarlaHostTest is registered when the Carla host target exists. It compiles the
production host with deterministic external native-library entry points and a
QCoreApplication: 40 cases/163 executions with management, discovery and patchbay
APIs; 39 cases/162 executions with management and patchbay; 37 cases/148 with
management and discovery; 36 cases/147 with management alone; 22 cases/66
executions with the parameter API alone, or 15 cases/49 executions in the older
API branches. Coverage includes both rack and patchbay
callbacks, constructor failures, native state round trips and legacy values,
parameter-count changes, callback lifetime/thread behavior, MIDI offsets, audio
buffers and offline transitions. It also covers existing-file/checkpoint failure
containment, restoration errors and recovery, track lock release, agent parameter
discovery/edits/automation, and fresh native values versus controlled manual
values. Management coverage adds missing symbols and failed attachment, invalid
requests before native writes, partial failures, native snapshot restoration,
metadata storage reuse and handle release before engine destruction. Five nested
parameter cases add 41 executions for optional symbol isolation, metadata and
fractional choice pagination, rejected writes, malformed native metadata, explicit
readback and native state recovery. Five additional layout cases/six executions
exercise native callbacks during unequal-size swaps, removal, append, failed
edits, window/metadata rejection and automation identity after native save/load.
These tests are authored, not compiled or run.
They establish no evidence yet for the actual Carla library, hosted plugins,
graphical regression, sound quality, MIDI output routing or opaque-state assets.

Native hosted-plugin discovery now has a separate CarlaPluginDiscovery lifecycle
inside the instrument's existing bridge. An independent declaration probe and
optional runtime symbol resolution expose start, poll and stop without making
discovery a prerequisite for instrument operation or parameter editing. The
library remains loaded through scanner cleanup, and instrument destruction stops
the scan before releasing the host wrapper and native engine. Calls use the
[pinned CarlaUtils.h discovery API](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/CarlaUtils.h)
and its
[PluginDiscovery.cpp implementation](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/utils/PluginDiscovery.cpp).
Carla owns cached-format enumeration and binary scanner processes.

Callback records are copied before native storage can be reused. Callbacks catch
all exceptions at the ABI boundary and terminate the scan with an explicit error
when copying or validation fails. Exact 64-bit discovery identifiers preserve
their bit pattern in the signed host-load representation. LV2 bundle basenames
are excluded from load paths because the URI identifies the plugin. SFZ labels
carry filenames; JSFX labels are relative to the search root, following
[CarlaJsfxUtils.hpp](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/utils/CarlaJsfxUtils.hpp).
JSFX import-root configuration and opaque dependencies are not solved by scanning.

Results are retained up to a bounded record count and paged without rescanning
finished handles. Polling advances native work and enforces the elapsed deadline
between native calls. Explicit terminal states distinguish completion, manual
stop, timeout, partial results at the cap, native/callback failures and a null
startup handle. A null handle has no detailed native error API, so its response
does not claim that no plugins exist. Scanner completion can include native skips
and is not a compatibility result. Scans are scoped to one live instrument,
without a durable catalog or cross-restart scanner recovery. Native startup,
directory traversal and helper cleanup remain non-preemptible here.

CarlaPluginDiscoveryTest compiles the production discovery component against
deterministic external API functions: eight cases/34 executions with the API,
or one unavailable-API execution without it. Coverage includes missing symbols,
callback storage reuse, exact identifiers, paging, input checks, concurrent-start
rejection, stale IDs, result caps, deadlines, exceptions, cleanup and format-specific
load arguments. One CarlaHostTest execution sends a discovered candidate through
the actual studio capability and native host-add path, then checks scanner release
with instrument destruction. All are authored, not compiled or run. Actual
library/helper/platform compatibility, scanner diagnostics and third-party loading
still need native execution evidence. Current source declares 116 native
capabilities plus eight agent tools; source checks do not prove those behaviors.

## Native Carla internal graph access (source implementation)

CarlaPluginPatchbay attaches to the instrument's existing native host handle. An
independent header probe and optional runtime symbols gate graph availability;
missing symbols do not disable management, parameters or discovery. Carla Rack
explicitly rejects internal graph routing because it uses Carla's serial chain.
Carla Patchbay exposes three studio operations: inspect, connect and disconnect.
All are project mutations, including inspection: the pinned
[PatchbayGraph refresh implementation](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/engine/CarlaEngineGraph.cpp)
calls `removeIllegalConnections()` and rebuilds native connection IDs.

The bridge installs its base engine callback once, before activation and loading
hosted plugins, then copies graph strings only during a synchronous refresh.
An atomic collection flag ends capture on success or failure without replacing
the live native callback slot. LMMS owns this slot; no other LMMS consumer
installs it. An unexpectedly nonempty engine cannot acquire routing observation.
The pinned [native engine callback implementation](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/engine/CarlaEngineNative.cpp)
calls the base observer separately from its editor server and NativeHostDescriptor
notifications. The pinned [base callback setter](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/engine/CarlaEngine.cpp)
writes the callback and context without synchronizing with runner reads. To
avoid changing that slot while hosted plugins run, CarlaInstrument acquires its
Patchbay bridge during construction. Before engine destruction it disables
collection and releases discovery/management handles, retaining the observer and
its library through native cleanup. Carla's cleanup stops its engine runner
before returning; late callbacks see a live, inactive context. Unrelated
callbacks are ignored even on runner threads. Graph
callbacks on a different thread fail the snapshot without touching its containers;
C++ exceptions never cross the observer ABI. Actual library thread/lifetime and
editor behavior remains unverified. Calls require the application thread and
the existing audio-engine change guard; this is not a general concurrent graph
subscription API.

Snapshots identify clients, optional hosted-plugin indices, ports and routes for
audio, MIDI and CV. They reject invalid metadata, duplicate IDs/routes, dangling
references and incompatible connection direction/types, with a 65536-record cap.
Connection edits use current endpoint pairs, resolving native connection IDs
immediately before disconnection. This avoids acting on stale IDs after refresh.
Native acceptance is followed by a fresh graph confirming the desired route;
rejection, exceptions and failed confirmation propagate for StudioRuntime's
checkpoint rollback. Only the internal graph is selected. Port/group IDs still
require rediscovery after native structure changes or restoration. External
hardware routing, GUI concurrency, actual DSP routing and native Carla project
restoration require additional execution evidence.

CarlaPluginPatchbayTest compiles the production adapter with deterministic native
callbacks: ten cases/55 executions with graph headers, or two cases/three
executions without them. Cases cover optional symbols, Rack exclusion, borrowed
strings, audio/MIDI/CV routes, unsigned identifiers, connection-ID renumbering,
invalid requests, malformed/oversized/worker callbacks, native rejection and
exceptions, partial edits, readback disagreement and observer lifetime through
native shutdown. Three CarlaHostTest cases add 15 executions through production
studio dispatch: optional API isolation and preloaded-engine rejection,
argument validation, native-host notifications during refresh, and graph state
passing through the existing native save/restore path after successful and
partial edits. The restore fixture explicitly restores a track checkpoint; it
does not establish end-to-end StudioRuntime rollback with a real Carla engine.
All new native tests are authored, uncompiled and unrun.

## Instrument-generated MIDI in the native routing path (source implementation)

Carla's formerly unsupported `write_midi_event` callback now accepts complete
channel messages through the existing native engine. The pinned
[NativeHostDescriptor ABI](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/includes/CarlaNative.h)
carries a frame offset, port index, size and four inline bytes. The pinned
[native engine](https://github.com/falkTX/Carla/blob/66afe24a08790732cc17d81d4b846a1e0cfa0118/source/backend/engine/CarlaEngineNative.cpp)
emits callbacks during process and skips outgoing MIDI payloads larger than four
bytes. LMMS's native output serializers represent channel voice messages only.
A checked scalar decoder preserves message/channel/data and rejects unsupported
system messages, running status and malformed lengths/data.

Carla captures at most 512 output events in a fixed stack block. A thread-local
capture identifies the currently processing instrument; off-thread, wrong-owner
and out-of-process callbacks cannot mutate it. Bytes are copied during callbacks,
then stably sorted by sample offset without allocation. Dispatch occurs after
native processing and its mutex complete. This avoids reentering the input path
or locking the instrument recursively through an output backend. Failed native
processing clears consumed input and discards all buffered output. Invalid or
overflowing selected output fails the processing block and attempts release of
previously submitted output notes. No exceptions cross the native callback ABI.

MidiPort now owns explicit Track/Instrument output source selection, preserving
Track as the legacy/default path. Its existing filters, fixed key/velocity models
and native client still perform routing. `midi.configure` prevalidates the new
choice against Instrument::supportsMidiOutput; `instrument.inspect` exposes that
capability, and the graphical MIDI panel edits the same saved source. This stops
the track's direct output when the instrument's output is selected, so Carla
passthrough or MIDI effects do not duplicate every sequenced note.

The port retains counts for up to 4096 submitted notes, keyed by actual outgoing
channel/key, plus sustain/sostenuto state. Routing changes and track/port teardown
attempt targeted release before retiring the route. Possible activations survive
native send exceptions; successful releases clear their entries. This bookkeeping
does not establish physical device receipt. New output from either source is
suppressed during offline export; cleanup of earlier live output remains allowed.
Existing MIDI port XML is preserved and missing source attributes restore Track.

Sample offsets reduce to whole MIDI ticks at the existing native client boundary;
ALSA schedules relative ticks while other backends can send immediately. This is
not proof of sample-accurate delivery. Cancellation of pending ALSA events is
implemented below but unrun; already-dispatched events and shared-channel cleanup
interactions remain limitations. SysEx/system support, multiple native
output ports, inter-track MIDI routing and recording generated events into native
editable clips remain incomplete or unverified. GUI behaviour, real MIDI effects,
native backend failure handling, latency and sustained performance still require
execution evidence.

Nine MidiPortTest cases/37 executions and six CarlaHostTest cases/22 executions
are authored for the new path. They exercise real LMMS decoding, source/filter
state, cleanup and studio dispatch with deterministic external native boundaries.
The host fixture covers Rack and Patchbay, callback storage reuse, stable ordering,
absence of dry duplication/feedback, state restore, queue limits, processing
exceptions, off-thread callbacks and output reentry after mutex release. None are
compiled or run. Source validation and registration checks are not execution
evidence; the complete commissioned application remains unverified.

### ALSA scheduled output cancellation and retirement

Source review found that releasing the port's submitted-note ledger did not
cancel future ALSA events. A queued note-on could arrive after cleanup. A queued
note-off also cleared the local ledger before kernel delivery, so simply removing
the queue could discard the only release for an already sounding note.

`MidiClient::cancelOutputEvents` now runs before port output release, even with an
empty ledger. Immediate backends retain their existing behavior. ALSA tags each
channel message with its native source port ID and removes matching pending events
by queue, subscriber destination and tag. This uses the documented
[ALSA event removal API](https://www.alsa-project.org/alsa-doc/alsa-lib/group___seq_event.html).
Inspection of [alsa-lib v1.2.12](https://github.com/alsa-project/alsa-lib/blob/v1.2.12/src/seq/seq.c)
and [Linux v6.6 queue removal](https://github.com/torvalds/linux/blob/v6.6/sound/core/seq/seq_queue.c)
confirmed that the destination condition also selects the queue, while tag matching
distinguishes this client's source ports. The implementation does not cancel the
whole client's queue or use the destination port as a source-port identifier.

ALSA retains a 16-bit possible-activation mask per native output. Submitted
note-offs and pedal-off messages do not clear it prematurely. After removal,
immediate sustain-off, sostenuto-off, all-notes-off and all-sound-off clear each
channel only when the native calls succeed. Channel message submission bypasses
the library output buffer; positive tick offsets still schedule in the kernel,
while zero-delay messages and cleanup bypass the queue. Negative delays,
unavailable native output routes, out-of-range tags and native write/removal errors
fail explicitly. Possible activation survives a native submission failure.

The sequencer mutex serializes submission, cancellation and output ID allocation.
A newly allocated output ID has any old tagged events removed before use; failure
retires that output and records the error. Backend-first shutdown cancels and
releases ports before freeing the queue. Port destructors avoid model-signal
reentry into failed cancellation. Source, disable and unsubscribe failures retain
their prior selection, and channel/key model callbacks restore their prior value
when cleanup fails. Destruction logs failures and remains best effort.

A call-path audit also found that Song stop/pause only requested deferred audio
play-handle clearing. They did not call InstrumentTrack's output cleanup hook.
Song now releases MIDI output explicitly for both its own instruments and those
in PatternStore under the engine model guard, including an already-stopped retry.
Errors on one port do not skip the others or prevent transport from stopping.
The last attempt's bounded error is exposed as `midiOutputCleanupError` in
transport inspection; structured stop/pause report failure when cleanup fails.
Repeated pause also retries while preserving the paused state. The error belongs
to transient native transport state, not project serialization.

Six portable port cases/19 executions and six ALSA cases/22 executions were added.
The ALSA boundary fixture exercises real native event construction, removal
criteria, immediate cleanup and partial-failure retries without a device. Seven
kernel rows use a stopped shared queue and virtual peer to check cancellation,
cleanup delivery, surviving events on another port, routing changes, port ID
reuse and backend-first destruction. Two portable cases/five executions traverse
actual Song/PatternStore instruments through structured stop/pause and verify
failure reporting, continued cleanup on other tracks and retry. MidiPortTest now
has 46 cases/111 executions with WinMM (105 portable). With the clock coverage
below, MidiAlsaSeqTest has 18 cases/66 executions, including ten kernel-dependent
executions. All remain uncompiled and unrun.

Removing queue cells cannot recall an event already dispatched by the kernel or
device; there is no in-flight dispatch fence or physical delivery acknowledgment.
This cleanup also does not gate new live or plugin-generated output after stop.
The conservative channel-mode cleanup can silence another track sharing a
destination/channel. Runtime API errors, latency, CPU/memory bounds and actual hardware behavior
require native execution. Source checks do not establish any of those results.

### Native MIDI musical clock resolution and tempo updates

Inspection of Engine::framesPerTick and the outgoing NotePlayHandle/Carla paths
showed that LMMS supplies delays in its own musical ticks: 192 per default 4/4 bar,
or 48 per quarter note. MidiAlsaSeq configured its queue at 16 PPQ while forwarding
those tick counts unchanged. Consequently, a nonzero queued delay represented
three times its intended duration. The shared `TicksPerQuarterNote` constant now
expresses the native project resolution and drives both Engine's sample conversion
and ALSA queue configuration. It is independent of the selected time signature.

The [ALSA sequencer documentation](https://www.alsa-project.org/alsa-doc/alsa-lib/seq.html)
defines tick rate through PPQ and microseconds per quarter note, and requires PPQ
setup before the timer starts. Inspection of
[Linux v6.6's sequencer timer](https://github.com/torvalds/linux/blob/v6.6/sound/core/seq/seq_timer.c)
also confirmed that changing PPQ on a running timer is rejected while retaining
the same PPQ permits a tempo update. Startup now installs and verifies 48 PPQ
before queue start. Subsequent tempo changes use a synchronous native read/set/read
sequence with unit skew, avoiding a tempo command stranded in the library buffer
after EAGAIN. Readback checks both musical units and tempo before marking the
configuration verified.

The backend caches a successful BPM to avoid per-event configuration ioctls.
A negative native return or mismatched readback invalidates that cache. The next
positive-delay send retries using the current native Song tempo and fails before
submission if verification still fails. Song's direct tempo signal records the
failure without throwing through Qt. Immediate events and cleanup can still be
sent. Already queued events are not retracted by a tempo configuration failure;
explicit transport stop uses the cancellation path above.

`MidiClient::OutputClockStatus` and `midi.inspect.outputClock` expose whether the
backend reports a queue clock, whether its configuration was verified, the last
verified PPQ and microseconds-per-quarter, and the current native clock error.
Unverified initial values are zero; after a later error, the last verified values
remain explicitly unverified. This is a transient diagnostic, not project state,
an external-device acknowledgment, a clock-phase lock or a latency measurement.

Four new ALSA cases/17 executions are authored. Six rows compare the native
configuration and LMMS frame conversion across tempos 10–999, sample rates
22.05–192 kHz and several meters. Eight failure rows cover initial read, write,
readback, ignored setter, wrong tempo/PPQ/skew and zero skew base, including
scheduled-send rejection, immediate cleanup and retry. Another case checks
synchronous tempo changes, cached repeated sends, and recovery from an invalid
tempo. Two kernel rows inspect the actual running queue before/after native Song
tempo changes and observe scheduled delivery through a virtual peer. Their coarse
arrival bound detects accidental immediate dispatch; it does not assert an OS
jitter limit. No new or existing native case has compiled or run in this workspace.

Whole-tick rounding remains at the native outgoing boundary. Integer MIDI tempo
also rounds by less than one microsecond per quarter note. Relative scheduling,
device latency, queue/audio phase, external clock manipulation and physical
delivery are not continuously measured or compensated. The new source removes
the known factor-of-three scale error without claiming sample-accurate output or
complete-system timing verification.

## Native MIDI recording extraction and bounded transaction (2026-10-13)

The recording trace found that conversion from native input-note signals into
editable MIDI notes lived in `PianoRoll`. `MidiNoteRecorder` now owns that state in
the core, and the Piano Roll uses the same builder as the new `midi.record`
transaction. Onset positions come from the native note snapshot, including clip
placement/trim conversion, rather than the timeline position when Qt eventually
delivers the signal. A track reservation replaces the core's Piano Roll recording
query. Its owner identity prevents one recorder from clearing another's state;
the flag lives in `Track` so GUI clip teardown can release it safely after the
derived instrument members have been destroyed.

`StudioMidiRecording` operates native Song/PatternStore/MidiClip playback and
receives `InstrumentTrack` input signals on a dedicated application-thread QObject.
It requires an observed active queued MIDI backend and running audio clock, clears
pre-take input/voices, stages bounded notes, and checks cancellation, clock/input
health and destination/transport identity. Song now exposes processed playback
frames and a playback generation so pattern wraps can complete a timed take while
restarts invalidate it. The private signal receiver prevents queued callbacks from
one take entering another. Its callbacks catch errors and do not pump Qt events:
note-on emission can occur inside NotePlayHandle construction, where a nested
transport stop would be unsafe.

NotePlayHandle accumulates musical duration over processing periods instead of
dividing all elapsed frames by the release-time tempo. The initial buffer offset
is included correctly. Explicit completion snapshots cover held/sustained notes
when a bounded take ends, including after pattern loops. Voice cleanup clears
sustain state and its borrowed handle list before deletion. Note cloning now uses
an owning temporary so failure while copying detuning cannot leak the new note.

Publication prepares deep note copies and a stably sorted pointer vector before
changing the clip, with cooperative checks before publication. Native metadata
signals run after the track lock is released. Cleanup releases the reservation,
stops transport, restores timeline ticks/loop selection and pattern selection,
and reports output-release errors before publication. Existing StudioRuntime
mutation checkpoints provide rollback and durable undo/replay. The current source
declares 117 native operations plus eight agent tools (125 declarations).

`StudioMidiRecordingTest` adds 12 cases / 32 authored executions. They cover shared
note formation and quantization, trim/onset snapshots, tempo/frame offsets,
clip/song/pattern capture, held and sustained notes, cancellation/timeout/input/
DSP/output failures, note budgets, target removal and transport restarts, queue
discard, reservation ownership, detached detuning, publication cancellation, and
checkpoint undo/redo/restart/replay. The tests use production native models,
byte decoding, queued input and engine periods with fixture device boundaries.
They have not compiled or run; this is source work, not device evidence.

The operation has polling and whole-tick precision, retains LMMS's key-based
overlap semantics, and does not yet provide CC/program/SysEx recording,
plugin-generated MIDI recording or device latency
compensation. Source diagnostics pass across 246 changed C++ files; they check
delimiters, whitespace, Python syntax, capability declarations and build/test
registration, and do not compile C++. Native compilation, GUI regression checks, physical MIDI/audio
capture, musical-quality evaluation, and complete-system verification remain
outstanding. The full DAW transformation remains active and incomplete.

## Editable MIDI pitch performance (2026-10-13)

The next recording trace found a broken prerequisite in the old pitch path:
`NotePlayHandle::processTimePos` tried to record track pitch into a note detuning
curve but returned immediately for new live MIDI notes, whose detuning was null.
Its timeline subtraction would also have moved backwards at pattern wraps.

Native input now emits pitch snapshots at note onset and for each active input
note when a MIDI bend changes the track pitch. Each snapshot carries the native
note's elapsed musical duration. `MidiNoteRecorder` owns fresh detuning curves on
the model thread; it never creates or changes a live voice's detuning for capture.
That keeps the monitor path from applying both global pitch and captured detuning.
Existing per-note detuning playback still uses the native NotePlayHandle path,
whose obsolete GUI recording parameters have been removed.

The curve builder retains the starting track pitch as its reference, adds
discrete native automation nodes only when the value changes, replaces same-tick
updates, and enforces a total node budget. `midi.record` enables this by default,
with `recordPitch` and `maximumPitchPoints` controls and corresponding result
metadata. Capture cleanup restores the track's original pitch before publishing.
The Piano Roll consumes the same native snapshots and curve builder, restores
its original independent pitch model, and reports a bounded-recording failure
without stopping transport inside a note-construction signal.

Current recording cannot combine these note curves faithfully with a separately
automated, controlled, or linked track pitch baseline. Headless pitch capture
rejects those bindings explicitly; note-only recording remains selectable.
The Piano Roll keeps note-only capture for such a binding. Native detuning's
60-semitone magnitude also bounds the relative bend; an extreme change across
a nonzero starting pitch fails instead of being clipped. These are remaining
capability gaps, not a reduction of the final application scope. CC/program/SysEx
lanes, channel-independent same-key voices, plugin-generated MIDI recording,
device timestamps and latency compensation still require work.

Five cases / 14 additional executions are authored for curve ownership, elapsed
time and replacement, limits/nonfinite input, chord/sustain/pattern-wrap pitch
capture, native playback frequency, pitch restoration, and explicit note-only
capture with automation or linked models. The existing durable-history case now
checks detuning after undo/redo/restart/replay. Timer fixtures wait for native
recording to start, so checkpoint I/O cannot consume the test performance early.
The MIDI transaction target now contains 17 cases / 46 authored executions.

Source diagnostics pass across 246 changed C++ files and 117 native operations.
No native C++ test, GUI check, physical-device capture, ABI check or complete-system
music-production workflow has run. The previously observed missing toolchain and
17 uninitialized pinned submodules still prevent native verification here.

## Editable native automation performance (2026-10-13)

`automation.record` now captures native parameter/controller performance into an
existing arrangement or PatternStore automation clip. It uses Song's automation
sampling path and AutomationClip's native nodes, scale conversion, interpolation
and overdub behavior. The agent catalog now declares 118 native operations plus
eight agent tools (126 declarations).

AutomationClip can reserve a detached native take, enforce a total point budget,
latch recording failures and publish a fully prepared curve. Existing nodes are
copied and rebound to the detached clip, then rebound to the destination before
the final map swap. No live curve points change during capture. Cancellation or
failure discards the take; a first zero-valued sample is retained even when the
curve was empty. Recording uses an explicit source-tick quantization setting and
keeps LMMS's native rule for removing conflicting nodes during a constant value.

The recording trace also exposed a native pattern-coordinate defect: selected
automation clips after pattern zero were still filtered and offset by their
editor placement. Native automation playback and Song recording now use local
ticks for the selected PatternStore pattern. Muted automation tracks/clips and
unavailable bindings are excluded. The primary model enables controller values
before sampling, including on the first recording tick.

The bounded operation validates the target geometry and bindings, linked
controller identities, registered MIDI delivery health when relevant, DSP state,
audio-device clock, transport generation, arm state, cancellation and timeout.
Completion uses processed frames, or a song clip's observed end, with 10 ms
control-thread polling. Cleanup stops playback, releases the arm state and
restores prior timeline/pattern selection and controller-use state before
publication. Other capture and rendering paths recognize the reservation.
The existing mutation runtime supplies checkpoint rollback and durable undo,
redo, recovery and receipt replay.

Ten native cases / 24 executions are authored for detached ownership and
publication, failed/empty/cancelled staging, trimmed and quantized source ticks,
logarithmic values, second-pattern capture/playback, MIDI controller first-sample
and input-loss behavior, native failure cleanup, clip-end completion, competing
capture/render reservations and session rollback/history/replay. The tests use
native engine periods with fixture device boundaries and have not compiled or run.

This operation samples only the first connected parameter at musical ticks and
retains existing curve nodes outside its native overdub edits. The point budget
includes retained nodes. It does not capture hidden global curves, simultaneous
targets, raw CC/program/SysEx packets or hardware timestamps. Controller-driven
parameter recording is now implemented in source; complete MIDI expression,
physical timing and full-system musical verification remain outstanding. The
previously observed missing native toolchain and 17 uninitialized pinned
submodules still prevent compiling or executing the new tests here. The full DAW
transformation remains active and incomplete.

Source diagnostics passed across 250 changed C++ files and 118 native operation
declarations; `git diff --check` also passed. These checks cover delimiters,
whitespace, Python syntax and build/catalog/test declarations, not C++ compilation
or native execution.

## Conversational MIDI controller setup and persistence (2026-10-13)

The automation-recording trace exposed a missing native interface: the studio
could connect shared LFO/peak controllers and record a pre-existing MIDI
controller, but could not create or route the connection-owned MIDI controller
used by LMMS's graphical connection dialog. `controller.midi.connect` now creates
that native object for an automatable parameter, with a validated CC number,
channel and input-enabled state. The controller belongs to ControllerConnection,
does not enter the Song rack, and is retired when replaced or disconnected.
Existing manual parameter values are retained. Linear/log mapping and native
CC interpolation remain in AutomatableModel/MidiController.

The existing `midi.inspect`, `midi.configure`, `midi.subscribe` and
`midi.input.reset` operations now select either an instrument track or a parameter
with a direct MIDI connection. Ambiguous selectors and controller output/voice
settings are rejected. Parameter metadata exposes the MIDI selector and null rack
index. Named endpoint discovery, unavailable subscription intent and queued input
health use the same MidiPort/MidiClient machinery as instrument routing. The
agent catalog now declares 119 native operations plus eight agent tools
(127 declarations).

Input-loss cleanup previously sent synthetic CC 120 to every event processor,
which could alter a parameter actually bound to CC 120. MidiEventProcessor now
states whether it needs note cleanup. The default preserves instrument voice
cleanup; MidiController retains its last received value while the queue failure
remains observable and requires explicit recovery. This virtual-interface change
requires rebuilding native components; ABI compatibility has not been tested.

Persistence review also found that the GUI's discard-MIDI save preference applied
to recovery checkpoints. Song now marks checkpoint serialization with a scoped
flag. AutomatableModel and InstrumentTrack preserve controller connections and
instrument-port state in those checkpoints regardless of the sharing/export
preference. Ordinary native project saving still honors the preference, which is
not modified by checkpoint creation. Received controller values and delivery
queues/counters remain transient native state rather than saved performances.

`StudioMidiControllerTest` adds seven cases / 27 authored executions covering
linear/log CC input, omni/specific channel filtering, invalid settings, selector
and field admission, MIDI/rack ownership during rewiring, named subscriptions,
queue/backend failure and recovery, ordinary XML export options, checkpoint
rollback, undo/redo, reopening and receipt replay. The existing three automation
recording controller rows now use the new structured connection/configuration
operations. The real-process harness adds setup, discovery, configuration,
invalid-output rollback, history/replay and native XML checks. None has compiled
or run; fixture boundaries do not prove physical delivery.

Source diagnostics and `git diff --check` pass across 252 changed C++ files and
119 native declarations. A fresh read-only build inventory again found no
CMake/Ninja/compiler/qmake, no candidate tools in its targeted installation
locations, 17 uninitialized pinned submodules and no native LMMS executable.
The inventory is not an exhaustive machine search. Native compilation, GUI
regression, plugin ABI/compatibility, physical-device timing and a complete
conversational music-production workflow remain unverified. CC support is native
7-bit input through direct parameter bindings; MIDI learn, full expression/event
lanes, hardware timestamps and the remaining application scope still require
work. The full DAW transformation remains active and incomplete.

## Native conversational MIDI learn (2026-10-13)

The remaining MIDI-learn detector lived inside ControllerConnectionDialog.
AutoDetectMidiController now belongs to the native core and is shared by the
dialog and `controller.midi.learn`. The GUI keeps its latest matching event
behavior; bounded studio learning latches the first valid matching CC. The
dialog now accepts CC 0, which its previous positive-only selection condition
rejected. Detection validates the original event fields because the native CC
number accessor masks values to seven bits. Reset clears stale detection state.
These GUI changes have not been exercised.

StudioMidiLearn uses a detached detector, native MidiPort subscriptions and the
existing queued input path. Admission requires the initialized application
thread, stopped/disarmed transport, no capture or audio-device reservation, and
observed active queued MIDI input. It does not require an audio clock. Optional
channel filtering and an exact named input restrict detection; omitted named
selection listens to inputs available at admission. Named publication requires
a reported source among those candidates that remains available. Raw backends
have no named selection.

The prior parameter connection stays in place during detection and can still
consume input. The prepared replacement uses the same connection-owned native
MidiController as explicit CC setup, preserving the manual parameter value and
seeding the live controller buffer with the detected CC value. Target/binding
identity, scale/range, transport, backend, port and queued-delivery health are
checked before publication. Failure, timeout or cancellation retires the detector
and any prepared connection. A scoped application-thread guard rejects nested
learning. Existing session busy-operation rules provide transaction exclusivity;
the detector is not a global reservation for arbitrary external native code.

The operation uses cooperative 10 ms event-loop polling and a 1..600000 ms
timeout (default 30000). Resource/cancellation checks cannot preempt blocked
native callbacks. Source identity and input-health observations do not prove
hardware delivery or provide input timestamps. Recovery checkpoints, undo/redo
and receipt replay preserve the learned binding without starting another learn.
The captured value and detector are transient: restored controllers start at
native zero until new input arrives. The catalog now declares 120 native
operations plus eight agent tools (128 declarations).

`StudioMidiLearnTest` adds eight cases / 32 authored executions for malformed
events, shared first/latest behavior, raw/named filtering, first-event retention,
prior binding preservation, timeouts, cancellation before publication, input
loss/overflow, target/binding/transport changes, ambiguous or lost source names,
admission validation, reentry and session rollback/history/recovery/replay. The
fixture replaces the native MIDI client while retaining production port
registration and queued dispatch; it provides no hardware evidence. The native
process harness adds inactive-input refusal, preserved binding/history and cached
failure replay. None of these tests has compiled or run.

The recorded build inventory still lacks a native C++/Qt build toolchain, has
17 uninitialized pinned submodules and finds no LMMS executable. Native execution,
GUI regression, backend integration, plugin compatibility and full conversational
music-production evidence remain outstanding. The full application transformation
remains active and incomplete.

Source diagnostics pass across 258 changed C++ files and 120 native declarations;
`git diff --check` also passes. These checks cover delimiters, whitespace, Python
syntax and build/catalog/test declarations. They do not compile C++, execute the
new tests or establish GUI, device or full-system correctness.

## Native track ordering and pattern ownership (2026-10-13)

The application coverage review found that the studio had no native track move
capability. TrackContainer merely erased and inserted pointers without validating
ownership or bounds. TrackContainerView separately swapped pattern ownership
between the source and destination. That endpoint swap failed to rotate all
intervening patterns in a longer move, and direct native moves omitted pattern
mapping entirely. Since project loading assigns pattern indices in track order,
an inconsistent mapping also risks changing which content belongs to a pattern
after saving and reopening.

TrackContainer now owns reordering. It validates the target row and container,
rejects foreign-thread/reentrant calls, prevalidates pattern slots, and applies
the change under the native audio-engine model guard. A bounded vector rotation
changes row order; existing PatternTrack/PatternStore swaps rotate every crossed
pattern column. Clips and parameter/controller/automation objects retain their
identities, and the current pattern selection follows its original content. The
track-vector write lock is released before clip and final track notifications;
the engine model guard stays held through the operation. Normal
row-based automation precedence remains effective, so reordering competing
automation rows can intentionally change playback.

`track.edit` exposes optional final zero-based `to` destinations in song and pattern
containers alongside rename/mute/solo, with combined-field prevalidation. Its
response reports the previous and resulting row, whether it moved, and the
new pattern index where applicable. Project inspection reports each song pattern
track's native column index. Invalid/sparse pattern layouts reject before reorder.
The existing runtime supplies durable mutation history, rollback and receipt
replay. Native track/column serialization remains authoritative; no alternate
ordering manifest or copied composition model was introduced. The catalog now
declares 120 native operations plus eight agent tools (128 declarations), with
the existing track-edit function carrying the additional native behavior.

TrackContainerView no longer performs pattern remapping. It observes native
`trackMoved` and synchronizes existing views, then reconciles queued additions
against native order. This also lets direct core edits update an existing GUI.
The TrackContainer return type and instance layout changed, requiring a full
native rebuild; ABI and GUI compatibility are unverified. Selection is preserved
while moving, without changing existing project-reload selection semantics.

`StudioTrackMoveTest` adds nine cases / 30 authored executions for song and pattern
row moves, first/middle/last pattern rotations, interleaved ordinary tracks,
non-4/4 meter, retained clip/model/curve ownership, validation, sparse stores,
thread affinity and reentry. Native engine periods exercise selected-pattern
automation and competing-row precedence. Runtime cases cover checkpoint rollback,
undo/redo, cached replies after undo, ordinary project save/load, reopening and
receipt replay. The process harness adds a three-pattern move and checks each
owner's note data, inspection metadata, XML order, refusal and history. None has
compiled or run; fixture assertions are authored evidence requirements, not
verified audio or compatibility results.

The recorded native toolchain/dependency blocker remains unresolved. Compilation,
GUI regression, full native rendering, real inference/listening, compatibility
and resource-use evidence remain required for the complete commission. The full
DAW transformation remains active and incomplete.

Source diagnostics and `git diff --check` pass across 262 changed C++ files and
120 native operation declarations. The diagnostics check delimiters, whitespace,
Python syntax and build/catalog/test registration; they do not compile or execute
native code. The 30 new executions remain authored and unrun.

## Pattern lifecycle, local automation time and durable selection (2026-10-13)

Continuing the native pattern trace found coordinate inconsistencies beyond track
reordering. PatternStore removed columns by passing a fixed 192-tick threshold to
Track::removeBar, while the latter shifted clips by the current meter's bar size.
With shorter bars, late columns could fail to shift; with wider bars, earlier
columns could shift too. Pattern-change notification likewise divided clip time
by 192 instead of using the native clip-to-column relationship. Meter changes
updated note/sample lengths but did not reposition the pattern columns in core.

Removal now deletes only an existing target clip and assigns each surviving clip
its native index position under the engine guard. Sparse rows no longer create a
clip merely to delete it. Notifications find the clip's owned row index, and the
PatternStore connects to Song's meter change to apply the existing position repair
path without needing an editor. Native clip and automation target identities are
preserved throughout these changes.

The trace also exposed a regression in the earlier automation-recording change:
TrackContainer now evaluates selected pattern columns in local time, but
PatternStore still added the editor column offset before calling it. Constant
curves could not expose that error. PatternStore now forwards local time directly.
Arrangement pattern automation also incorporates the signed source offset and
positive wrapping used by native pattern playback. Regression fixtures contain
changing curves and check early, middle and later local ticks in nonzero columns,
including actual native engine periods and signed offsets across loop boundaries.

Selection persistence was another gap in continued editing: PatternTrack
construction selected each newly loaded pattern, leaving the final one selected.
Song now writes a native `currentpattern` header attribute and applies it after
all tracks and columns load. The same native serializer handles recovery
checkpoints. Absent legacy attributes retain prior constructor behavior;
malformed/out-of-range values select zero. This adds checkpointed editor context
without claiming to restore live playback or unsaved transport changes.

`StudioPatternTest` adds nine cases / 31 authored executions covering those paths,
meter changes across 3/4, 7/4, 7/8 and 5/16, deletion before/after/at selection,
sparse-row cleanup, owner notifications, legacy and invalid XML, project round
trips, reorder/delete history, cached replies after undo, and reopening. The
existing native process harness now verifies selected-pattern identity across
move, history, refused-edit rollback and XML save. No native case or process
harness has compiled or run. The recorded build toolchain and pinned dependency
blocker remains unresolved; GUI behavior, rendered audio, broad project/plugin
compatibility and the full conversational production cycle still require evidence.
The full DAW transformation remains active and incomplete.

Source diagnostics pass across 264 changed C++ files and the existing 120 native
operation declarations; `git diff --check` also passes. These are delimiter,
whitespace, Python syntax and registration checks, not C++ compilation or native
test results. The 31 new executions remain unrun.

## Destination-aware native clip copies (2026-10-13)

The arrangement trace found that the studio could clone only on the source track,
whereas native GUI paste could recreate clip state on a compatible destination.
Changing a clip's base owner alone would leave derived state attached to its old
instrument/automation track and sample mute signals. Recreating through XML would
also reopen named sample assets and lose resident audio if that original path
were unavailable.

`Clip::copyTo` now constructs the appropriate native clip with its destination
owner from the outset. MIDI copies deep-copy notes and detuning, retain native
step/type state, and restore exact source geometry after initialization; temporary
note ownership covers initialization failures. Sample copies share immutable PCM
while retaining independent playback properties and connecting to the destination
track. Automation copies retain native target bindings and rebind each copied
node's owner so later edits update the new curve. PatternClip copies naturally
refer to the destination PatternTrack's pattern index. Existing `clone()` methods
continue using the same derived native copy constructors on their original owner.

The existing `clip.clone` capability now accepts destination selectors. Song
destinations append at a required position with a checked end bound. PatternStore
destinations require explicit replacement of a different existing cell; the core
constructs a copy under the engine guard, swaps it into the owned column, removes
the old cell and assigns the current-meter column position. Other columns and
selection retain their identities. Inconsistent destination rows, incompatible
types, foreign-thread calls and source/replacement recording conflicts are
rejected. This adds no operation declaration: the catalog remains 120 native plus
eight agent tools. The GUI XML clipboard implementation is unchanged.

`StudioClipCopyTest` adds ten cases / 40 authored executions. They cover independent
note detuning and automation node ownership, original target bindings after
source deletion, in-memory PCM and missing original sample paths, source windows,
destination mute behavior, pattern reference semantics, same/other pattern rows
in 7/8, invalid selectors and geometry, sparse columns, recording reservations,
thread affinity, ordinary native XML, history, replay after undo and reopening.
The process harness adds song/pattern copies and checks content, source
independence, history, refusal and XML. Its sample fixture additionally renders
copied split clips on another track and compares independently decoded PCM.

All native cases and process comparisons remain unrun. The previously recorded
compiler, Qt/build tooling and pinned dependency blocker persists. No rendered
equivalence, GUI regression, hardware/plugin compatibility, full inference and
listening cycle, or CPU/RSS result is established by these changes. The full DAW
transformation remains active and incomplete.

Source diagnostics pass across 266 changed C++ files and 120 native operation
declarations; `git diff --check` also passes. The diagnostics cover delimiters,
whitespace, Python syntax, catalog consistency and test registration. They do not
compile or execute LMMS. The 40 new native executions remain unrun.

## Native track cloning, model identity and construction cleanup (2026-10-13)

The next duplication trace found three native risks. `Track::clone` serialized
all clips and reconstructed them from XML, reopening named samples even when
their accepted audio remained in memory. Restoring duplicated parameter settings
could adopt the source's serialized ID; a fresh source uses a high-bit in-memory
ID while saving clears that bit, allowing two live models to serialize the same
identity. Both Track factory overloads also manually paired model locks, so an
exception during construction or restoration could leave the engine locked and a
partially constructed track registered.

Track cloning now serializes native track/plugin settings separately and copies
clips through `Clip::copyTo`. PatternTrack duplication copies the entire owned
column directly and then retains its copied arrangement markers on the new
pattern. PatternStore row cloning replaces the default cells in each existing
column. This retains resident SampleClip audio, current automation model bindings
and independently editable notes/curves. Source capture state and pattern layout
are checked before constructing the duplicate. Pattern copies check recording in
every affected row/column, since SampleTrack::updateClips retires playback handles
across its track. Clip copies similarly reject recording in other destination
sample clips; two additional regression rows extend that suite to 42 executions.

`JournallingObject::KeepIdentityScope` keeps newly allocated model identities
during duplicate-settings restoration and restores ordinary ID-loading behavior
on every exit. ProjectJournal's allocator now reserves both loaded and fresh
forms of each serialized identity. Native project loading outside this scope
continues to restore IDs normally. Automation-identity tests exercise both fresh
and previously loaded source tracks, then bind each copy separately and reload
the native project to verify target separation.

Track factories now hold their model lock and constructed track through RAII.
PatternTrack construction records each row's prior column count and removes
appended cells, its pattern-map entry and its container entry if construction
throws. Clone failures remove their duplicate and restore prior pattern selection;
native errors or serializer diagnostics reject incomplete settings. The GUI clone
action checks failure before creating a view. Studio mutations continue using
their existing native checkpoint/history/rollback machinery.

FileResourceIO now provides an event-suspension scope that retains cancellation
and budget checks while preventing its nested asset loaders from pumping Qt
events during the clone transaction. Track cloning checks the active cooperative
budget between clips/rows and before publication. This is not OS preemption, and
does not bound the execution time of one arbitrary plugin callback or large clip
copy. Source code cannot establish CPU/memory behavior without native execution.

`StudioTrackCloneTest` adds 14 cases / 33 authored executions: song/pattern row
copies for each clip type, whole pattern copies in 4/4 and 7/8, resident audio with
an absent original path, fresh/loaded automation identities, MIDI port and owned
controller copies, recording and sparse-layout refusal, partial-copy cancellation,
serializer errors/exceptions, failed factory restoration, failed construction
before/after cell allocation, model-lock release from another thread, nested ID
scope unwinding, loaded-ID allocation collisions, XML/history/replay and reopening.
The process harness adds bundled instrument and pattern duplication, distinct
saved automation targets, project reload and history/receipt behavior. None of
these native checks has compiled or run.

This step exposed a separate resource gap: plugin settings used their native file
loaders, so an absent original caused clone refusal and changed original bytes
could yield a different duplicate. The following resource-cloning step addresses
the registered built-in loaders. GUI regression, wider plugin compatibility,
native audio results, the full inference/listening/revision cycle, and CPU/RSS
evidence remain required. The
recorded native toolchain/dependency blocker remains unresolved. The full DAW
transformation remains active and incomplete.

Source diagnostics pass across 270 changed C++ files and 120 native operation
declarations, and `git diff --check` passes. These checks cover source delimiters,
whitespace, Python syntax and build/catalog/test registration; they do not compile
or execute LMMS. The 33 new executions and two added clip-copy regression rows
remain authored evidence requirements.

## Accepted native instrument resources during cloning (2026-10-13)

Native track cloning now collects the source instrument's accepted resource
registrations into its temporary DataFile and activates them while restoring the
duplicate's settings. Each registration belongs to an exact DOM element and
attribute and retains the declared path. A changed reference is rejected. Nested
loads mask and restore the current reference, and the context stays on the calling
thread. Two instruments, or multiple oscillator waveforms, can therefore retain
different accepted versions of the same original filename.

SampleBuffer registration optionally retains its owning shared pointer for this
short-lived clone document. AudioFileProcessor, SlicerT and TripleOscillator reuse
that immutable decoded buffer, preserving PCM and sample rate without decoding or
allocating another audio buffer. Native SampleClip and LFO settings participate in
the same read mechanism. Ordinary resource exports continue retaining accepted
file snapshots without pinning the decoded sample buffers through this option.

GUS, SoundFont and GIG restoration creates independently owned snapshots from the
accepted input, then runs the native plugin loader. GIG copies preserve the root
filename, sample pools and declared convolution companions. Accepted inputs are
verified before use; a new bank copy is checked against the original accepted
fingerprint before publication. Bank ownership stays independent because a player
may explicitly retire its files after native readers unload them. Original paths
remain metadata and never become private temporary filenames in ordinary saves.

Missing or changed original files no longer cause these built-in loaders to choose
different clone input. Missing or altered accepted files, a changed serialized
reference, or cancellation during verification rejects the clone. Existing native
RAII cleanup removes the partial track; the studio keeps its normal transactional
checkpoint behavior. Cooperative file checks and clone event suppression remain
active. This does not preempt arbitrary plugin execution, and independent bank
loads still cost disk space and native engine memory.

Nine additional cases / 26 executions are authored: exact node/attribute/document
and thread isolation; nested and exception scope restoration; changed DOM or
accepted input; cancellation; independent single-bank and complete GIG ownership;
late GIG companion tampering; optional resident-buffer retention; two versions of
one source for AFP, SlicerT, TripleOscillator, Patman and SF2; three oscillator wave
attributes; partial native clone cleanup and retry; and playable split GIG clones
with different companion PCM. Native GUS and GIG comparisons, source-owner deletion,
portable saves and reloads are included. Plugin-dependent tests explicitly skip
when their required native plugin/build feature is absent; a skip is not full
verification. The process harness adds independently decoded AFP and oscillator
clone render comparisons after original input changes.

Source diagnostics pass across 270 changed C++ files and 120 native operation
declarations. The C++ tests and process audio comparisons remain uncompiled and
unrun. External plugin file access, GUI behavior, the complete autonomous
production cycle, native compatibility and CPU/RSS evidence remain outstanding.
The recorded missing native toolchain and pinned dependencies still prevent that
execution. The full DAW transformation remains active and incomplete.

## Native mixer ordering and headless channel ranges (2026-10-13)

The capability audit found channel moves confined to GUI actions and a concrete
headless routing defect: MixerView::updateMaxChannelSelector owned range updates
for instrument and sample routing controls. A track created before a bus therefore
kept its earlier maximum unless a view or later project load refreshed it.

Range maintenance now belongs to Mixer and runs after channel creation/deletion.
It covers song and PatternStore instrument/sample tracks, with null container
handling during engine initialization. Their audio buses also react to selector
property changes, so a normalized controller selecting a different channel after
a range change cannot leave the bus at an obsolete index merely because the
manual value stayed unchanged. The GUI's duplicate range maintenance is removed.

Mixer::moveChannel performs a direct move to a final index under the native model
guard. It rejects master/out-of-range indices, malformed channel indices, foreign
threads and reentrant moves. It rotates all crossed channels, retains effects,
routes and parameter identities, updates route names and solo selection, and
remaps snapshotted stored/reset track assignments. Snapshotting assignments before
updates prevents linked selectors from being shifted repeatedly. Native deletion
uses the same approach to static assignment/reset values. Channel naming now also
updates native volume/mute/solo parameter labels on rename, load and reindexing.

The GUI's left/right actions delegate to that operation and follow its move signal
to reorder existing channel views. Selection and effect racks stay attached to the
same views. Channel creation/deletion/reset and settings replacement are refused
when they reenter an active move; the GUI checks refused deletion before removing
its view. The studio exposes final-index moves through mixer.edit's new optional
`to` field and returns the resulting channel index. All edit fields, and the name
on mixer.add, are validated before mutation.

This preserves native numeric routing automation/controller semantics. Automation
attached to a bus volume or send stays with that model. Automation controlling a
track's channel selector still names numeric positions, as do continuous controller
values; it is not silently remapped into a channel-identity control system. Static
routing and reset values follow the existing channel objects.

MixerStructureTest contains 12 cases / 33 authored executions: headless selector
growth/shrinkage in both track containers; moves in both directions and across
multiple channels; retained effects, controls, sends, automation and solo identity;
default/custom names; linked current/reset assignments; numeric routing controls;
controlled bus refresh; invalid, reentrant and foreign-thread admission; actual
native stereo mixer processing; native XML, history, receipt replay and reopening.
The process harness adds bus assignment before moving, independently decoded
render comparisons, rejection, undo/redo, replay and native project reload.

Source diagnostics pass across 272 changed C++ files and 120 native operation
declarations. The new native cases and process comparisons have not compiled or
run. The missing native toolchain and pinned dependencies remain unresolved.
Dedicated studio channel-deletion/reset operations, GUI regression, arbitrary
callback exception behavior, full project/plugin compatibility, CPU/RSS evidence
and the complete conversational production cycle remain outstanding. The full
DAW transformation remains active and incomplete.

## Native mixer lifecycle and bounded inference declarations (2026-10-13)

The studio now exposes `mixer.remove` and `mixer.reset` through native Mixer
deletion/reset. Their core methods return admission results, so the adapter cannot
report success after master-removal, foreign-thread or active-move refusal. Removal
retires the channel's effects and routes and uses native assignment/range updates.
Reset preserves the channel and its surviving control identities, clears effects
and incident routes, restores native defaults and creates a unity send to master
for non-master channels. Resetting master clears its incoming sends. Existing
controller/automation bindings and control initial values retain native semantics.

Adding operations previously grew the inference request one function at a time.
StudioAgent now applies a local budget of 128 declarations independently of the
catalog: eight agent functions, one `studio_execute` adapter and up to 119 native
functions. Discovery and native rendering/recording/analysis are always selected;
remaining slots fill in catalog order. All 122 current native operations remain
discoverable and callable.
The adapter requires an exact native catalog name and object arguments and resolves
before ordinary request execution, including render timeout clamping. It cannot
dispatch non-catalog session controls, agent controls or itself. Legacy direct
function calls still resolve, so bounded declarations do not invalidate persisted
conversations. Collision checks include both declared and omitted function names.

The runtime retains the provider's call ID and original arguments in tool records
and continuation, while the durable receipt hashes the resolved native request.
Existing expected-revision checks, rollback, resource budgets, operation counting
and failure-driven skipping of dependent calls remain in the same execution path.
No recursive request execution or second mutation layer is introduced. The local
128 budget is a conservative compatibility choice; current official API pages
reviewed did not establish a universal provider limit. Official guidance supports
limiting initially available functions and deferring larger catalogs:
[OpenAI function calling](https://developers.openai.com/api/docs/guides/function-calling).
Actual model/provider behavior and discovery/context size remain unverified.

MixerStructureTest adds three cases / 12 executions for removal/reset, target
destruction, route cleanup, all track/container assignments, preserved reset
bindings, own/other solo, invalid/master arguments and foreign-thread refusal.
StudioInferenceTest adds ten malformed-wrapper rows and expands declaration and
catalog-reachability assertions. StudioRuntimeTest adds eight executions across Chat
Completions and Responses for native dispatch, history, receipt identity, original
tool records, completed jobs, reopening and durable failures with skipped dependent
work. The process fixture adds independently decoded render comparisons after reset
and removal, direct receipt replay after undo, project reload, and catalog-dispatched
tempo/render inference calls. The 30 new native executions are authored, unrun.

Source diagnostics pass across 272 changed C++ files, 122 native operations and
eight agent operations, with 128 inference declarations including catalog dispatch.
They check delimiters, schema inventory, Python syntax, registration and whitespace;
they do not compile or execute C++.

The missing native toolchain and pinned dependencies still prevent compiling and
running these checks. This is source progress; the complete autonomous DAW,
plugin/project compatibility, GUI behavior and resource performance are unverified.
The full transformation remains active and incomplete.

## Mixer topology reentry and GUI ownership (2026-10-13)

The lifecycle review found a concrete native lifetime hazard: reset invokes
EffectChain::aboutToClear while retaining a raw channel pointer, but the callback
could previously delete/reorder that channel. Route updates likewise emitted model
signals without protecting the route returned to the caller. Protection limited
to channel moves left creation, reset, deletion, bulk clear/load and solo callbacks
open to structural reentry.

Mixer now admits topology changes only on its owner thread outside an active
topology or solo transition. A shared RAII guard covers channel and route APIs,
bulk clear/load and publication callbacks. Private helpers perform the internal
steps of an admitted operation without allowing callbacks to start nested changes.
Destruction closes topology admission as well. Normal C++ exception unwinding
releases the guard, allowing native checkpoint restoration through StudioRuntime;
direct core calls can still leave partial changes when a callback throws.

The GUI review found separately parented EffectRackView objects surviving their
channel view, the channel widget being removed from the rack layout instead of
its rack, unowned rename scenes, selection shifting to a different bus when an
earlier bus was deleted, and refresh disconnecting models through obsolete native
indices. Channel views now own their rack through a guarded QObject reference,
own the rename scene and disconnect stored solo/mute connections. Removal retires
the correct rack and disables late rename commits before destroying the channel.
It retains the selected view when possible. View edits and bulk clear are guarded
against reentry; refresh uses connection handles rather than old channel indices.

Existing native channels receive rack-layout entries when the view is constructed.
MixerView can attach to an initialized engine without creating GuiApplication;
EffectView conditionally uses the desktop window host and otherwise parents its
control window to the effect view. This supports isolated native QWidget lifetime
tests. It does not change the headless studio's QCoreApplication lifecycle or prove
that every plugin editor supports standalone construction.

Effect rack updates tolerate a retired model. Queued effect deletion captures a
QPointer to the original view, so a native reset or refresh cannot redirect a stale
request to a replacement effect view. Direct deletion also verifies membership.

Four new MixerStructureTest cases / 14 executions cover callback reentry during
create/delete/reset/clear/load, existing-route amount changes, route destruction,
route-driven solo refresh and solo masks; exception admission release; foreign
thread route refusal; and studio reset failure with native checkpoint restoration,
retry, replay and undo. The suite now has 19 cases / 59 executions. A separate
MixerViewLifecycleTest has eight cases / 14 executions covering existing racks,
five selection/deletion arrangements, late rename signals, rack/scene retirement,
repeated refresh, refresh after native deletion, reversed parent destruction,
queued effect deletion across reset/remove/refresh, nested edits and bulk clear.
That suite uses QApplication and the offscreen Qt platform; core tests remain
GUI-less. All 28 new executions are authored and unrun.

Source diagnostics pass across 276 changed C++ files and the unchanged catalog of
122 native operations / 128 inference declarations. A fresh read-only PATH check
finds no CMake, Ninja, C++ compiler or qmake; git reports all 17 pinned submodules
uninitialized. No native compilation or execution occurred.

Native compilation, actual GUI and plugin behavior, callback/event failure modes
outside these guarded APIs, full project compatibility, autonomous production
traces and CPU/RSS/audio evidence remain outstanding. The full transformation
remains active and incomplete.

## Native effect-chain ownership and order (2026-10-13)

The effect-chain review found unguarded vector reordering, empty-chain access in
adjacent move helpers, manual audio lock pairs around callback-producing edits,
and GUI ordering that could disagree with the native chain. The GUI also retired
an effect view before establishing whether native removal succeeded. Missing
plugin serialization appended the original DOM node into another document,
potentially altering a previously retained snapshot.

EffectChain now uses the native audio change guard, owner-thread admission and
a scoped transition flag for append, detach, reorder, clear/load and serialization.
Final-position moves rotate the existing ordered vector and notify observers;
they preserve plugin/control/automation identities. Empty, invalid, foreign-thread
and reentrant structural requests are refused. Native loading preserves plugin
and peak-controller teardown-before-load order, while local unique ownership
covers unpublished instances. Owned append transfers at publication before
callbacks; owned detach releases the QObject parent. The legacy detach entry
point preserves its previous parent-managed lifetime behavior. Missing-plugin
save imports a deep copy of the original node.

Mixer topology admission includes active owned effect chains so their callbacks
cannot reset, move or delete the owning mixer channel. Reset propagates a refused
effect clear. GUI racks rebuild their ordered view list from native effects while
retaining surviving view objects and control windows. A refused native detach
keeps the GUI view intact; successful queued deletion remains guarded by QPointer.

The studio adds `effect.clear` and extends `effect.move` with a checked final `to`
position. Existing up/down requests retain their boundary no-op behavior. The
native catalog has 123 operations; the local inference declaration budget remains
128, using the existing `studio_execute` route for omitted individual declarations.

EffectChainTest contains 12 cases / 42 executions. It covers order-dependent native
DSP, retained automation/control identities, owned and legacy lifetimes, rejected
edits, callback exceptions and audio mutex release, missing-plugin XML snapshots,
song/pattern instrument/sample and mixer chains, history/replay/reopen and failed
studio edits with checkpoint rollback. MixerViewLifecycleTest adds two cases,
bringing that suite to 10 cases / 16 executions. The process harness adds a real
bundled Amplifier/Stereo Matrix comparison: attenuate/keep left then swap versus
swap then attenuate/keep left, plus rendered clearing, XML, history and replay.
These 44 added native test executions and the process scenario are authored only.

Source diagnostics cover 278 changed C++ files and the 123-operation catalog;
Python syntax and whitespace checks pass. No native code compiled or ran. A fresh
PATH check still finds no CMake, Ninja, C++ compiler or qmake. Existing pinned
dependencies remain uninitialized. Native compilation and execution, real plugin
and GUI behavior, autonomous production traces and CPU/RSS/audio evidence remain
outstanding. Synchronous callback tests do not establish arbitrary callback,
cross-object destruction, crash or hang recovery. Direct core calls can retain
partial changes on exceptions; the studio checkpoint provides transaction rollback
where restoration succeeds. The full transformation remains active and incomplete.

## Native effect and mixer control inventory (2026-10-13)

The control-discovery review identified a functional gap in the native agent
interface. MixerChannel owns its EffectChain as a value member with no QObject
parent, and MixerRoute's amount model and EffectChain's enabled model likewise
have no QObject parent. The previous scan of mixer QObject descendants therefore
missed mixer effects, every chain's bypass control and mixer-send amounts. A
parameter-based agent could not edit or automate those controls even though native
DSP, GUI and XML serialization already supported them.

The studio now enumerates these native owners explicitly without reparenting them
or introducing duplicate control state. EffectChain exposes its existing BoolModel.
A shared collector refreshes native plugin models before gathering pointers,
includes standalone model roots and deduplicates handles. Instrument inspection
uses that collector too. Effect parameters are accepted only while their effect
belongs to its native processing chain; missing-plugin dummy controls are omitted
because editing them would not update the plugin's preserved XML.

Parameter metadata now identifies effect instances, chain bypass, mixer channels
and send endpoints. Filters can select a track/container, channel, exact effect
instance, route endpoints or controller. Mixed selector families fail before an
edit. Native moves preserve model handles while discovery reports the updated
indices. Restoration and plugin model refresh can replace handles.

The new `effect.inspect` operation returns ordered plugin/subplugin identity,
missing-plugin status, chain bypass metadata and native effect controls. Missing
effects retain their original plugin names/key attributes and opaque state. Project
observations now expose track effect chains, chain enable state, sample-track mixer
routing and send parameter handles. Agent instructions point to this discovery
path and the existing parameter/controller/automation operations. The catalog has
124 native operations while the declaration budget remains 128.

StudioEffectControlsTest adds 10 cases / 30 executions. Authored checks cover all
five chain kinds, repeated plugins, precise locations and filters, native bypass
and smoothed mixer-send DSP, moved identities, placeholder/retired control exclusion,
plugin model refresh, controller connections, ambiguous selectors, and native
automation/XML/history/replay/reopening for bypass and send models. Read-only
discovery is checked against unchanged checkpoint history and revision. The process
harness now discovers real Amplifier/Stereo Matrix controls, edits one repeated
Amplifier independently, compares bypassed audio and controls an actual mixer send.

Source diagnostics pass for 279 changed C++ files, the 124-operation catalog,
Python syntax and whitespace. The 30 new native executions and expanded process
scenarios have not compiled or run. The missing native toolchain and uninitialized
dependencies remain unresolved. Actual plugin compatibility, GUI behavior, audio,
CPU/RSS measurements and autonomous production evidence are still required. The
full transformation remains active and incomplete.


### Native graph model and plugin curves (2026-10-13)

The preceding completed turn made source progress on native effect/control
inventory. This turn extends native sound design to waveform and effect curves.
The full commission remains active and incomplete.

The repository investigation found a shared graphModel implemented in the Graph
widget translation unit, with a file dialog in the model. Eight bundled plugins
used that model for saved DSP inputs that numeric parameter discovery could not
reach. Their Base64 loaders also copied decoded buffers without validating their
lengths; several overwritten temporary pointers leaked allocations, and Vibed's
length assertion provided no release-build protection.

GraphModel.h and core/GraphModel.cpp now own the native model and algorithms.
Graph.h retains the widget and imports the model; the file dialog lives in the
widget adapter. Native waveform loading uses existing SampleBuffer and Oscillator
code with FileResourceIO event suspension. Checked bulk publication validates the
entire input before entering the audio change boundary, retains hidden storage and
sample addresses, then notifies existing plugin observers. Owner-thread and active
update admission guards cover model mutators. GUI line strokes use that checked
publication. Single-point smoothing and negative convolution/phase offsets have
bounded index handling.

Plugin::GraphResource exposes existing graph pointers, keys, labels, units and
writability without duplicating project state. Native graph.list/inspect/set/
transform/load capabilities address instruments or exact effect instances on song,
pattern and mixer chains. Instrument/effect observations expose available graphs,
and the agent instructions direct sound-design work through discovery. The native
catalog has 129 operations; inference declarations remain within the existing
128-tool budget through the generic catalog executor.

Registrations cover Waveshaper and Dynamics Processor transfer curves, Multitap
Echo gain/filter tap curves, BitInvader, FreeBoy, Watsyn, Vibed and Xpressive raw
waveforms. Xpressive processing was editor-driven: changing smoothing or raw wave
samples outside its view did not refresh the DSP copies. Native signal connections
now refresh those buffers, and graph edits clear the corresponding generating
expression. The editor shares the native refresh method. Dedicated expression
editing is still an unimplemented capability; native presets remain supported.

All eight graph loaders use a common bounded Base64 decoder that checks encoding,
float alignment/count and finiteness before copying. ASCII whitespace and unpadded
encoding remain accepted; missing attributes keep defaults. Fixed-size tables
require their exact length. BitInvader preserves the 200-sample serialized table,
accepts shorter bounded legacy data with zero filling, restores its visible length
and recomputes normalization for that visible prefix. Present malformed graph data
is collected as a native project error so the studio's existing transactional load
and recovery path can reject it. This is not an atomicity claim for each plugin's
entire direct loadSettings implementation.

GraphModelTest adds 12 cases / 24 executions for checked edits and stable borrowed
samples, hidden tails, quantization, legacy serialization, malformed/truncated data,
missing attributes, reentrant/foreign-thread refusal, exception guard release,
edge transforms and native audio-file decoding. StudioGraphTest adds 7 cases / 22
executions for five native effect locations, repeated instances, graph metadata,
strict sample validation, native transforms, read-only/missing plugins, selector
rejection, actual native effect-buffer DSP and prepublication overflow refusal.
Both suites use QCoreApplication and are registered with 120-second CTest timeouts.

The process harness now exercises real graph serialization in all eight bundled
plugins; BitInvader, Waveshaper and Xpressive render/silence paths; a shrunken
BitInvader table's hidden tail; embedded waveform recovery after source deletion;
malformed-project rollback; and durable undo/redo/replay. Xpressive's verification
preset explicitly makes its output depend on W1 before editing that native wave.

Source diagnostics pass for 302 changed/new C++ files, 129 native operations,
8 agent operations, bounded inference declarations, Python syntax and whitespace.
The 46 native C++ executions and expanded process scenarios have not compiled or
run. Qt/compiler/MOC availability and 17 uninitialized pinned submodules remain
unresolved. No actual audio, GUI, CPU/RSS or autonomous-production success is
claimed. Native plugin callbacks may still throw after publication or retire
objects; these guards do not establish arbitrary callback/destructor safety.
Legacy corpus compatibility and all remaining application capabilities still need
native verification before the full transformation can be considered complete.


### Native expression editing and Xpressive ownership (2026-10-13)

The preceding turn made source progress on waveform and effect-curve access. This
turn addresses the expression-driven part of Xpressive's sound design. Full DAW
completion remains unproven and the original transformation objective is active.

The editor previously assigned saved expression strings, configured ExprFront,
compiled formulas and generated samples inside XpressiveView::expressionChanged.
The instrument only restored the strings/samples and constructed output parsers at
note onset. Reopening the editor could emit textChanged and regenerate a saved
random waveform. Headless operation therefore lacked a checked way to edit formula
state even after native graph access had been added.

Plugin now exposes optional ExpressionResource inventory and ExpressionResult
validation/edit hooks. The three expression capabilities select the current native
instrument or exact effect instance, expose source/symbols/semantics and dispatch to
those hooks. Read-only or missing resources fail before mutation. Instrument/effect
inspection and native agent instructions include this sound-design path. There are
132 native operations and 8 agent operations, with inference declarations still
bounded at 128 through the existing generic catalog executor.

Xpressive owns five formula resources and stages ExprFront preview evaluation before
publishing accepted text/samples under its native audio change guard. Wave publication
uses the existing GraphModel callbacks, smoothing and WaveSample DSP copies. Outputs
keep their note-onset compilation lifecycle. Empty wave text retains sample state;
empty output text disables that output on subsequent notes. Existing XML attribute
names and generated sample serialization remain authoritative. Load does not
regenerate saved wave samples, including legacy formulas that may themselves be
invalid. Such source can be discovered and explicitly validated or repaired.

The editor is now an adapter for native validation/publication. Signal blocking on
layout changes prevents accidental regeneration. Native expression/graph edits
synchronize the displayed text without recursive publication. Invalid editor drafts
retain the last accepted native formula and are not written into project state.
GUI behavior still needs native regression verification.

ExprFront captures parser diagnostics and initializes its data pointer defensively.
The shared preview path preserves the existing wave preview dimensions and exposes
the output preview's artificial note context explicitly. It preserves random state
for validation and advances it for successful generation. Event-suspended budget
checks run before compilation and between 64-frame batches. Waveform previews retain
native nonfinite-to-zero behavior; nonfinite output previews refuse publication.
Native waveform lookup now checks finite phase and wraps a rounded endpoint before
indexing; random index/seed conversion checks finite representable bounds. Empty or
invalid output pairs explicitly zero their DSP buffer. These changes do not prove
finite audio for every future note/automation state or interrupt arbitrary parser
work already in progress.

The repository pins ExprTk at a4b17d543f072d2e3ba564e4bc5c3a0d2b05c338, but that
submodule is uninitialized. Upstream's documented parser.error diagnostic interface
was checked against its [official documentation](https://github.com/ArashPartow/exprtk).
Fetching the pinned source through the browser failed; compatibility with the pinned
header remains a required native build check. Existing compile definitions and
platform options were moved onto the exprtk interface target so the plugin and the
new native test use the same parser configuration.

XpressiveExpressionTest adds 10 cases / 19 executions for native time/integration,
output symbols, domain exclusions, malformed syntax/source, nonfinite behavior,
empty semantics, random-state preservation, cancellation, lookup bounds and preview
allocation limits. StudioExpressionTest adds 5 cases / 13 executions for five native
chain locations, repeated owners, metadata, read-only validation, invalid/refused
edits, schema rejection and real native effect-buffer DSP. The parser test compiles
the production ExprSynth implementation and is conditional on the bundled ExprTk
target. Both suites are registered with 120-second timeouts.

The process harness now uses expression.set for its Xpressive graph/DSP fixture.
A separate durable native session exercises actual Xpressive formula generation,
read-only history invariance, wave samples and audible/silent output, invalid edit
retention, empty text, graph/formula ownership, undo/redo/replay, project/preset
serialization and restart with retained samples and rendered output.

Source diagnostics pass for 305 changed/new C++ files and the 132-operation catalog,
plus Python syntax and whitespace. The 32 new native executions and process scenarios
have not compiled or run. Compiler/Qt/MOC and all 17 pinned submodule initializations
remain unavailable. There is no new native audio, GUI, CPU/RSS or autonomous production
proof. Callback lifetime/reentry beyond the guarded expression path, real parser
interruption, full project/plugin compatibility and the remaining application-wide
capabilities still require implementation or verification before completion.

### Implementation record: native VST controls, 2026-10-13

Previous turn classification: progress. The full commission remains active and
unbounded. This continuation addresses another editor-dependent native capability;
it does not establish completion of the full DAW or the VST host.

Inspection found parameter model construction in both VST parameter editors and in
settings loaders. Fresh plugin loading supplied no controls until an editor opened;
settings restoration could allocate another set, and editor destruction deleted
native automation targets. Parameter indices were recovered from display labels,
and splitting legacy records at every colon broke labels containing colons.

The shared VstParameterModels implementation now owns one model per native index.
VeSTige owns it directly; VstEffectControls owns the effect's instance. Both initialize
on native load and expose refreshParameterModels to the existing studio inventory.
The GUI binds existing models and owns only widgets. Native edits use one direct
writer per control; no duplicate editor connection is installed. Refresh keeps IDs,
updates unbound values without sending them back to the worker, and preserves
connected automation, controllers and model links. Count changes refuse replacement
until explicit plugin reload. Same-plugin restoration preserves model addresses.

The decoder validates the complete native reply before publishing values, accepts
colons in names and existing decimal locale formats, and sorts by numeric index
rather than QMap key order. Legacy scalar attributes left by older controller saves
are accepted during native restoration. Metadata restoration validates normalized
values before touching models and retires old controller connections. Saving keeps
the plugin's non-chunk parameter attributes intact while appending native automation
and controller information, avoiding the shared-key overwrite in the old code.

VstPlugin tracks the worker's declared parameter count, validates returned indices
and records, reports absent/failed replies, and checks outgoing normalized values.
RemoteVstPlugin validates index/range before calling the hosted setParameter and
stages bulk records before dispatch. Scoped locks replace manual unlocking in native
requests and both DSP adapters. VeSTige and VstPlugin explicitly gate editor creation
on GUI availability. VeSTige exposes its binary as the `plugin` file resource through
the existing resource operation. Remote float encoding and controller/automation
scheduling retain their current semantics; parameter writes have no new remote
acknowledgment. Plugin dependencies and native/Wine compatibility remain unverified.

The shared model rejects wrong-thread and synchronous refresh reentry before adapter
locks. Audio model guards precede plugin locks; event delivery is suspended during
native work. Decoding and model creation have cooperative cancellation/budget checks.
These guards do not make arbitrary plugin code preemptible or prove every callback
lifetime, third-party behavior or resource bound. Dynamic parameter layout changes,
VST program operations and explicit FXP/FXB path APIs still require work.

VstParameterModelsTest authors 10 cases / 24 executions using the production model,
including malformed record rows and five native effect-chain ownership locations.
It checks numeric index ordering, colon/locale compatibility, one native writer,
identity retention, explicit retirement, automation/controller/link preservation,
legacy XML, repeated restores, invalid metadata, owner-thread/reentry refusal and
cancellation. Its deterministic callback drives an actual native EffectChain buffer;
it is not a hosted VST DSP fixture. The target is conditional on vstbase and compiles
the shared implementation directly with a 120-second timeout.

Source diagnostics cover 317 changed/new C++ files and the unchanged 132-operation
catalog. The 24 new native executions remain uncompiled and unrun. No real VST binary,
remote protocol regression, GUI lifetime test, legacy project corpus, native rendered
audio, CPU/RSS measurement or autonomous production workflow was executed. The missing
native toolchain, Qt/MOC and 17 uninitialized pinned submodules still prevent native
validation here. All completion evidence gates remain open.

### Implementation record: native indexed plugin programs, 2026-10-13

Previous turn classification: progress. All five commissioning attachments were
reread and the current VST source/evidence inspected. The complete application remains
the objective. This continuation adds native program discovery and selection; full
completion is unproven and the goal remains active.

The previous VST program-name request built a delimiter-separated string, capped the
catalog at 256 entries, and selected every program when indexed names were missing.
That fallback could overwrite the current unsaved sound merely by opening a menu.
The worker's cached current-program reply could also lag an explicit selection.

Plugin now exposes an indexed NativeProgramPage contract distinct from Instrument's
MIDI-bank program list. Two studio operations select either an instrument or an
exact native effect owner. Paged results contain explicit name availability; the
studio checks page structure and selection readback. Native capability inventory and
agent instructions describe the difference from MIDI bank/program pairs. The catalog
has 134 operations; the existing 128-declaration budget and full catalog executor
continue to apply.

The production VstPrograms helper performs bounded read-only queries, including
indices above 255. It calls an indexed-name callback only for unselected programs and
uses the actual current name for the selected entry. Unsupported indexed names remain
explicitly unavailable, with selection still supported. This intentionally removes
the old side-effecting name fallback: RemoteVstPlugin::getProgramNames previously
called effSetProgram in its enumeration loop. An unselected name cannot be read
through a missing indexed callback without changing live plugin state. Actual names
can be learned through explicit selection and native checkpoint/revision recovery;
listing itself does not perform those edits.

The existing index/category dispatch convention is consistent with the reference
[JUCE VST2 implementation](https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_plugin_client/juce_audio_plugin_client_VST2.cpp).
This cross-check is not a hosted-plugin compatibility result. Program name encoding
and arbitrary callback behavior still require native ecosystem verification.

Two appended remote message IDs retain existing message numbering and carry a
structured page or a checked selection result. The worker validates the requested
index, calls its existing effSetProgram dispatch and reads effGetProgram afterward.
Malformed, missing, failed and inconsistent replies are rejected by the LMMS host.
The old current-program reply is refreshed from the native plugin; zero-program
selection avoids the old -1 dispatch path. Legacy relative rotation uses a widened
sum, avoiding overflow. Matching LMMS and remote-worker builds are required.

VeSTige and VST effect adapters guard owner thread, reentry, audio-model changes and
plugin access. Program selection synchronizes existing parameter models without
replacing automation targets. GUI menu/next/previous actions use the native adapter,
report errors and mark successful changes as modified. Menu entries use individual
strings rather than splitting labels at a pipe. Existing chunk/parameter project
serialization and its numeric program attribute remain authoritative. The legacy
zero-program attribute is accepted. Native undo/replay/reopen behavior has not been
executed with a real VST and remains a required compatibility gate.

VstProgramsTest adds 6 cases / 11 executions for large catalogs, names with separators,
unknown names, empty/tail/out-of-range pages, invalid query bounds, strict selection,
readback mismatch and native callback failure. StudioPluginProgramTest adds 3 cases /
12 executions for five effect ownership locations, unchanged read-only state, exact
instance selection, stable parameter identity, native EffectChain DSP, invalid
arguments, missing plugins, malformed pages and refused/incorrect selections. These
fixtures do not run an actual hosted VST. Both native targets are authored, registered
and uncompiled/unrun.

The FXP/FXB path remains unfinished. Source inspection found that loadPresetFile
continues after failed fread calls, allocates from unvalidated file counts, decides
preset versus bank from the path suffix and dispatches parameter indices without
checking the file against the loaded plugin's dimensions. savePreset derives a
program name from the filename, mutates the live name and does not check fwrite
results. Exposing these methods as successful headless file operations would not
satisfy the commission's compatibility/recovery requirements. Checked native parsing,
serialization, status replies, accepted-file handling and preset/bank APIs are next
work; no external controller or replacement sound generator is introduced.

Source validation covers 321 changed/new C++ files and 134 native operations. The
23 new native executions have not compiled or run. Compiler/Qt/MOC availability and
17 uninitialized pinned submodules remain unresolved. No new real VST audio, GUI,
legacy project compatibility, autonomous production trace or CPU/RSS evidence exists.
The full application's completion gates remain open.


### Implementation record: checked native VST preset files, 2026-10-13

This record supersedes the FXP/FXB source gap described in the preceding entry.
Native VST instruments/effects now expose preset inspection, application and export
through `Plugin::NativePresetInfo` and the studio's exact plugin-owner selectors.
The three operations raise the source catalog to 137. GUI dialogs call the same
owner-thread, reentry and audio-guarded adapters; parameter models remain attached
through native synchronization after import.

`VstPreset.h` decodes/encodes all four VST2 FXP/FXB forms with complete byte/count,
version, normalized-float and nested-ID checks. Parsing is independent of the
filename. It accepts zero size fields used by established writers and version-2
bank selection. `VstPresetAccess.h` validates the entire file against native plugin
dimensions before application, uses actual VST callbacks, preserves the exposed
active edit buffer during bank export, and retains unimported current state when
loading a partial version-1 parameter bank. Opaque chunks stay native plugin data.
Neither the format nor the worker response proves opaque payload interpretation.

`VstPresetFile.h` owns checked UTF-8-opened worker file streams, including close
failures. `VstPresetStorage.h` owns host-side accepted input, digest validation,
private worker output, cooperative file checks and atomic `QSaveFile` publication.
`FileResourceSnapshot::captureBounded` checks the opened file's size before copying
and retains accepted read-scope identity. Both the file and decoder allocation
accounting have a 256 MiB bound; total peak memory and arbitrary native callback
latency remain unmeasured. Worker status errors reach both the studio and GUI.

Bank import exposed an existing persistence shortfall: non-chunk VST XML saved only
the selected program. Native serialization now also embeds an FXB (FXP for effects
with zero indexed programs) as canonical Base64 `nativepreset`, while retaining
legacy current-program fields. Restore validates the new representation and uses
the checked worker path. Existing opaque chunks now require checked I/O replies;
failed advertised chunk capture no longer silently falls back to incomplete state.
These changes are intended to make native bank state part of the existing project,
checkpoint, undo/replay and session recovery flow; none of those paths has been
executed with a real plugin in this environment.

Import and export are marked as project mutations: export may select programs
while collecting a non-chunk bank, so the session owns a checkpoint before it
starts. A successful export restores exposed state but still creates a revision.
Exported files are not reverted by project history. Publication can precede a
later session commit failure; receipts/digests describe the actual file operation.
Recovery cannot be claimed for arbitrary plugin-internal side effects or a plugin
that refuses restoration. Matching rebuilt host/worker binaries are required.

Three QCoreApplication native test targets author 19 cases / 50 executions:
codec/callback behavior, production host storage with deterministic worker requests,
and five native effect-owner locations with EffectChain DSP fixtures. Independent
hardcoded byte fixtures and all their truncated prefixes supplement round trips.
The tests include private-input replacement, opened-file size limits, cancellation,
retained output after errors, canonical project data, complete-bank validation,
active-program restoration, partial banks, chunk/zero-program handling, invalid
selectors and native error propagation. No mock is presented as a real VST worker.

Source diagnostics pass for 328 changed/new C++ files and 137 native operations,
with eight agent operations and the 128-declaration inference limit unchanged.
All 50 new native executions are uncompiled/unrun. The absent native toolchain/Qt
and 17 uninitialized pinned dependencies still prevent native execution. GUI,
legacy project/preset corpus, real plugins/DSP, checkpoint/restart correctness,
CPU/RSS and autonomous long-horizon production remain unverified. The complete
headless agent-native DAW objective remains active and unproven.


### Implementation record: VST state acknowledgment and native worker coverage, 2026-10-13

The preceding turn made source progress on checked preset files and native bank
serialization. Review of the current host/worker boundary found another concrete
gap: `VstPlugin::setParameterDump` sent restored values without awaiting execution,
while the worker queued the bulk message on its control thread and could process
later audio messages immediately. Bulk restore now uses a separate appended
protocol ID, complete numeric/count/index preflight and an explicit success/error
reply after native callbacks finish. Host ownership is checked before taking the
audio guard. Callback exceptions reach the caller; opaque partial changes still
require the session's existing checkpoint recovery.

`VstParameterWire.h` centralizes strict wire parsing and locale-independent
round-trip float formatting. Native scalar automation retains its processing-thread
path without acknowledgment per update. Queries validate their complete field
layout before publishing cached values and surface worker-side query errors. The
old six-decimal VST transport no longer dictates the precision of new parameter
messages. Single-write application/readback, native quantization, asynchronous
automation timing and unusually large parameter dumps remain separate concerns.

Three new test targets author 10 cases / 36 executions. Twenty exercise the production
wire helper; twelve launch the production VST worker and load actual native
MODULE fixtures. The host sources compile into the test's engine instance, while
the child executes the ordinary worker binary. Fixtures have 700 parameter/chunk
programs or zero indexed programs, native stereo gain DSP, a failing chunk mode,
and an editor-open trap. The test runs under QCoreApplication without DISPLAY.
It covers checked restore ordering, precise values, foreign-thread refusal,
program queries/selection, FXP/FXB files, retained exports on errors and native XML
restoration after constructing a new worker. It does not replace a third-party
compatibility corpus or a full session/agent recovery trace.

The same review found an unbounded copy into the worker's fixed 1,024-event MIDI
arrays. Native dispatch now uses a checked, dynamically sized pointer packet with
owned event records that remain valid through the next audio callback. It keeps
stable sample ordering and reuses peak packet capacity. Four helper executions
cover zero through 8,193 events and producer-queue reuse; the additional worker
execution sends 1,500 events whose pointers the native fixture reads only during
audio processing. Allocation/growth latency and CPU/RSS remain unmeasured.

CMake wires matching native Windows and 64-bit Linux worker targets and all three
fixture modules as build dependencies. The tests are authored but unconfigured,
uncompiled and unrun. Source validation passes for 334 changed/new C++ files and
137 native operations; eight agent operations and the 128-declaration inference
limit are unchanged. No new native audio, GUI, timing, memory or autonomous workflow
execution evidence exists. Full-application completion remains unproven and the
commission remains active.


### Implementation record: VST project recovery boundary, 2026-10-13

Following native host/worker state tests, the recovery review traced VST state
through the actual effect wrapper and StudioRuntime checkpoints. Legacy opaque
chunk XML still used permissive Base64 conversion before applying plugin state.
Malformed text could therefore be reduced to a different accepted byte stream.
The host now validates the entire encoding before native program selection.
Canonical, ASCII-wrapped and unpadded legacy forms remain supported; corrupt
characters, invalid padding/bits, incomplete or empty payloads and oversized state
are rejected. The decoder checks cooperative cancellation/budgets during long scans.
Plugin-specific interpretation of a valid opaque chunk remains unverifiable from
the VST2 callback result alone.

Fourteen additional native C++ executions are authored: thirteen legacy encoding
rows and one actual worker case for rejection before program mutation followed by
valid wrapped-state recovery. A separate Python process harness authors three
full-build scenarios using `lmms studio`, production `vsteffect`/VstPlugin and the
native parameter-bank, chunk-bank and zero-program modules. LMMS performs all model
edits, checkpoint storage, worker lifecycle, DSP and rendering. Scenarios include
complete bank restoration, presets, native automation connections, undo/redo,
receipt replay, malformed project rollback, failed chunk serialization, graceful
reopen, abrupt-exit recovery and continued editing. Stereo PCM is compared after
these transitions. The tests rediscover revision-scoped parameter handles and
check restored automation targets instead of assuming object identities persist.

CMake registers three 360-second process cases and a `StudioVstProcessFixtures`
build target for their native dependencies. Build-specific effect/base modules
are copied to a private test workspace's native plugin search directory; original
VST binary paths remain external dependencies. Protocol traces, native renders,
stderr and verification records are produced only by actual process execution.
The existing larger process suite and real inference/third-party compatibility
verification remain required. No deterministic fixture establishes musical
reasoning, subjective listening quality or complete DAW autonomy.

Current source diagnostics pass for 334 changed/new C++ files and 137 native
operations; eight agent operations and the 128-declaration inference limit are
unchanged. Python syntax/import/CLI checks and `git diff --check` pass. No native
scenario has configured, compiled or run: CMake, Ninja, compilers and qmake are
absent from PATH and all 17 pinned dependencies remain uninitialized. Actual
recovery/audio correctness, full ecosystem compatibility, CPU/RSS and complete
autonomous workflows are still unverified. The commission remains active.


### Implementation record: VST loading and metadata refusal, 2026-10-13

The native plugin boundary review found three related failures. PE machine-type
inspection dereferenced an unchecked file offset in a whole-file mapping; parameter
text parsing interpreted byte lengths as decoded QString lengths and could stop
advancing; and failed VST effect construction disabled processing without marking
the effect unusable. Together these could crash or stall plugin inspection, or
allow an inert effect to be acknowledged by the studio.

PE classification now uses at most 88 bytes of bounded QFile reads with signature,
offset and complete-header checks. Existing x86/x64 machine values and field
positions follow Microsoft's [PE specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
This remains classification for the existing worker routes, not full executable
validation, immutable binary acceptance or expanded architecture support. Failed
effect construction now publishes `isOkay() == false`, which the existing native
effect-add and missing-effect restore paths already understand. Null subplugin
keys are handled without dereference.

The parameter wire helper now frames raw byte strings before host decoding and
rejects malformed, incomplete or extra fields before replacing caches. Worker
requests validate native indices and field counts and return explicit errors.
Stack-local text buffers are cleared for every callback, avoiding stale output
when a plugin does not supply a value. Metadata queries use the host owner thread
and native audio guard. Existing GUI entry points catch these errors so that Qt
event callbacks can display an unavailable value safely. Native execution and GUI
regression behavior still require verification.

Twenty-nine additional C++ executions are authored across the PE helper, parameter
wire and actual-worker targets. They cover truncated and malformed binary headers,
unaligned offsets, UTF-8 field boundaries, non-advancing malformed lengths, complete
cache retention, empty native callbacks and invalid metadata requests. The three
existing full-process scenarios now also require refused plugin additions to
preserve history, effect ownership, automation and rendered output, followed by
successful continued use of a valid plugin. These scenarios have not run.

Source validation passes for 336 changed/new C++ files and 137 native operations;
the eight agent operations and 128-declaration inference limit are unchanged.
Python syntax/import/CLI checks and `git diff --check` pass. No native compilation
or execution is available: build tools remain absent and all 17 pinned dependencies
remain uninitialized. Full application, third-party/corpus, audio, resource and
autonomous workflow evidence remains incomplete. The commission remains active.


### Implementation record: bounded VST parameter messages, 2026-10-13

Large valid VST layouts could exceed the shared-memory transport's 512 KiB message
limit because parameter dumps, restores and bulk metadata used one message for
the entire layout. The transport already refuses oversized messages safely, but
that refusal disconnects the worker during otherwise valid native operations.

The production host and worker now page parameter and metadata reads in groups
of at most 256 entries. Checked reply headers, native index order, bounded names
and text, and exact normalized float parsing protect complete-cache publication.
The native parameter count is not truncated. Cancellation or malformed replies
leave the previous host cache intact. Bulk restoration stages the full layout in
the worker through acknowledged begin/append messages, validates all pages before
commit, and releases staging on abort or malformed requests. No native setter
runs before commit. Paged waits hold the existing communication/audio guards and
retain deadlines without processing reentrant Qt events.

This improves the native VST implementation used by instruments, effects and the
studio's existing parameter/project operations. Old message IDs and handlers are
preserved; matching rebuilt binaries use appended protocol IDs. Total allocation,
native callback latency, external plugin state changes and GUI responsiveness are
not solved by message paging. A setter failure during commit may partially mutate
native state and still depends on studio checkpoint recovery. Audio-thread scalar
writes retain the existing asynchronous behavior.

The new transfer target authors 32 native executions, and the actual-worker target
adds three cases. A 65,537-parameter ABI module exceeds the former parameter and
metadata message sizes and exercises full read/restore, cancellation, continued
scalar editing, native persistence across worker restart and stereo processing.
Protocol refusal cases require no native mutation before commit and subsequent
successful reuse. The wire, transfer, PE, MIDI and worker targets now contain 101
authored executions; none has configured, compiled or run.

Source diagnostics pass for 338 changed/new C++ files and 137 native operations.
Python syntax and whitespace checks pass. CMake, Ninja, compilers and qmake are
still absent from PATH; all 17 pinned dependencies remain uninitialized. Actual
large-layout throughput, third-party/corpus compatibility, GUI behavior, CPU/RSS,
audio correctness and complete autonomous workflows still require native evidence.
The commission remains active.


### Implementation record: paged native discovery and durable reads, 2026-10-13

Reviewing large VST layouts through the full native session exposed a second
capacity boundary: control discovery returned one unbounded parameter array.
More than 16 MiB of result metadata could fail receipt archiving and stop a healthy
session during a read. The transport and session response both need bounded
transfers for large native layouts to remain agent-operable.

`parameter.list` and `instrument.inspect` now page native model metadata, with
filters applied before parameter offsets. `effect.inspect` pages its ordered
effect inventory and each effect's controls; `parameter.inspect` pages native
choice labels. Every page reports total, offset, limit, nextOffset and hasMore.
Count bounds and JSON array byte budgets prevent large inventories from being
assembled into a single receipt. Native indices, values, writable handles and
the complete underlying layout remain authoritative. Agent instructions explain
the continuation fields, and existing complete-inventory test readers now follow
them. Native cancellation checks avoid FileResourceIO event dispatch while model
pointers are being collected or serialized.

The new QCoreApplication target authors 21 native executions, including an
aggregate inventory exceeding 16 MiB, durable read receipts and replay, history
retention, all-owner filtering, complete control/choice enumeration, last-control
editing, invalid bounds and cancellation without Qt event reentry. A metadata item
larger than its own page budget produces an explicit read refusal; concise reads
and subsequent session work must still succeed. Existing VST process scenarios
also compare paged top-level and nested native handles. No native target or process
scenario has compiled or executed.

Source diagnostics pass for 340 changed/new C++ files and 137 native operations.
Python import/CLI checks and four pagination-helper checks pass; these exercise
instrumentation, not the DAW. Whitespace checks pass. Build tools remain unavailable
and all 17 pinned dependencies are uninitialized. Complete model enumeration and
plugin refresh still precede paging, and non-parameter metadata is outside these
array budgets. Dense-layout throughput, GUI/client compatibility, real native
audio, CPU/RSS, project/plugin ecosystem behavior and complete autonomous workflows
still need execution evidence. The commission remains active.


### Implementation record: dense native sessions and journal allocation, 2026-10-13

The large VST fixture now has an authored full LMMS process scenario. The same
65,537 native controls must survive paged discovery, first/middle/last editing,
automation on the final control, durable undo/redo and receipt replay, project
save/load, graceful restart and acknowledged-edit crash recovery. It independently
checks all saved native FXP floats and legacy parameter records, then removes the
native preset from a private project copy to exercise the staged legacy restore.
Native PCM comparisons and fresh automation target discovery are required after
restoration. All native models, DSP, persistence and recovery remain inside LMMS;
the Python code supplies test inputs and records evidence.

The scenario streams full JSONL traces and requires complete inventories exceeding
16 MiB to arrive through bounded pages with unchanged history and revision. It
checks archived receipt bytes/digests and immediate replay, and records native
resource snapshots tied to the actual LMMS process instance. CPU deltas and native
measurement intervals cover LMMS only; worker and harness resources are excluded.
No native measurements or execution result exist yet. The new CTest target uses
fixture 3 and a 900-second timeout.

Native journal allocation also had a dense-layout bottleneck: each allocation
started below 32,768 and scanned occupied IDs again. A continuing cursor now
searches the existing 23-bit native namespace, checks both live and serialized
aliases, and explicitly refuses exhaustion after one full traversal. Clearing
retired reservations does not restart the cursor. Five authored test cases / 11
executions check linear occupancy probing over 65,537 allocations, actual journal
alias reservations, cursor retention, wrap and invalid/exhausted namespaces. The
saved ID format and loaded-file identity policy remain unchanged. Allocation
memory, native throughput and existing owner-thread assumptions are not resolved
by this change.

Source diagnostics pass for 342 changed/new C++ files and 137 native operations.
Python syntax/import/CLI and whitespace checks pass. Native build tools remain
unavailable, and all 17 pinned submodules remain uninitialized. The four VST
process scenarios and new journal cases remain authored and unrun. GUI/corpus
compatibility, third-party plugins, actual audio and resource measurements and
complete autonomous music-production workflows remain required. The commission
remains active.


### Implementation record: immutable native agent tool results, 2026-10-13

Paged native discovery exposed another persistence boundary. The agent copied
each successful or failed tool result into both provider history and structured
evidence records. A batch of individually bounded responses could exceed the
16 MiB session manifest limit before another inference round could request
compaction. Session commits now publish immutable full tool records under
`tool-results/` and retain checked references in active records and provider
outputs. Responses above 256 KiB are stored this way immediately; above 4 MiB of
combined active input/records, older responses above 8 KiB also move. The latest
moderate output remains directly available to the provider.

This preserves the existing provider call/output positions and IDs, pending native
work, operation budgets, project history and job identity. It does not infer a
summary or finish the task. The same native history operation accepts a committed
job ID and tool call ID to retrieve the exact original record in bounded UTF-8
chunks. Existing context/record selection resolves stored payloads too. Explicit
compaction and provider replanning preserve these references, and call retrieval
continues after job archiving or application restart.

Startup verifies referenced payloads before native project replacement. Fresh
history reads and outcome evidence check the file digest and byte count, original
arguments and success status against the committed record. Nested references are
refused. Legacy inline records remain supported, including semantically equivalent
JSON formatting in provider output. Result-file publication precedes the manifest
commit; failure retains the last acknowledged state and invokes mandatory native
rollback for an uncommitted mutation. Existing receipt replay semantics remain.

`StudioToolResultTest` authors 11 cases / 35 executions for chat and Responses,
large and accumulated moderate results, protocol pairing, exact retrieval,
compaction/restart/archive continuity, corruption, rehashed but inconsistent
payloads, publication failure during native project loading, legacy migration,
pending-call recovery and duplicate call IDs. A scripted provider retrieves a
stored native inventory, obtains its tempo and requests a native revision before
citing evidence. It exercises runtime control flow, not independent musical
reasoning or listening.

Source diagnostics pass for 343 changed/new C++ files and 137 native operations;
Python syntax and whitespace checks pass. The new target is uncompiled and unrun.
Build tools remain unavailable and all 17 pinned submodules remain uninitialized.
Payloads still have a 16 MiB per-record limit and are buffered for verified reads.
Arguments, observations, inference reasoning, metadata and disk archives can still
grow; this does not implement automatic semantic compaction, provider context
window sizing, disk quotas or orphan cleanup. Actual provider behavior, CPU/RSS,
audio, GUI/ecosystem compatibility and complete autonomous workflows remain
unverified. The full commission remains active.

### Implementation record: bounded native project observation and arrangement pages (2026-10-13)

Routine agent observations previously built the complete native project before
removing notes and automation points from the provider payload. Large arrangements
therefore still incurred the full traversal and temporary JSON, while unbounded
track names could overflow observations or load receipts. The runtime now requests
`project.inspect` with `section: "summary"` under its cancellation/resource scope.
The native summary reads counts and at most 16 track headers from each owner,
returns bounded UTF-8 name previews and continuation metadata, and checks a 64-KiB
object budget. It does not materialize clip/note arrays or curve points first.

The same operation has `tracks`, `clips` and `notes` sections that serialize only
the requested native range. Song and PatternStore ownership, native ordering and
editable indices are retained. Pages default to 128 items, accept up to 1024 and
have a 512-KiB item-array budget. They return total/offset/limit/nextOffset/hasMore;
clients continue at one revision and rediscover indices after structural edits or
restoration. Track/clip pages can omit full names and sample paths in favor of
256-byte previews with truncation flags. An oversized individual full item fails
explicitly. Clip/active-detuning metadata gives counts without expanding curves.

The no-argument/full inspection remains available. Native project new/load/import
accept `summary: true` to return the bounded summary after their existing native
operation. This avoids placing all arrangement contents into a load receipt while
retaining native load validation and the runtime checkpoint/rollback/replay rules.
The agent receives instructions for progressive discovery and the incomplete
meaning of previews and counts. Read loops check cancellation while suspending
Qt event dispatch to avoid model-pointer reentry and reject partial results.

`StudioProjectInspectionTest` authors nine cases / 28 executions: dense notes and
curves with long Unicode names; both native owners; track/MIDI/sample/automation
clip and note pages; native editable index preservation; invalid selections;
aggregate inventory metadata above 16 MiB through bounded durable receipts;
receipt replay and unchanged project history; oversized-name preview recovery;
large saved-project load, reopen and continued editing; cancellation without event
dispatch; and a scripted provider in both supported protocols that reads a note
page and requests a native note revision. Each provider round must receive the
compact observation. This scripted control flow does not prove musical judgment.

Source diagnostics cover 344 changed/new C++ files and 137 native operations;
Python syntax and whitespace checks pass. The new target remains uncompiled and
unrun: compiler, CMake, Ninja, Qt tools and LMMS are unavailable, and all 17 pinned
submodules remain uninitialized. These response bounds do not establish limits on
native getters, project load/checkpoint memory, full legacy snapshots, other
inventories, working memory, provider history or disk growth. Native performance,
CPU/RSS, real provider behavior, audio, GUI/ecosystem compatibility and full
autonomous production workflows remain unverified. The commission remains active.

### Implementation record: bounded mixer/automation state and native agent history (2026-10-13)

The compact project header exposed counts for mixer channels/routes, controllers
and hidden global automation, but inspecting these through the full snapshot still
expanded unrelated native content. Automation inspectors also serialized every
point and target. The project inspector now pages `channels`, `routes`,
`controllers` and `globalAutomations`. Channel headers include native volume/mute/
solo and effect-bypass handles plus effect/send/receive counts; route entries keep
the native sender, receiver, amount and handle. Controller headers avoid recursive
control expansion. Hidden automation headers retain native clip indices, including
empty/unbound clips, without expanding their points or targets.

Arrangement, global and per-note automation inspection now has explicit full,
summary and points sections; non-note curves also have target pages. Point
continuation uses the actual next native tick and QMap lowerBound, preserving
sparse/signed keys and the maximum integer key without repeated offset scans or
increment overflow. Incoming/outgoing values, tangent values/locks, interpolation
and tension remain native. Target pages preserve slot indices, availability and
editable handles. Point/target pages share the existing 128 default / 1024 maximum
item counts and 512-KiB array budget. Name previews permit bounded discovery when
a single full label is too large. Reads preserve lazy curve absence and suspend
Qt event dispatch around native pointers while checking cancellation.

Global inspection can select exactly one parameter or native global clip index.
Index selection also exposes empty or unbound curves; empty bound evaluation uses
the native target value. Controller creation and curve mutations that previously
returned a whole rack/curve accept compact metadata acknowledgements. Their
omitted/false defaults preserve existing responses. No alternate automation model,
composition format or audio engine was introduced.

The large-curve recovery fixture exposed another expansion: session undo/redo
returned a complete restored project, potentially exceeding the receipt limit.
Both now accept an optional boolean summary, validated before restoration. They
also have individual native agent declarations, taking the supplemental agent
catalog from eight to ten operations while retaining the overall 128-declaration
budget and 137 native production operations. The agent is instructed to restore
separately and rediscover live handles. A scripted provider uses the native catalog
router to inspect/revise mixer automation, then invokes undo, rediscovers and checks
the original value, invokes redo, and checks the revised value with fresh handles.
Conversation state and evidence follow the existing native transaction boundary.

`StudioMixInspectionTest` authors 12 cases / 57 executions covering all four
inventories, three curve owners and three interpolation modes, signed tick
boundaries, target metadata exceeding 16 MiB in aggregate with bounded receipts and
replay, oversized labels, absent/empty/unbound curves, invalid selection fields,
cooperative cancellation without Qt reentry, compact mutations and history argument
validation. A 200,001-point native curve fixture requires its legacy result to
exceed 16 MiB, then exercises compact mutation, undo, old-request replay, redo,
reopen and continued editing. Both provider protocols exercise the scripted
history sequence. These fixtures remain uncompiled/unrun and do not demonstrate
independent musical reasoning or listening.

Source diagnostics cover 345 changed/new C++ files, 137 native operations and ten
supplemental agent operations; the declaration limit remains 128. The current
machine still has no C++ compiler, CMake, Ninja, Qt tools or LMMS executable, and all
17 pinned submodules remain uninitialized. Serialized page limits do not bound
native getters, complete parameter discovery, curve evaluation, mutation costs,
checkpoint/load allocations, accumulated provider context or disk growth. Native
headless/GUI builds, actual fixtures, audio, third-party/project compatibility,
CPU/RSS and complete autonomous production traces remain required. The full
commission remains active and unproven.

### Implementation record: agent continuation after native project replacement (2026-10-13)

The newly available native history tools exposed an execution-order gap. Failed
calls and engine restart already invalidated queued provider decisions, but a
successful undo/redo or load kept executing the same batch. Because requests used
native numeric indices, an otherwise valid `track: 0` could select a different
restored track. Automatically advancing the job's expected revision did not make
that old decision refer to the intended object.

After a successful session undo/redo or project new/load/import, the runtime now
retains the actual result and appends failed outputs for every remaining call in
that provider batch. Import is included because its resulting layout and global
settings need fresh observation even when existing objects survive. The runtime
uses the canonical resolved operation, so catalog dispatch follows the same rule.
The successful operation, its checkpoint/history transition, the complete paired
provider outputs and the emptied pending queue share the existing atomic manifest
commit. Skipped calls are not dispatched or charged as native operation attempts;
their IDs remain recorded and their outcomes cannot support completion evidence.

This does not end the job. The next native inference step supplies a fresh bounded
observation, preserving intent, plan, memory and prior evidence. The provider can
inspect the new contents and continue with fresh call IDs. Ordinary note/clip/
track creation and scalar edit batches retain their existing sequential execution.
A publication failure invokes existing native rollback and stopped-session rules,
leaving the last acknowledged pending work intact for restart invalidation.
Large result archives retain the successful result independently of the skipped
outcomes, with all references and outcomes published in the same manifest.

`StudioAgentRestoreTest` authors six cases / 30 executions: 16 project-operation/
protocol/routing combinations, four restart/ID-reuse combinations, two archived
large-result cases, two failed-manifest/rollback cases, two ordinary composition
cases and four failed-restore cases. Four of the 16 main rows require the built
MIDI importer. Fixtures require untouched restored tracks, no skipped render file,
paired provider outputs, correct history/revision/operation accounting, a fresh
observation of current native names, rejected skipped evidence, continued native
editing, durable skipped results and refusal to reuse their call IDs. The source
record also corrects obsolete protocol text that said the agent could not undo.

Source diagnostics and whitespace checks pass for 346 changed/new C++ files and
137 native operations; supplemental agent tools remain ten under the existing
128-declaration budget. None of the new native executions has been compiled or run.
Compiler/CMake/Ninja/Qt tools and LMMS remain unavailable; all 17 pinned submodules
are uninitialized. This change does not add immutable binding for every numeric
selector or invalidate decisions after all other structural changes. Native builds,
actual provider continuation, musical/audio results, full project/plugin/GUI
compatibility, CPU/RSS and complete autonomous workflows remain required. The full
commission remains active and unproven.


### Queued selections across structural edits (2026-10-13)

The runtime now records transient selections for remaining calls immediately
before dispatching the next call. After successful native execution, it resolves
the same selectors again. A changed or deleted existing target skips all remaining
calls with failed outputs in the same manifest commit as the successful result.
The job then observes the current native project and continues with new decisions.
This extends the earlier whole-project invalidation rule to object lifetimes
inside the current project; it preserves failure/restart behavior and ordinary
composition batches whose existing targets remain attached.

`StudioCapabilities::selection` resolves track/container, clip and destination
clip selectors, note indices, mixer channels and route endpoints/existing route
models, effect slots, controller slots, native parameter IDs, plugin owners,
global automation indices and explicit/implicit playback-pattern selection.
It uses the audio model guard and suppresses event reentry. It does not enumerate
or refresh parameter inventories, create automation, or validate future calls.
Missing targets may be created by earlier calls. Parameter lookup here only
captures matching current journal identities; actual project membership and
editability remain the normal operation resolver's responsibility.

QObject targets are retained as QPointers. Notes have a lazy application-thread
lifetime token; holding a token during comparison prevents address reuse from
impersonating a retired note. Copy construction and clone omit the token;
assignment preserves the destination identity. Playback note copies allocate no
identity and copy no identity reference count. The member still changes Note's
layout and has unmeasured memory/runtime cost. No identity is serialized or
exposed as a new tool handle. Hosted plugin removal/swapping unconditionally
invalidates the batch because PluginHost has no slot lifetime interface.

Seven native test cases / 47 executions are authored and registered with a
600-second timeout: 38 structural operation/protocol/router rows, note lifetime
copy/assignment/clone/address reuse, destination and invalid/unresolved selectors,
global/pattern lifetimes, and two rows each for stable/new-target continuation,
restart and failed-manifest rollback. The fixtures check actual LMMS models and
the durable runtime. They remain uncompiled/unrun; source checks cannot establish
native execution, ABI compatibility, realtime performance or musical judgment.

This is an intra-batch safeguard, not a persistent object-address protocol.
Pagination offsets and plugin-internal program, graph, expression, hosted
parameter and patchbay layouts are not individually bound, nor are tuning slots.
Further native builds, plugin/layout fixtures, CPU/RSS measurements, GUI/headless
compatibility, editable-project/audio evidence and complete autonomous production
traces are still required for the commission.


### Durable outcomes when replanning or replacing a conversation (2026-10-13)

Explicit resume after a project revision or reasoning-provider change previously
cleared `pending` before preserving its outcomes. The old provider exchange could
therefore end with unanswered calls, and their IDs were absent from the retained
tool-record index used to reject reuse. Starting a new conversation could archive
a paused/cancelled job with unresolved pending calls.

`StudioAgent::skipPending` now appends failed outputs in the job's existing
provider protocol and records the original calls before clearing the queue.
Revision/provider replanning invokes it before context archiving and provider
replacement. The existing archive index consequently reserves every abandoned
call ID, exposes its exact arguments and skip reason through history, and excludes
it from successful completion evidence. Prior completed results, operation counts,
intent, plan and session memory survive. A resume with unchanged revision/provider
retains its queued work. Cancellation by itself still allows that continuation.

When a new intent replaces an inactive job, `archiveJob` closes pending calls in
a staged copy, validates the resulting native agent state and checks the 16 MiB
archive limit before writing. The reference becomes authoritative only in the
new session manifest. The archived job retains its prior status and project
edits. An archive-write failure preserves the acknowledged pending decisions;
a manifest-publication failure stops the session without publishing new outcomes.
Existing recovery and replay rules apply. Unreferenced archives may remain after
failed publication, and already-discarded legacy outcomes are not recreated.

`StudioReplanningTest` registers five cases / 54 native executions with a
600-second timeout. Twenty-four rows cover revision/provider/both changes across
both protocols, partial execution and restart; four reject abandoned ID reuse;
sixteen preserve cancelled/paused jobs under a new intent; eight exercise failed
context/job archive creation or manifest publication; two retain queued work on
an unchanged resume. Fixtures check native project state, protocol output pairing,
exact history calls, evidence rejection, fresh observations, continued edits and
persisted counters. All remain uncompiled/unrun. This source change does not
establish provider musical judgment, audio correctness, project/plugin/GUI
compatibility, archive-boundary execution or resource/performance behavior.


### Bounded discovery of archived native decisions (2026-10-13)

History listing previously exposed context counts but no paged call inventory.
An archived job's final exchange was synthesized as protocol/input only, so it
reported zero tool records and rejected context/record reads even though the
job archive retained those records. Whole-context bytes are retained for existing
digest-bound readers. The runtime now derives the final exchange's remaining tool
records separately, reports their count and serves exact selected record reads.

`agent.history.list` accepts `job` plus `context` to page original record indices,
call IDs, function names, recorded outcomes and bounded error previews. Arguments
and full responses remain in the exact-read interface. Pages retain the existing
16-default/32-maximum count limits and add a 64 KiB record-array byte limit.
Previews convert only a bounded UTF-16 prefix and cut at a complete UTF-8 boundary.
Referenced contexts are checked against their digest, shape and committed tool
index; legacy inline contexts and final archived-job records use their committed
state. External result payloads are verified when selected for exact reading,
not loaded during metadata listing. Empty legacy record inventories stay empty.

Six native cases / 19 executions are authored with a 600-second timeout. Ten rows
cover both protocols and referenced/final stores before/after restart plus legacy
inline stores after restart. Other cases cover large stored results and skipped
calls, escaped/Unicode preview limits and cancellation, invalid selectors and
missing legacy records, fresh corruption checks versus receipt replay, and a
scripted agent that discovers an earlier tempo call, reads it and revises the
native song. Full final-context bytes and digests remain covered by existing
history tests as well as the new fixtures.

All fixtures remain uncompiled/unrun. Full archive read/parse costs remain bounded
by the existing 16 MiB file limit rather than by one returned page. Retained-index
scans cooperate with cancellation/resource checks. Historical handles are not
valid current project selectors. Native performance, CPU/RSS, real provider
judgment, audio and project/plugin/GUI compatibility remain unverified.

### Audio source identity through evaluation and revision (2026-10-13)

Listening previously decoded an open source and then hashed that descriptor.
An in-place rewrite could therefore make the digest describe different bytes from
the submitted excerpt. Analysis lacked source identity, and render receipts lacked
a digest that either evaluation operation could require. Native rendering now
hashes the encoded stream during atomic publication and returns `sha256` and
`bytes`. Analysis and listening decode a private `FileResourceSnapshot` and report
its `sourceSha256` and `sourceBytes`. Both accept an optional exact lowercase
SHA-256 expectation and fail on mismatch before sample measurement/provider submission.
Agent instructions carry the successful render digest into evaluation and require
a new render after native revision. Existing callers may omit the expectation.

The copy establishes accepted bytes, not a filesystem-wide instant in time: a
concurrent writer can alter the original while copying, but the digest describes
the actual copy used by the decoder. A known expected digest rejects such changes.
Temporary disk use scales with input size; listening retains its 1 GiB source and
20 MiB excerpt limits, while analysis retains its duration limit without a new
file-byte cap. Analysis preflights the original header's duration before copying
and checks the accepted header again before measurement. Copying
and verification are cooperative; blocked OS I/O or decoder calls remain outside
that interruption guarantee. The copy is released after evaluation, so this does
not add a persistent export store or guarantee a later pathname still exists.

Committed receipts remain historical: replay does not reread a changed or missing
file. Fresh requests verify accepted bytes again. Six native cases / 21 executions
are authored in `StudioAudioIdentityTest` with a 600-second timeout. They cover
16/24/32-bit native WAV publication, analysis/excerpt attribution under in-place
overwrite, atomic replacement and deletion during decoding, invalid/mismatching
digests, duration limits, cancellation in capture/decoding, provider non-submission
on mismatch, replay and restart. A scripted provider fixture follows native sample
arrangement, rendering, digest-bound analysis/listening, gain revision, rerender,
reevaluation and editable project preservation in both protocols with/without
restart between rendering and evaluation. It checks independent encoded hashes,
submitted PCM, measured level changes and recovered native sample gain.

The existing tool declaration test also checks optional digest schemas. None of
these native fixtures have been compiled or executed. Source diagnostics do not
establish rendering correctness, real provider judgment, complete autonomous music
production, performance, resource usage or project/plugin/GUI compatibility.

### Native export range and codec control (2026-10-13)

The headless renderer previously forced whole-song export, supplied fixed codec
settings and left the song's prior repeat count active. It now exposes native
`exportLoop`, `betweenMarkers`, `loopCount`, `bitRate`, `stereoMode` and
`compressionLevel` through `audio.render`. Defaults are explicit per request.
Export flags and count are restored after success or failure. Codec-specific
options are rejected on other formats; FLAC compression is rejected when the
build lacks support, and the native FLAC device now checks the library's
acknowledgment. That result follows the documented
[libsndfile compression contract](https://libsndfile.github.io/libsndfile/command.html#sfc_set_compression_level).
The bitrate range preserves native CLI 64..384 admission, including choices absent
from the GUI's shortlist. OGG retains LMMS's existing bitrate-to-VBR-quality mapping;
results distinguish it from MP3 CBR and do not claim measured encoded bitrate.

`Song::exportRange` centralizes native song/marker, tail and repetition calculations.
The native renderer uses it after setting export state and updating length, thus
retaining exclusion of muted tracks and clips. Headless admission also uses it;
the successful receipt records the range actually accepted after native device
setup, because setup can refresh clip lengths. Effective repeated ticks and
progress use 64-bit arithmetic; returning to a marker range's first tick preserves
completed-pass progress rather than resetting it to zero. The receipt encodes the effective count as a decimal
string. Native final empty ranges, invalid active markers and end-position overflow
fail rather than yielding a misleading export. These changes affect shared GUI/CLI
rendering and require those regressions to run too.

Two native repeat-path issues were addressed in source. Remaining export passes
are now consulted only during export, so stored repeat settings cannot alter normal
playback. Timeline jump aggregation now forwards synchronously on the caller's
processing thread; existing direct sample, LFO and VST handlers therefore receive
the discontinuity before the next tick instead of waiting on the application event
queue. GUI receivers retain Qt's normal cross-thread delivery behavior.

Ten cases / 44 executions are authored in `StudioExportSettingsTest`, with a
600-second timeout. Twelve rows exercise ranges in two meters and wide repetition
counts; four decode native PCM to check section order, repeats, duration and tail;
seven cover cancellation, timeout, publication/setup errors, invalid markers,
codec mismatch and an empty range after mute exclusion. Fourteen codec rows cover
FLAC compression extremes and OGG/MP3 settings at 64/96/320, including three MP3
stereo modes. Other cases exercise invalid options and declarations, native device
setup changing range plus muted extent exclusion, ordinary playback, synchronous
processing-thread jump delivery, completed-pass/wide-length progress and tick
overflow refusal, and session history/undo/redo/replay with and
without reopen. Optional encoders/FLAC control or MP3 decoding can cause explicit
skips in their codec rows; authored counts do not imply executed coverage.

No native test, rendered PCM, GUI regression, codec compatibility or CPU/RSS result
has been obtained. Buffer rounding, encoder delay, native effect tails and transport
stop behavior remain native semantics. Complete audio/project/plugin compatibility,
real evaluation and the full autonomous cycle
remain outstanding. These controls do not establish completion of the commission.

### Native individual-track export without mute isolation edits (2026-10-13)

`audio.render` now accepts optional `track` and `container` selection for one native
Instrument or Sample stem. Without a selection it renders the full mix. A fresh
headless request requires a currently unmuted native source in Song or PatternStore;
the latter renders that row across the existing song pattern arrangement. The
receipt adds `stem` selection metadata, retaining native codec/range controls,
atomic per-file publication and the published encoded digest. Agent declarations
and instructions expose this through the existing operation: inspect both native
inventories and render each eligible source with a distinct path and request ID.
There is no directory-wide transaction or new external rendering engine.

The old native manager isolated stems by changing every candidate's mute model,
then unconditionally unmuting those candidates at completion. Automation could
unmute excluded sources, and isolation could disturb native mute/solo state.
`TrackRenderScope` now installs transient atomic flags under the native model
guard. Its owner-thread admission rejects invalid, nested or foreign-thread
selections, cooperatively checks cancellation while collecting track references,
and publishes flags only after all admission/allocation succeeds. QPointer members
permit cleanup if a native track has already been destroyed. The scope remains
alive until rendering stops and is shared by StudioAudio and RenderManager.

Effective track mute and each InstrumentTrack/SampleTrack audio bus honor exclusion
only during native export. Bus exclusion matters because an effect can emit audio
without a new note/sample. The selected source keeps its own mute automation;
automation tracks, PatternTrack wrappers, mixer routing, sends and master effects
remain active. No filter flag is serialized, journalled or copied into the mute
model. Native solo snapshots and instrument replacement now read the raw model
mute, and the previous-solo mute member is initialized. Normal playback sees no
filter after export. These shared model/processing changes require a native rebuild
and GUI, CLI and plugin regression execution.

RenderManager retains its native eligible-source ordering and output names. It
snapshots unmuted eligibility once, then uses that admission for subsequent stems.
Mute automation from an earlier export can leave a later source muted; the selected
source therefore contributes its arranged extent before automation starts, while
its mute model continues to control audio. New headless requests independently
check current mute state. Automation retains normal native final model values;
the filter does not restore or overwrite them. Marker ranges provide aligned stem
lengths; whole-song extents still include relevant native automation/pattern data.
Shared or nonlinear processing means stems need not sum to the full mix.

Ten cases / 21 executions are authored in `StudioStemTest` with a 600-second timeout.
Four rows check song/pattern and instrument/sample selection, two exercise genuine
solo restoration, and two decode native sample PCM through selected effects,
automated mixer volume and master processing while excluded bus effects stay idle.
Other cases cover invalid/nested/foreign-thread admission, four failure paths,
native bulk stems followed by a full mix, two manager setup failures, bulk mute
automation preserving eligibility/extent and excluding an initially muted source
that becomes unmuted, two history/replay/reopen rows, and scripted stale-selection
rediscovery in both provider protocols. Instrument rows exercise native selection
state rather than rendered plugin audio. Existing receipt/history behavior remains
per request; historical replay never regenerates a deleted output.

All native fixtures remain uncompiled/unrun. No rendered artifact, actual provider
judgment, CPU/RSS measurement, GUI/CLI acceptance or complete project/plugin
compatibility proof was obtained. Source diagnostics and authored fixtures do not
establish the commission's complete autonomous DAW requirement.

### Native runtime audio backend selection and retained recovery (2026-10-13)

`audio.backend.select` adds exact-name runtime selection of a compiled native audio
factory using its loaded settings. `AudioEngine::createAudioDevice` is shared with
the existing startup factory loop, which retains native probe ordering and dummy
fallback. Runtime selection does not probe alternatives or change MIDI. It requires
the owner thread, stopped/disarmed transport and no render/device reservation,
including transient stem admission. Sample/automation arming, reserved/active/pending
takes and MIDI note recording are checked through native models.

The engine stores the prior device/running state before changing it. A transition
guard rejects reentry throughout stop, construction, adoption, enabled audio-bus
port registration, quality/rate notifications and reported startup. Joins occur
outside the model mutex. Successful adoption updates the reported/requested name,
clears render-only mode and retires the stopped old device. It does not start song
playback or rewrite the loaded configuration. Cancellation is checked between native
phases. Failure restores the prior owner; restoration failure retains the existing
reservation for `audio.device.restore` and refuses another selection until recovered.
The reservation still handles stopped backends and failures after partial adoption.

The operation is transient in project history. Its receipt persists and replays
without device side effects, including after an engine/process restart. Fresh
startup still follows its explicit startup/configuration choices. New observations
are required to know current backend state. Native project checkpoints, provider
work and model handles remain owned by the same studio runtime.

Two concrete native lifecycle dependencies required attention. AudioJack can own
the connection used by MidiJack, which retains a raw AudioJack pointer; selection
refuses retirement of that shared owner until an independent MIDI startup selection
is used. General runtime MIDI rebinding remains required work. SDL previously called
global `SDL_Quit` in every device destructor and left device handles uninitialized
on partial construction. It now initializes handles, cleans failed construction,
uses paired audio subsystem references, closes only owned devices, checks startup
status and guards absent/stopped capture. This follows the documented
[SDL2 reference-counting contract](https://wiki.libsdl.org/SDL2/SDL_Init) and
[recursive audio-device lock contract](https://wiki.libsdl.org/SDL2/SDL_LockAudioDevice).

The API audit also found 63 calls to private AudioEngine start/stop helpers across
37 existing fixture files. Those calls now use the public AudioDevice methods to
which the private wrappers delegate. Source validation rejects the same private-call
pattern. This removes observed access errors; it is not a native compilation result.

Nine cases / 47 executions are authored in `StudioBackendSelectionTest`: two real
Dummy selection/playback rows, 24 injected lifecycle/cancellation failures, three
failed rollbacks with explicit retry, eleven invalid/busy/foreign-thread refusals,
reentrant callbacks, a device stop joining native DSP, two native render/history
rows with/without engine restart, two scripted agent protocols, and one optional
SDL dummy-driver lifetime/playback case. The process harness additionally selects
and replaces native Dummy devices, rejects selection during playback and verifies
historical replay both before and after process restart. The catalog now contains
138 native operations; inference declarations retain their 128-tool bound.

All native cases and process scenarios remain uncompiled/unrun. Exact factory
selection does not guarantee a particular physical device: backend-specific fallback
semantics remain. The prior owner retains open handles until commit, so exclusive
drivers may require an explicit switch to Dummy before reopening. Live sample-rate,
period-size and backend-specific configuration editing are not exposed by this
operation. Driver/plugin calls can block cooperative cancellation, and native
running flags are not universal hardware startup acknowledgments. PulseAudio
readiness is addressed in the following source change. Hardware, MIDI
rebinding, GUI coexistence, performance and full autonomous DAW proof remain
outstanding; this change does not establish completion of the commission.

### Native PulseAudio readiness, failure and worker ownership (2026-10-13)

The previous PulseAudio implementation returned from start as soon as it launched
its thread. It treated context connection as successful stream startup, ignored
stream and write failures, did not wake a blocked mainloop on stop, leaked partial
resources on allocation failure, and ran native DSP without an output on connection
failure. Its configured sink was unused and its latency calculation multiplied
seconds by a milliseconds constant.

`AudioPulseAudio` now waits for stream readiness with a ten-second deadline and
cooperative checks. Failure publishes the existing atomic `AudioDevice` status,
including the native error number, so runtime backend selection can restore its
retained previous owner. A worker-owned stream/context cleanup scope unregisters
callbacks and releases every acquired object. An independent stop flag survives
control-thread running-state resets during restart; the old worker is joined
before readiness/semaphore state is reset. Destruction joins as well.

The control thread can wake the mainloop under a pointer-lifetime mutex; all other
libpulse operations remain on the worker. The worker separates prepare, poll and
dispatch, checking stop after prepare because preparation clears previous wakeups.
The mutex is never held during a join. This avoids forced thread termination and
prevents device replacement from freeing a worker's live native state. Readiness
and failure are signalled without polling the server or spinning native DSP.

The native period's exact byte size supplies the latency request. Explicit loaded
sink configuration is honored, while `default` leaves server policy in control.
Playback fills the server's request in bounded native-period chunks, accepts
smaller frame-aligned loans, rejects malformed/null/misaligned buffers, checks
write errors, and cancels a loan when stopped before submission. Native sample,
track, mixer and effect processing still belong to LMMS. Callback entry points
contain exceptions before returning to libpulse. Relevant contracts were checked
against [PulseAudio stream.h](https://raw.githubusercontent.com/pulseaudio/pulseaudio/master/src/pulse/stream.h)
and [mainloop.c](https://raw.githubusercontent.com/pulseaudio/pulseaudio/master/src/pulse/mainloop.c).

A private native-function table permits transport failures in `PulseAudioTest`
without replacing these production lifecycle and DSP paths. Fourteen cases / 53
executions cover partial startup, delayed stream readiness, cancellation, failure
cleanup, immediate restart, blocked-poll and lost-wakeup timing, sink/latency
arguments, native sample PCM through irregular loans, write failures and stopped
borrowing, plus prior-running/prior-stopped engine rollback. The optional real
server row requires `LMMS_TEST_PULSEAUDIO_SINK`; with an isolated `PULSE_SERVER` it
can exercise a test null sink, three start/play/replace cycles and missing-sink
rollback. It is skipped when no sink is opted in and cannot certify physical output.

Source diagnostics pass, but no C++/Qt build, PulseAudio test execution or native
process trace exists in this environment. The deadline cannot interrupt a blocked
native library/plugin call; safe retirement still waits for the worker. Hardware
output/capture, other asynchronous backends, MIDI rebinding, GUI coexistence,
resource measurements and the full autonomous DAW commission remain unproven.

### Native ALSA transfer continuity and joined lifecycle (2026-10-13)

The ALSA worker previously discarded every unwritten suffix after a short write,
ignored fatal write/recovery results, returned success even when recovery prepare
failed, retried suspended hardware without a stop check, and used forced thread
termination for shutdown. Destruction could close a handle without joining its
worker. Constructor descriptor handling indexed an empty descriptor array and
inferred adjacent descriptor numbers belonging to unrelated owners. Hardware
period/buffer query failures could also be accepted as successful setup.

The PCM now opens nonblocking. A startup semaphore waits for native preparation
and worker allocation, with cancellation checks and a ten-second deadline. An
independent stop flag remains valid while a new control-thread start joins an old
failed worker. Transfers keep the current native LMMS buffer and advance only its
accepted frame offset. Short writes, interruption, not-ready results, underruns and
suspend/resume retain that buffer until completion or failure. Native errors are
checked, including recovery prepare, wait, malformed frame counts and exceptions.
Ten seconds without any accepted frames ends processing with `-ETIMEDOUT`; a
successful partial transfer renews that interval. Wait/resume retries use 25-ms
intervals and no-progress retries yield instead of spinning a ready PCM.

Stop signals the retry semaphore, joins the worker, and reports its final native
drop result. All PCM transfer, prepare, resume and drop calls belong to that worker.
No forced termination occurs. Drop errors are transferred back after joining so a
late cleanup result cannot overwrite the running state of a new start. Normal
stop preserves a prior failure; an explicit restart prepares the existing handle
and clears prior status through the AudioDevice lifecycle. Safe join still cannot
interrupt a native library or plugin call that blocks internally.

Constructor scope cleanup owns the PCM and allocated hardware/software parameters
through all failures and exceptions. Parameters and queried sizes are checked,
period direction is initialized, a too-small buffer returns an actual error, and
accepted fallback rate updates the AudioDevice rate. Close-on-exec processing
checks the descriptor count/result and touches only returned descriptors. Native
configuration and plugin behavior remain authoritative, following the
[ALSA PCM API](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html).

`AudioAlsaTest` is conditional on ALSA being compiled. Its private I/O function
table replaces only prepare/write/wait/resume/drop; setup always uses a real ALSA
null PCM. Twelve cases / up to 34 rows cover descriptor canaries, cancellation
after PCM open, preparation failure/cancellation/timeout and retry, exact unwritten
suffixes across errors and recovery, malformed/fatal responses, no-progress bounds,
stop/destruction during wait/suspension, stop failure and explicit restart, late old-worker cleanup, engine
selection rollback, and native sample PCM. One case uses all real entry points
for null-backend selection, transport, restart and replacement. Seven suspension
rows require `ESTRPIPE`; the wide-error row skips on platforms with 32-bit long.

The source validator passes, but these are authored scenarios without compilation
or execution. A null PCM discards output and can advance faster than real time;
preparation does not certify audible output or latency. Recovery can lose frames
already accepted by hardware and can contain gaps. Sndio lifecycle, JACK shared
callback/reconnection behavior, runtime MIDI rebinding, GUI coexistence, native
hardware tests, CPU/RSS measurements and complete autonomous studio proof remain
outstanding. The full commission is still active.

### Native sndio negotiation, transfer and restart ownership (2026-10-14)

The prior sndio constructor started the device, so even an unadopted candidate
could begin playback. The worker discarded partial-write suffixes and ignored
errors, while stop force-terminated a thread without a native stop and destruction
could close its handle before joining. Negotiation did not check signedness or
bytes per sample and did not report an accepted rate difference to the engine.
The native setup widget also discarded its loaded device name on construction.

The constructor now only opens/configures a nonblocking native handle and closes
all partial state on refusal or exception. PCM width, channels, signedness, native
byte order, rate and buffer parameters are checked; the prior integer-percent
rate tolerance is retained with wide arithmetic. An accepted rate becomes the
AudioDevice rate before engine adoption. The setup widget displays its loaded
setting. Native device selection and sndio's own environment-based default remain
in control of routing.

Worker start allocates buffers and descriptor storage, calls the native start API,
and publishes preparation success through a semaphore. Cancellation and a
ten-second deadline apply to the control-thread wait. Playback uses the existing
native LMMS float DSP, checks finite values, converts to signed native-endian PCM,
and retains the same byte buffer through short or temporarily blocked writes.
Offsets are in bytes because a legal partial write may end inside a PCM sample.
Polling uses the library's descriptor count and decoded revents; OS readiness alone
cannot trigger a fresh render. EOF, invalid returned counts, poll failures and
hangup/error events publish the existing atomic device failure status. Ten seconds
without an accepted byte ends processing; wait intervals and retry yields prevent
an empty/ready descriptor path from spinning or advancing native DSP repeatedly.

Worker cleanup calls sio_flush when a compile/link feature check finds that API.
Older libraries retain sio_stop, which drains pending samples. The worker reports
its cleanup result only after the owner joins it, preserving restart status across
late failures. Destruction joins before closing. EOF permanently marks a sndio
handle unusable, so an explicit restart closes/reopens the same configured device
after the old worker completes. Failed cleanup also requires reopen. Reopen must
preserve the current rate; a changed rate requires a new native backend selection
with normal engine notifications. Failed or cancelled reopen frees the new partial
handle and leaves retry possible. These choices follow the
[sndio manual](https://man.openbsd.org/sio_open.3),
[sndio implementation](https://raw.githubusercontent.com/ratchov/sndio/master/libsndio/sio.c),
and [CMake symbol probe](https://cmake.org/cmake/help/latest/module/CheckCXXSymbolExists.html).

sndio's boolean failures do not provide a portable native error code. LMMS uses
negative errno-style categories for them; only OS poll failure preserves an actual
errno. The existing processingFailure/nativeError observation exposes that state.
Constructor failure remains a factory admission failure. Starting prepares the
stream; it does not prove physical playback or measured latency. A nonblocking
write setting does not make open/negotiation/start/stop/close interruptible.

Fourteen cases / 61 executions are authored in `AudioSndioTest`, conditional on
sndio support. Its private transport table drives the production worker and native
LMMS sample DSP through format/cancellation faults, preparation failure, odd-byte
writes, poll control events, hangup and malformed results, no-progress deadlines,
stop/destruction/restart, EOF and failed-stop reopen, changed-rate refusal, failed
reopen cleanup, late old-worker cleanup, engine rollback and signed sample output.
One explicitly opted-in real-device row uses LMMS_TEST_SNDIO_DEVICE to check
transport progression, restart, rejected selection recovery and replacement.
The flush probe and fallback need native build coverage with both library versions.

Source diagnostics are passing, but no C++ compilation, native sndio execution,
physical-output capture or CPU/RSS measurement exists here. Shared JACK lifecycle,
runtime MIDI rebinding, GUI coexistence, compatibility corpora and complete
Interpret/Plan/Compose/Render/Listen/Evaluate/Revise/Preserve execution evidence
remain outstanding. The commission is active and completion remains unproven.

### Native JACK callback admission and partial construction (2026-10-14)

JACK's empty device stop hook allowed an already-running callback to keep using
project state while runtime backend replacement could retire the device. Stopped
callbacks still read MIDI and pushed captured audio. An independent atomic gate
now admits processing before the callback enters native model/DSP work. Stop
closes admission and waits outside the engine model lock for existing leases to
finish. Later callbacks silence master output and skip MIDI, rendering and input.
Start joins the prior processing generation before reopening admission. Ordinary
stop keeps the JACK client active; native deactivation would disconnect its graph
ports, according to the [JACK client API](https://jackaudio.org/api/group__ClientFunctions.html).
Destruction joins processing before native deactivation/close and storage release.

Construction checks registration of both input/output ports and process/size
callbacks, rejects zero rate/initial capacity, and closes partial clients after
refusal or exceptions. Output pointer storage uses automatic ownership. Process
and resize callbacks contain C++ exceptions; processing rejects absent buffers
and frame counts larger than the prepared input storage. Exception recovery
attempts to silence current port buffers without reusing an earlier cycle's
cached addresses. Informational backend output now goes to stderr, preserving
the headless JSONL channel. The legacy conditional external-bus copy uses the
actual native bus capacity and clears the remainder of the JACK output buffer.

The private JACK entry-point table wraps weakjack at call time and changes no
native synthesis or recording path. Eight cases / 31 scenarios are authored for
construction failures, early/failed startup, stopped callbacks, malformed buffers,
C-boundary exceptions, held native DSP during stop/destruction, sample playback
and input recording, prior-device restoration, and an optional real-server row
selected by LMMS_TEST_JACK=1. Native compilation and execution remain unavailable;
source diagnostics are not runtime evidence.

This work does not complete JACK lifecycle recovery. Shared MIDI detach still
needs lifetime synchronization, shutdown still loses the old native handle, and
automatic reconnection needs to recreate shared MIDI ports safely. The external
bus map/routing, JACK and weakjack builds, real-server callbacks and physical
capture remain unverified. Joining cannot interrupt a blocked plugin/native call.
CPU/RSS, GUI coexistence, runtime MIDI rebinding and the full autonomous DAW
acceptance traces remain outstanding; the commission is active.

### Native JACK shared MIDI lifetime and reconnect (2026-10-14)

The shared ownership defects identified in the preceding source pass are now
addressed in source. JACK shutdown no longer nulls the owned client pointer or
emits a Qt signal from the native callback. Lock-free flags and admission gates
publish loss; owner-thread health polling performs failure reporting, notification,
retirement and recovery. The old handle remains owned until native close joins
its callbacks. A separate MIDI reader gate covers the pointer snapshot, so detach
joins MIDI readers through that gate. Native port retirement can still block
inside JACK. Port registration
precedes publication, and either audio or MIDI can be destroyed first without
leaving the surviving object linked to the retired owner.

Recovery reopens an existing JACK server using the original configured client
name, with one second between attempt starts. It rebuilds shared MIDI ports and
keeps an exact sample rate; a different rate requires normal device selection.
Stopped devices and export reservations suppress automatic DSP restart. Failed
construction, activation, port recreation or rate admission leaves retry possible.
Standalone MIDI uses the same retirement discipline and drains shutdown events
delivered during old-client close before admitting a replacement. A MIDI-only
failure can rebuild its port without retiring the audio client. The idle MIDI
QThread and forced termination have been removed; backend admission and native
input observations now use checked availability.

JACK's shutdown restriction and native packet rules were checked against the
[callback API](https://jackaudio.org/api/group__ClientCallbacks.html) and
[MIDI API](https://jackaudio.org/api/group__MIDIAPI.html). Input validates a whole
native block before delivering scalar channel messages through the existing raw
parser and native owner-thread queue. Lost events, invalid buffers/counts/times,
malformed bytes, native access errors and exceptions stop admission and latch
per-port failure. A fixed scratch array permits up to 1024 events; recovery of
delivery still requires midi.input.reset after the backend is healthy. Health
readers hold the same lifetime gate while inspecting a shared audio owner.

JackLifetimeTest authors fourteen cases / 47 scenarios. Controlled JACK calls use
the production audio/MIDI lifecycle, real native DSP, raw parser, MidiPort queue
and processor dispatch. Scenarios cover partial acquisition, early publication,
malformed-block rejection, held readers/DSP, both destruction orders, reconnection
faults, rate preservation, late old callbacks, retry spacing, stopped/export
reservations and MIDI-only recovery. Two opt-in LMMS_TEST_JACK=1 rows exercise
real shared retirement. All 47 new scenarios and the prior 31 JACK scenarios are
uncompiled/unrun; source diagnostics do not establish native behavior.

The environment still lacks the compiler, Qt build tools, executable and all
17 initialized submodules. External graph/MIDI connections after server loss,
conditional external-bus synchronization/routing, runtime MIDI rebinding and
JACK MIDI output remained incomplete at this source checkpoint; the next section
records its subsequent implementation and remaining limits. The original raw backend also does not
support system/SysEx input, and queued input does not preserve sample offsets.
Health polling is nominally 100 ms with event-loop delays; native calls and joins
cannot be forcibly interrupted. No physical sound/capture, real-time latency,
CPU/RSS, GUI coexistence or full autonomous studio acceptance evidence exists.
The complete commission remains active.

### Native JACK MIDI output and cancellation (2026-10-14)

The former empty JACK output path now creates a native MIDI output port alongside
the input port. Native MidiPort track/instrument routing feeds the existing
channel-message encoder into a fixed 1024-event queue. Enqueue records source
identity, native frame time, delay and stable order; callbacks never dereference
the source model. Delay conversion snapshots tempo/sample rate and refuses a
delay beyond the signed half-range of JACK's 32-bit frame clock. Callback sorting
handles wrap and overdue events. Tempo changes do not retime queued delays.

An independent output lifetime gate joins producers and native buffer users
before port retirement. Shared callbacks submit after native DSP without holding
the MIDI lifetime lease across DSP. Early failure or stopped/export-reserved audio
still clears the output buffer, discards musical events and attempts cleanup.
Stop itself discards the schedule and advances an enqueue generation. A producer
admitted before stop cannot enqueue after an immediate restart.
The output callback attempts the queue mutex without waiting; contention can
delay messages. It clears each native buffer before any write, checks writes,
and removes a normal-message prefix when a later write fails. Output errors are
visible through midi.inspect.outputBackend, including the last native error
after successful recovery. Other backends report output observation unavailable
until they implement that observation; this field does not infer hardware health.

Cancellation removes the retiring source's schedule and releases channels that
may have received notes/pedals. Four cleanup controllers retain delivery progress
across capacity-limited buffers. A failed unregister closes the owning audio
client before replacing MIDI ports; recovery can then rebuild the owner. Reconnect
drops the old event schedule and retains pending channel cleanup. Actual endpoint
delivery cannot be guaranteed across shutdown or port retirement, and cleanup on
a shared external channel can affect another source using that channel.

The existing lifetime fixture gains four output-port acquisition/reconnect rows
(fourteen cases / 51 scenarios). Eighteen output cases add 45 scenarios: bytes
and ordering, native source routing, native-clock wrap, enqueue-tempo semantics,
delay/packet validation, cancellation, retired-address reuse, overflow, native
faults, partial cleanup, queue contention, stop/export/immediate restart, reconnect, failed native
unregister, concurrent producers and output-reader joining. An optional real JACK
receiver checks note and cleanup packets. The combined fixture is 32 cases /
96 scenarios, all uncompiled/unrun. The prior AudioJackTest remains eight cases /
31 scenarios, also unrun.

API rules were checked against the official
[JACK MIDI API](https://jackaudio.org/api/group__MIDIAPI.html) and
[timing API](https://jackaudio.org/api/group__TimeFunctions.html). These sources
support the buffer/clock contract, not proof that this implementation runs.
No compiler, CMake, Qt build tools or LMMS executable is on PATH, and all 17 pinned
submodules remain uninitialized. Native/weakjack compatibility, physical output,
plugin-generated MIDI, deadline behavior, GUI coexistence and CPU/RSS are unproven.
At this checkpoint, system/SysEx messages, agent-managed JACK destinations, automatic graph restoration,
runtime MIDI rebinding and the legacy external-bus map remain outstanding. Native
close errors and blocked native/plugin calls also limit retirement guarantees.
Full autonomous DAW acceptance remains unproven and the commission stays active.

### Native MIDI graph discovery and studio routing (2026-10-14)

JACK's shared native MIDI ports now expose graph discovery and connection changes
inside the application. Two capabilities, midi.backend.inspect and
midi.backend.route, bring the native catalog to 140 operations while preserving
the 128-declaration inference budget. The agent instructions explain client-wide
scope, input delivery recovery, and the difference between graph verification and
device receipt. Existing per-track subscriptions remain separate because a single
raw JACK output cannot provide independently routed destinations for each track.

MidiClient supplies an optional backend graph interface; unsupported backends
report that explicitly. MidiJack queries native MIDI endpoints by direction,
filters its own ports, validates exact native type/direction, preserves UTF-8
names, sorts/deduplicates observations and checks connections. Native arrays are
released with jack_free through a scope guard. The application refuses scans over
1024 names, names of 4096 bytes or more, and combined names over 256 KiB. Native
JACK allocation happens before those limits. Studio responses page at 128/1024
items and 512 KiB using the established inspection mechanism.

Routing operates only on the current client's own port and one exact external
peer. It runs on the owner thread with event dispatch suspended and refuses
reentry. The studio requires stopped/disarmed transport, no unfinished takes,
export, render context or device transition. Reader joining and cleanup waits run
outside the model lock. Output mutation blocks new musical enqueueing, discards
the old schedule and waits for cleanup plus a subsequent native callback before
changing the edge. Its 250 ms cooperative drain deadline can refuse large-period
or stalled clients; native calls remain uninterruptible. Cleanup is channel-wide
and can release notes even if the routing action later fails.

Input mutation joins readers and resets the merged raw parser. Existing per-port
failure machinery cancels pending delivery and releases previous processor notes;
those readable tracks/controllers require explicit midi.input.reset, including
when native routing was rolled back. A successful route reports the verified
edge, scope and reset requirement. Cancellation, native errors or failed
postcondition checks attempt inverse mutation and verify the old state. Errors
distinguish confirmed restoration from uncertain native graph state.

Connections are process-local external state. They do not create project history
and are neither project-saved nor automatically reconnected after server loss.
Durable receipts return historical results without repeating graph mutations.
This scope prevents a replay from silently reconnecting an endpoint; automatic
restoration and per-track JACK ports remain unfinished work, not claimed support.

Fifteen new graph cases / 47 scenarios cover shared/standalone discovery, paging,
UTF-8/size limits, array cleanup, type/direction validation, idempotence, native
errors and rollback uncertainty, input reset, output cleanup acknowledgement,
timeout/cancellation, foreign and nested calls, studio admission guards, session
and backend restart receipts, and scripted Chat/Responses provider paths. The
combined JackLifetimeTest now has 47 cases / 143 scenarios. Its optional real
receiver uses the studio graph API to connect, observe and disconnect. The JSONL
process harness adds unsupported-backend observations, refusal/replay and project
history checks. All remain uncompiled/unrun; the controlled transport does not
prove a real server, device, plugin or provider works.

Native API contracts were checked against the
[JACK port API](https://jackaudio.org/api/group__PortFunctions.html) and
[lookup API](https://jackaudio.org/api/group__PortSearching.html). Source checks
cover 376 changed/new C++ files and 140 native operations. No compiler, CMake,
Qt build tools or LMMS executable is on PATH; 17/17 pinned submodules remain
uninitialized. Native/weakjack execution, hardware delivery, graph race behavior,
GUI coexistence, performance and the full autonomous production cycle remain
unverified. The complete DAW commission remains active.

### Explicit native MIDI reconnect preferences (2026-10-14)

This checkpoint extends the graph interface with `midi.backend.reconnect`,
bringing the native catalog to 141 operations. It retains an already connected,
validated exact endpoint for process-local recovery, or forgets the preference
without changing the graph, including while offline or playing. A successful
explicit disconnect also forgets its preference; a failed disconnect preserves
it. Idempotent command replay returns its historical receipt and never recreates
a forgotten preference. The agent instructions and both scripted inference
protocol fixtures now include explicit retention.

The native owner timer rotates through retained routes, attempting at most one
due entry per 100 ms tick and waiting at least one second between attempts for
that entry. A common AudioEngine admission check protects both manual routing and
recovery from playback, pause, armed/in-flight sample/automation/MIDI recording,
export reservations, device transitions and active render scopes. Automatic
recovery also requires this exact current MIDI backend and, for a shared JACK
client, its current audio owner. No Qt event dispatch occurs during recovery.
The model lock is released before callback joining, cleanup waits or graph calls.
Native calls and joins can still block; this is not a hard timing guarantee.

Preferences are bounded to 64 routes and 64 KiB of UTF-8 endpoint names across
both directions. Discovery merges missing retained names with live endpoints,
reporting availability, current connection, retention, pending recovery, last
attempt error and admission deferral separately. Offline results are explicitly
unobserved. A newly created native JACK client can recover retained routes, but
backend-object destruction/application restart forgets them. Project loading
does not recreate or clear process-local preferences. Exact native names may
identify replacement devices; there is no durable physical device identity.

Recovery reuses the existing native graph mutation/verification/rollback path.
Input changes keep affected deliveries failed until explicit reset. Output
changes discard old scheduled events and drain cleanup before touching the old
graph. Because those releases cannot reach a missing destination, a successful
automatic reconnect queues pedal/note releases on all sixteen raw channels.
New musical output is refused until all releases have been submitted; small
native buffers preserve cleanup progress. This can release notes on other
destinations sharing the output. Existing output-backend observations distinguish
pending cleanup/activity from graph connectivity; neither establishes external
receipt or audible hardware output.

Eleven additional reconnect cases / 46 scenarios cover preference preconditions
and bounds, shared/standalone input/output restart, missing endpoints, twelve
admission deferrals and subsequent recovery, native failures, explicit input
reset, output cleanup before/after reconnect, one/three/64-event buffer capacity,
forgetting while offline/playing, fair retry spacing and receipt replay. The
combined JackLifetimeTest now has 58 cases / 189 scenarios, including 26 graph
cases / 93 scenarios. The process harness also checks unsupported reconnect
preferences and replay without a project revision. These are authored fixtures,
not successful native executions.

The native API contract was checked against the
[JACK port API](https://jackaudio.org/api/group__PortFunctions.html), including
direction/type preconditions and positive connection observation. Source-only
validation covers the catalog, interfaces and authored fixtures. No native C++
configuration, compilation, JACK restart/hotplug run, real provider trace,
physical audio, CPU/RSS or full autonomous production-cycle evidence is available.
The previously recorded missing toolchain and 17 uninitialized submodules still
prevent native execution. The complete application commission remains active.

### Native master audio graph control (2026-10-14)

The active JACK master input/output graph was still configured through GUI
settings. `audio.backend.inspect` and `audio.backend.route` now expose those
native channels through the same internal capability/agent runtime, bringing
the catalog to 143 operations. This does not substitute for the legacy per-track
external-bus path: its `AUDIO_BUS_HANDLE_SUPPORT` macro is not enabled anywhere
in the inspected build configuration, and its buffer alignment, registration
and callback/map lifetime still require implementation and native verification.

AudioDevice now has default unsupported graph observations/mutations, with JACK
implementing master endpoint discovery and one-edge changes. Discovery checks
UTF-8/type/direction/ownership, frees the native list on every path, bounds it to
1024 names/256 KiB, and returns zero-based native channels and connected channel
indices. Studio results page endpoint arrays under a 512 KiB budget and expose
loaded startup targets separately, with explicit truncation after 1024 UTF-16
units. Offline observation and unsupported backends remain explicit. Audio and
MIDI share the same native graph admission guard and exact-name validators.

Graph mutation requires the current device, idle owner thread, stopped/disarmed
transport, completed recording takes, and no active export/device/render scope.
The device pauses and joins DSP/capture outside the model lock. Input attempts
invalidate queued capture even when later rolled back. An unchanged edge is
idempotent. Native failure/cancellation attempts inverse mutation and verifies
the old state. Confirmed rollback restores the prior processing admission;
unconfirmed restoration leaves processing stopped and disables timer restart
until an explicit start. Native calls and joins remain cooperatively bounded.
Stopped callbacks can clear master output and drain/cancel shared MIDI output,
so graph changes can interrupt that client. No hardware receipt is inferred.

Graph edits do not change project revisions or loaded startup settings. Server
recreation still applies those settings, whose names now use UTF-8; live manual
edges are not automatically retained. Historical receipts never reconnect or
disconnect the current graph. Agent instructions explain native channel indices,
the separate startup policy, capture invalidation and uncertain-restoration
behavior. Chat and Responses fixtures use the new operations through the actual
StudioRuntime dispatcher. The JSONL harness checks unsupported observations,
failure/replay and unchanged project revision.

Eighteen new audio graph cases / 66 scenarios are authored: discovery and
paging, invalid peers/schema/channel indices, native mutation/observation faults,
verified versus uncertain rollback, cancellation, input invalidation, callback
joining, shared MIDI cleanup, owner/reentry guards, offline/unsupported results,
twelve reservation refusals, existing-edge races, startup/replay separation,
scripted agents, queued device identity and native sample PCM after graph changes.
Queued audio routes bind to the JACK QObject instead of a mixer channel; mixer
edits do not invalidate device channels, while device replacement clears the
target and invalidates stale queued work. An optional
`LMMS_TEST_JACK=1` case registers independent real JACK audio source/sink clients,
routes both directions through studio commands, observes native rendered PCM
and checks a captured sample. The full JackLifetimeTest now authors 76 cases /
255 scenarios. The existing AudioJackTest remains eight cases / 31 scenarios.
None of those native fixtures has been compiled or executed here.

The API review used the [JACK port documentation](https://jackaudio.org/api/group__PortFunctions.html)
and [callback documentation](https://jackaudio.org/api/group__ClientCallbacks.html).
Current environment inspection still finds no CMake, Ninja, compiler, Qt build
tools or LMMS executable on PATH, and all 17 pinned submodules are uninitialized.
Native/weakjack builds, actual JACK trace, hardware/audio latency, GUI coexistence,
plugin compatibility, CPU/RSS and the complete autonomous production cycle remain
unverified. Full application completion remains unproven and the goal is active.

### Native bus output buffering (2026-10-14)

The disabled JACK bus path previously copied `AudioBusHandle::m_buffer` before
native DSP, starting at frame zero and copying only the smaller of callback and
period size. That cannot preserve the master's one-period delay, partial-period
cursor or callbacks spanning several periods. Native port registration and
retirement also run from track model edits, where joining a DSP callback that
needs the model mutex can deadlock. The macro remains disabled. This change
implements the native audio-buffer foundation; it does not enable JACK track
ports, publish new routing capabilities, or establish external delivery.

`AudioBusHandle::externalOutput()` acquires a retained, read-only source on the
model owner thread. Its private `AudioBusOutput` storage is allocated once on
first acquisition. The existing default external-output flag alone allocates no
snapshot storage. Each acquired source has two stereo periods: the engine swaps
and clears them with the master buffers, and the native bus job publishes its
finite output after track volume, panning and effects, before mixer sends,
mixer effects and master gain. Effect tails use the same path. Muted and
render-excluded jobs clear their working buffer before returning, while their
new external write period stays silent. Normal mute/exclusion transitions share
the master's existing period delay.

The planar `AudioEngine::renderNextBuffer` overload takes the master destination
and a span of stereo `BusDestination` entries. It validates all channel pointers,
stereo layouts and frame counts before consuming audio. Every destination uses
the same native period and read offset, including mono or multichannel master
conversion, partial callbacks, and callbacks larger than one period. Multiple
destinations may retain the same source. Null sources produce silence. Callers
must supply disjoint, sufficiently sized writable buffers and retain source
ownership before entering a callback; buffer capacity and native port lifetime
are the caller's responsibility. There remains exactly one native render
consumer. The engine does not allocate destination storage during copying.

The bus overload holds the model mutex through rendering and copying. Bus
disablement clears both snapshots immediately; re-enablement begins with silent
history. Destruction marks retained storage retired and clears it under that
same mutex before removing the engine job. A destination cannot follow a newly
constructed bus at a reused address. A retired source also remains silent after
engine recreation with a different period size. Registration exceptions during
bus construction remove its published job before unwinding, and bus-list
admission now uses an exception-safe model guard. This protects model/snapshot
ownership only: a future native backend must still safely publish, rename and
retire its own port handles, and must not join DSP while holding the model lock.

Both existing master-only buffer overloads and the new bus overload now silence
an entire callback when a later native period fails, including an earlier
audible prefix. The render consumer tracks the monotonic DSP failure count and
discards both cached periods when it changes. Resetting the failure latch before
the next callback therefore cannot replay audio cached before the failure.
Direct native period rendering uses the same failure synchronization. This
does not interrupt a blocked plugin or prove recovery of private plugin state.

`AudioBusOutputTest` is registered with CTest and authors 17 cases / 68 scenarios:
24 callback-size/master-layout combinations; native track gain/pan/effects and
independent mixer/master processing; mute and stem exclusion; disable/enable and
late acquisition; address reuse; 12 native instrument/effect/mixer fault cases;
six existing master-buffer fault cases; eight invalid destination layouts;
effect tails; null/zero-frame calls; owner admission; held-render retirement;
four failure-epoch/reset cases; engine recreation; render reentry; and standard
or nonstandard registration exceptions. Tests use actual native engine jobs,
models and signal processing with controlled play handles/effects. No JACK
callback, server, physical device or inference provider is exercised by them.

The storage calculation is two stereo periods, or 4096 sample bytes for a
256-frame period, per acquired bus, excluding container/allocation overhead.
That storage remains until bus and retained-source ownership ends, and snapshots
continue advancing after acquisition even if no backend currently uses them.
The additional copies and whole-callback model lock have not been profiled for
latency, contention, CPU or RSS. No native test has been compiled or executed:
CMake, Ninja, C++/Qt tools and LMMS remain absent from PATH and all 17 pinned
submodules remain uninitialized. Source validation passed across 379 changed/new
C++ files and 143 catalog operations; whitespace and Python syntax checks also
passed. These are delimiter, registration and consistency diagnostics, with no
native compilation or execution. Native
port integration, GUI coexistence, plugin/project compatibility and the full
Interpret → Plan → Compose → Render → Listen → Evaluate → Revise → Preserve
execution still require implementation and verification. The goal remains active.

### Native JACK track ports and studio routing (2026-10-14)

This continuation connects the retained native bus buffers to actual JACK track
port registration and callback code. It replaces the dormant
`AUDIO_BUS_HANDLE_SUPPORT` map with `JackAudioBuses` and adds
`audio.bus.inspect`, `audio.bus.configure` and `audio.bus.route` to the native
studio catalog. The full LMMS commission remains active. This is source
implementation progress, not a completed or demonstrated headless DAW.

Song and PatternStore instrument/sample tracks stage output intent in their
existing AudioBusHandle hooks. Preview/internal buses are excluded. Model hooks
perform no native port calls or callback joins. The device owner prepares
enabled buses before initial activation/client recreation and otherwise
reconciles pending requests through stopped/disarmed admission. A thread-local
model-edit depth now prevents backend joins from an enclosing native edit;
MIDI capability admission releases its brief model guard before checking that
shared admission. Explicit configuration also resynchronizes idempotent model
flags with the retained device's desired state.

The owner publishes immutable port/source snapshots with preallocated callback
scratch. A single-reader hazard protects the whole JACK callback, including
buffer clearing when DSP is stopped. The callback reacquires native buffers
each cycle and asks AudioEngine to copy bus and master output with the same
period cursor. Snapshots retain source identity rather than raw model pointers;
track deletion silences and atomically retires the source under the model lock.
Old snapshots and their retained storage are released by the owner after reader
exit. Native port names use stable per-device numeric identities. Inspection
reports the current label separately; SampleTrack now propagates renames to its
audio bus, matching the existing instrument-track behavior.

Registration allocates snapshot storage first, validates both stereo handles
and cleans up partial ownership. Disabling first publishes silence and, for an
active client, waits for native callback clearing. It then disconnects and
observes all edges, publishes an omission, joins old readers and unregisters
both handles. Failed disconnection or a stalled clearing callback leaves owned
silent ports pending. An uncertain unregister result closes the entire owning
audio/shared-MIDI client and disables timer restart until explicit start.
Cancellation restores the previous enable request before native disconnection
begins; retirement finishes after that boundary. Inspection distinguishes
requested, published and pending state and reports native errors.

There are 256 published stereo bus slots per device. Further tracks remain
editable with pending requests. Healthy polling attempts at most eight due
records per tick with at least one second between retries per record. Active
retirement has a cooperative 250 ms callback-clearing deadline. Native calls
and reader joins can still block, including on a stuck plugin; this deadline
does not bound the whole operation. Initial/recreated client preparation is
separate from stopped/disarmed reconciliation on an already healthy client.
Bus storage retains the earlier two-period allocation/copy costs, and neither
that cost nor the snapshot publication path has been profiled.

The three studio operations bind queued intent to both the track and selected
audio device. They preserve project revision/undo history. Enable requests and
bus IDs have native bus/device lifetime; client recreation keeps desired buses
but loses their graph connections. Project serialization and historical receipt
replay do not restore those requests or edges. Graph observation can race other
JACK clients and does not establish external receipt or physical sound.

`JackLifetimeTest` adds 19 cases / 70 scenarios: delayed master/bus alignment at
five callback sizes with changing native buffer addresses; duplicate/long labels;
model-edit and preview exclusion; running/stopped retirement with shared MIDI;
held snapshot retirement and track address reuse; six partial-registration and
five retirement-failure paths; stalled callbacks; six registration-cancellation
and two retirement-cancellation rows; fourteen native admission conditions; five
DSP/buffer/capture faults; client recreation; the 256-bus boundary; three queued
selection changes; history/receipt replay; scripted Chat/Responses execution;
twelve invalid/unsupported targets; and an optional independent real-JACK sink
receiving native sample-track PCM with master volume zero. The combined fixture
now has 95 cases / 325 scenarios, in addition to the retained buffer fixture's
17 cases / 68 scenarios. The JSONL process harness adds unsupported bus
inspection/refusal and receipt replay checks.

Source diagnostics passed across 381 changed/new C++ files and 146 native
catalog operations; whitespace and Python syntax checks passed. These checks
are lexical, registration and consistency checks. No native C++ was configured,
compiled or run, and no native fixture, real JACK server, physical device or
inference provider was exercised. The latest environment audit still reports
the absent native build toolchain and 17 uninitialized submodules. Full native
build/test execution, GUI/headless coexistence, plugin/project compatibility,
resource and audio measurements, and the complete autonomous production cycle
remain unproven. This continuation made source progress; the goal is not blocked
or complete.

### Native MIDI client transfer preparation (2026-10-14)

The previous continuation made source progress on JACK track ports. This
continuation addresses the ownership prerequisite for runtime MIDI replacement.
Existing MidiPort models held permanent pointers to their original MidiClient;
changing the engine's selected pointer alone would strand track/controller
models on the old backend. Input delivery also retains queued callbacks for
individual registrations. The complete LMMS objective remains active.

`MidiClient::transferPortsTo` is a private engine-facing handoff primitive. It
requires idle owner admission, stopped/disarmed transport, joined native audio,
an idle source and an empty replacement. It snapshots existing port identities,
refuses active input processors/recovery, and pauses both old and staged input
deliveries. Already queued input is discarded and readable deliveries latch
recovery. It releases tracked output notes/pedals through the original client
and cleans local input voices before preparing the replacement. Output attempts
and conflicting port routing mutations are suppressed/refused during handoff.

The replacement registers every existing model, prepares mode/name resources,
copies available endpoint inventories, reapplies present requested names and
validates readiness before any client pointer changes. Absent requested names
remain in the model for later discovery; raw replacements retain this intent
without exposing a named inventory. Native model objects, parameter values,
output source, channel/controller choices and serialized routing intent are
preserved. ALSA sequencer preparation additionally checks that each enabled
direction has its native port ID. These checks do not upgrade existing void
subscription methods into verified external connection acknowledgements.

All model pointers and inventories commit before old native registrations are
removed. Old input registrations are made inactive before native removal, so
queued or late old deliveries cannot follow the new binding. Replacement
signals connect to the existing models and view notifications are queued for
the caller's eventual return to the event loop. The caller must immediately
retire the source client after commit. A prepared candidate is discarded after
failure, including failure to clean up its native resources. Base registrations
are revoked before native cleanup so a candidate destructor cannot invalidate
or deliver through the retained original binding. Source retirement exception
counts describe thrown errors only, not verified handle closure or physical
MIDI receipt. Cancellation is cooperative through preparation; committed
retirement completes without a new cancellation boundary.

Failure can preserve the original binding while leaving its input latch set.
Discarded events, note releases and schedules are not replayed on rollback.
Explicit input reset still requires supported, observed backend health for a
backend fault. Already cleaned local voices do not receive a delayed synthetic
cleanup after a successful commit. Raw byte parsers and operating-system queues
remain backend-owned; this change does not prove a physical cutover timestamp.
JACK output handoff uses its existing cleanup submission and subsequent-cycle
observation, with a cooperative 250 ms deadline. A stalled callback or cleanup
failure refuses publication. Native calls and joins are not forcibly bounded.

Review also found that a MidiPort constructor exception after registration
could leave a live input registration for an incompletely constructed object.
The constructor now revokes that registration and attempts native cleanup before
unwinding. Client destruction invalidates only models still bound to that
client, rather than a model staged for another owned backend.

`MidiClientTransferTest` authors 18 portable cases / 74 scenarios, including all
raw/named source-target combinations, all four port modes, multi-port publication,
22 preparation-failure rows, 12 cancellation rows, three cleanup failures,
concurrent and nested-dispatch input, output suppression/releases, mutation
reentry, active input refusal, model deletion, failed retirement/rollback cleanup,
six constructor failures, twelve owner/reservation refusals, actual native song
and pattern instrument tracks plus MIDI controllers, endpoint rediscovery and
empty-client health checks. Three additional JACK cases / eight scenarios use
the private native API table for shared/standalone cleanup, stalled-cycle retry
and boolean/exception cancellation. `JackLifetimeTest` now contains 98 cases /
333 scenarios. These fixtures are authored, not compiled or executed.

Source diagnostics passed across 382 changed/new C++ files and 146 native
operations, with whitespace and Python syntax checks. A fresh environment audit
found no CMake, Ninja, C++ compiler, Qt build tools or LMMS on PATH and all 17
submodules still uninitialized. No C++ build, native test, real backend migration,
GUI session, physical MIDI/audio or provider execution occurred.

Runtime MIDI selection remains unfinished: the engine still needs exact
candidate factory creation, selected-client metadata publication, ownership and
audio stop/restart recovery around this primitive, a structured studio command
and receipt/selection behavior. Existing GUI raw/named checks use the engine's
client during view construction and also need integration review. End-to-end
backend replacement and the full autonomous production/compatibility/resource
acceptance remain unproven. This turn made source progress; the full goal stays
active and is neither blocked nor complete.


### Native runtime MIDI backend selection (2026-10-14)

This continuation connects the preceding private port-transfer prerequisite to
AudioEngine ownership and the studio runtime. `midi.backend.select` now selects
one compiled factory using its loaded native settings. Startup uses the same
factories while retaining startup probing order and its explicit Dummy fallback;
runtime selection never substitutes another factory. The operation is process-local,
does not save startup preferences or create a project revision, and reports the
selected/current backend separately from startup failure history.

Admission requires the owner thread outside model edits, stopped/disarmed transport,
completed takes and no export/device reservation. A MIDI transition flag rejects
reentry and other device/graph changes. Audio processing is joined before candidate
preparation. The transfer validates all models and native registrations before
publishing their client pointers; the engine then publishes ownership/name metadata
and retires the old client before returning to the event loop. Candidate failure
preserves old bindings, discards prepared native resources and restores the original
audio state. Its discarded input/output and local note cleanup cannot be replayed.

Audio restart follows the MIDI commit. The returned result explicitly distinguishes
`committed` from `audioRestored`; an unsuccessful restart retains the same native
audio device under the existing `audio.device.restore` reservation. Precommit errors
also retain this recovery path if restoration fails. There is no false MIDI rollback
claim after commit, and cancellation is suspended during recovery/result observation.
No quality/rate notifications are emitted solely for this unchanged audio device.

`ClientTransfer` is a distinct input discontinuity (failure code 6). Enabled inputs
still require explicit reset, but intentional transfer alone does not demand an
unavailable native health observation. Existing backend faults keep their health
requirements. JACK port readiness uses native availability while audio is paused;
its input-delivery health still requires running shared audio. Existing GUI track
menus, instrument MIDI controls and controller dialogs retain port models and update
raw/named controls after handoff.

Native MIDI graph edges and reconnect preferences are not recreated. A shared JACK
connection can stage only one MIDI client: selecting Dummy first releases it before
a subsequent JACK selection. Exclusive backends can similarly reject a candidate
while the old client remains open. Old JACK output cleanup must complete before
transfer; no force-abandon option was added. Subscription intent, native graph
observations and retirement exception counts do not prove physical delivery or
universal resource closure. Native calls/joins remain cooperatively interruptible
only where the existing backend implements that behavior.

Authored portable transfer/engine coverage is now 29 cases / 115 scenarios,
including 11 engine integration cases / 41 scenarios. It covers native Song and
PatternStore tracks plus MIDI controllers, raw/named settings, running/stopped audio,
factory/registration failures, cancellation, reentry, failed stop/restart recovery,
late cancellation after commit, receipts/history across restart and scripted
Chat/Responses selection. Six JACK transfer cases / 14 scenarios include shared
candidate preparation while audio is joined, source cleanup/retirement, occupied
shared-client refusal and cancelled candidate cleanup. Combined JackLifetimeTest is
101 cases / 339 scenarios. JSONL live selection/refusal/replay checks are authored.

Source validation covers 384 C++ files and 147 native operations. These are source
checks, not native compilation or execution. All new native/process/GUI fixtures
remain unrun: the eight checked C++/Qt/LMMS commands are absent and all 17 submodules
remain uninitialized. No hardware, real provider, full production-loop, project/plugin
compatibility or CPU/RSS acceptance claim follows. This advances native integration;
the full LMMS commission remains active and completion remains unproven.


### Native raw MIDI lifecycle and observed failure repair (2026-10-14)

The previous goal turn made concrete progress by connecting native MIDI selection
to AudioEngine and the studio runtime. This turn re-read all five commission
attachments, found no applicable AGENTS.md, and refreshed build availability:
cmake, ninja, cl, clang++, g++, qmake, qmake6 and lmms are absent from PATH, with
17/17 submodules uninitialized. Full native build/execution remains unavailable;
no denied Docker/WSL/network/escalation path was retried.

Current-source inspection found unsafe native lifecycle behavior in both remaining
POSIX byte-stream MIDI drivers. MidiAlsaRaw could wait in a ten-second poll while
its destructor waited one second, forcibly terminated the thread and closed handles
without a completed join. Cleanup was conditional on the thread still running,
so construction failures and naturally stopped readers could retain resources.
MidiSndio also forcibly terminated its reader, never closed its native handle,
used blocking duplex I/O and supplied one pollfd even though its API supplies a
required array size. Both ignored output write results and lacked observed input
health. These were source findings; native reproduction was unavailable.

Both native drivers now open nonblocking duplex streams and initialize ownership
before calling native APIs. Partial-open/descriptor failures retire acquired native
resources. Each reader validates a bounded 1..64 descriptor inventory, includes an
owned nonblocking close-on-exec wake pipe, and reads at most 256 bytes per cycle.
Shutdown requests stop, wakes the poll, joins without holding the native I/O mutex,
and closes resources even after an earlier reader failure. Forced termination and
volatile stop flags were removed. Every library handle call is serialized against
other read/write/descriptor operations; the existing raw-client packet mutex still
serializes bytes across output models. Poll itself runs outside the handle mutex.

Interrupted polls and temporary empty reads keep the reader alive. Fatal native
errors latch registered input delivery, stop the reader and expose observed backend
unavailability. Runtime/startup factories use the drivers' explicit readiness
checks. ALSA reports negative API/system errors; sndio uses normalized errno-style
categories because its API exposes EOF rather than a detailed error value. Output
checks every byte result, retries ALSA EINTR at most four times, and raises explicit
errors for backpressure or terminal failure. Temporary output backpressure does not
falsely stop healthy input; terminal output errors invalidate the duplex client.

This preserves the native parser/model/engine path. It does not add a substitute
MIDI processor or external controller. The implementation follows
[ALSA's raw MIDI interface](https://www.alsa-project.org/alsa-doc/alsa-lib/rawmidi.html),
[ALSA's API reference](https://www.alsa-project.org/alsa-doc/alsa-lib/group___raw_midi.html)
and [sndio's MIDI manual](https://man.openbsd.org/mio_open.3). A read error was not
classified as a recoverable overrun without supporting native evidence. Independent
ALSA hardware overrun-counter sampling remains outstanding.

RawMidiIoTest is registered only when ALSA or sndio is compiled. Its private native
API substitutions exercise the actual drivers' constructors, worker threads and
engine handoff: 11 source cases, 33 ALSA scenarios and 30 sndio scenarios. Coverage
includes nonblocking flags and multiple descriptors, partial construction cleanup,
fragmented/running-status input on the model owner, transient and terminal errors,
partial packets and bounded write interruption, packet serialization, held-reader
shutdown, native engine replacement with stale queued-input revocation, and cancelled
candidate cleanup. These are authored cases, not execution evidence.

Source validation passed across 391 C++ files and 147 native operations, including
whitespace/Python syntax/catalog checks. The generated validation record now tracks
raw MIDI fixtures separately. All native/process/GUI tests remain uncompiled/unrun.
Raw output still has no timestamp scheduler or arbitrary SysEx/system output. A
rejected write can leave an incomplete packet; no deferred retry queue or musical
replay was added. Kernel/hardware buffers lack per-model cancellation or physical
receipt proof. Native calls, close and joins may block; no forced deadline is
claimed. Real backend/device behavior, GUI/plugin/project compatibility, latency,
CPU/RSS, real-provider reasoning and the complete Interpret-to-Preserve production
cycle remain unverified. This turn advances the actual native application; the full
commission stays active and completion is unproven.


### Native raw MIDI packet queue and retirement cleanup (2026-10-14)

The preceding lifecycle repair made the actual native drivers observable and
joinable, but its direct byte writes could leave a partial MIDI message on ordinary
backpressure. This turn continues the full application commission, with all five
attachments read and no applicable AGENTS.md found. Build availability remains
unchanged: cmake, ninja, cl, clang++, g++, qmake, qmake6 and lmms are absent from
PATH, and 17/17 submodules remain uninitialized. No denied execution route was
retried. The source changes below supersede the preceding section's immediate
byte-output behavior and fixture counts.

ALSA raw MIDI and sndio now share a native packet-ownership helper, RawMidiOutput.
The existing MidiPort models and channel-voice encoder remain authoritative.
Model output enqueues complete packets into a fixed 1024-packet client queue;
only the native worker submits them after writable readiness. Each cycle makes
at most 64 nonblocking writes. Short writes keep the exact remaining suffix,
and EAGAIN/EINTR/zero progress retains it for a later cycle with a 1 ms backoff.
Wake-pipe draining is bounded as well. ALSA polls up to 64 descriptors for each
direction plus its wake descriptor; sndio polls its bounded duplex inventory.
Native handle access remains serialized and shutdown still joins before closing.

Cancellation removes unsubmitted source packets. A partially written packet must
finish its framing before the worker submits reserved zero-valued controllers
120, 64, 66 and 123 for affected channels. Cleanup precedes new music and has
storage independent of the music queue. Repeated local releases coalesce, and
cleanup survives a port's removal. Worker source identities are opaque pointers;
completion of an old partial packet does not assign its channel to a new port
that reuses the same address. Queue overflow discards pending music, retains
channel cleanup, raises an enqueue error and keeps the last failure observable.
Fatal native errors stop the duplex client and retain uncertain cleanup without
automatic replay or false recovery.

Port transfer waits cooperatively up to 250 ms for native-library submission.
Timeout, cancellation or unavailable output refuses transfer while preserving old
client bindings; a fresh request can retry after cleanup progresses. Destruction
attempts the same cleanup, reports failure, then joins and closes. The worker
never dereferences a retired model. These are source-level guarantees pending
native execution, not verified real-time or physical MIDI behavior.

The existing output status fields are now shared between midi.inspect and
transport.inspect.midiBackend.outputBackend. Pending events, queue limit, cleanup
channels, activity and current/last errors remain inspectable after the last port
retires. The conversational runtime directs the agent to inspect that state and
recognize channel-wide cleanup and unacknowledged device receipt. No new operation
or alternate MIDI engine was introduced; the catalog remains 147 operations.

RawMidiOutputTest adds 12 portable cases / 26 scenarios for message encoding,
exact partial suffixes, no-progress retries, bounded service, cancellation,
source retirement/address reuse, reserved cleanup, overflow, fatal failure and
concurrent producers. RawMidiIoTest now has 16 cases with 39 ALSA and 36 sndio
scenarios, including worker-only writes, queued/partial cancellation, cleanup
observation after port removal, handoff refusal/retry and shutdown submission.
The private transports retain the actual driver constructors, worker loops and
engine integration. These fixtures are authored, uncompiled and unrun.

Source validation passes for 394 changed C++ files and 147 native operations;
the record separately inventories portable queue and conditional driver tests.
This is delimiter, whitespace, Python syntax and source/catalog validation only.
Raw output still ignores timestamps and rejects arbitrary SysEx/system messages.
Finishing a cancelled partial note-on can briefly activate it before cleanup;
channel-wide cleanup can release other sources sharing that channel. Library
acceptance does not establish kernel drain or hardware receipt. Already submitted
bytes cannot be revoked, and discarded music is not replayed. A fatal backend
with uncertain cleanup can refuse handoff; shutdown may report incomplete cleanup
before closing. Queue locks span individual nonblocking writes, and native calls,
close and joins have no forced deadline. Native execution, hardware, GUI, plugin
and project compatibility, real-provider reasoning, complete production cycles,
latency and CPU/RSS acceptance remain unverified. The full commission remains
active and its completion remains unproven.


### Explicit retirement of failed native MIDI output (2026-10-14)

The previous turn made source progress on raw packet ownership and cleanup. This
turn re-read the five commission attachments, checked for applicable AGENTS.md
files (none found), and refreshed the build environment. The eight checked
C++/Qt/LMMS commands remain absent from PATH and all 17 submodules remain
uninitialized. There is no live native build/test process. The complete DAW scope
and native model/DSP authority remain unchanged.

Current-source review found a recovery dead end: a terminal raw MIDI error can
retain uncertain notes or a partial packet, while normal backend selection must
finish cleanup before publishing a replacement. Since that worker cannot resume,
every normal replacement can fail. The application needs to preserve and report
that uncertainty while allowing an explicit choice to retire the failed client.

`midi.backend.select` now accepts optional boolean `abandonFailedOutput`, default
false. Output status exposes `abandonmentSupported` and `abandonmentEligible`.
Only ALSA raw MIDI/sndio with a terminal queue, requested stop and no running worker
report eligibility. Live output, temporary backpressure, overflow recovery,
unobserved output and unsupported backends refuse. Engine admission checks this
before stopping audio or constructing a candidate; the transfer primitive
revalidates it through preparation. No native worker is forcibly interrupted and
no failed output is automatically treated as cleaned.

Explicit abandonment uses the existing native engine transfer. It preserves port
objects, parameter models, requested endpoints and local input cleanup. Replacement
registration/validation must complete before commit. Failed factory preparation,
registration, validation, cancellation or eligibility changes preserve the old
output ledgers. At commit, those records are counted and cleared as the same native
ports are rebound; old notes are never submitted to the replacement. The failed
client is then retired and the original audio state is restored, or the existing
audio.device.restore reservation is retained with a committed MIDI result.

Selection results now include `previousBackend` and `outputAbandonment`. The latter
contains an explicit abandoned flag, decimal-string local note/pedal counts, the
old queue/cleanup counts and current/last native error, plus false physical-cleanup
verification. A retired port can leave no local note records but retain client
cleanup, which remains represented. The previous backend name is not a physical
device identity. The conversational runtime directs the agent to inspect eligibility,
use the option explicitly and report uncertain hardware cleanup. Historical receipts
persist these observations; replay/restart does not repeat the selection.

Five new portable transfer/engine cases plus two additional scripted provider rows
add 19 scenarios. They cover admission refusal, candidate/cancellation failures,
local input cleanup, revoked eligibility, final validation, raw/named replacement,
model retention, audio restart recovery, strict boolean/default behavior and receipts
across restart. MidiClientTransferTest now has 34 cases / 134 scenarios. One new
conditional raw-driver case adds eight scenarios for terminal input/output failure,
partial packets and retired ports; existing backpressure rows also verify refusal.
RawMidiIoTest now has 17 cases / 43 ALSA plus 40 sndio scenarios. The JSONL harness
adds unsupported-abandonment refusal and historical replay. All native and process
fixtures remain uncompiled/unrun; deterministic provider fixtures do not establish
real inference or autonomous musical success.

Source validation passes for 394 changed C++ files and 147 operations, including
source inventories, delimiter/whitespace checks and Python syntax. The validation
record no longer incorrectly labels runtime backend selection unimplemented; it
distinguishes its source implementation from missing execution evidence.
The new path does not purge kernel buffers, reset physical devices, establish
delivery/cleanup, replay music or add timestamp scheduling. Old native handles
remain open during candidate preparation, so exclusive reopen may require an
explicit intermediate Dummy selection. Other backends retain their existing cleanup
requirements. Native call/close/join deadlines, hardware, GUI/plugin/project
compatibility, resource use, real providers and the full Interpret-to-Preserve
workflow remain unverified. This advances native recovery; the full commission
remains active and completion remains unproven.


### Native ALSA raw input-loss observation (2026-10-14)

The previous goal turn made source progress on explicit terminal MIDI retirement.
This turn re-read all five commission attachments and the current implementation,
found no applicable AGENTS.md and refreshed build availability. CMake, Ninja,
the checked C++ compilers, qmake/qmake6 and LMMS remain absent from PATH; 17/17
submodules are uninitialized. No native build or execution evidence is available.

Source review found that the raw ALSA reader checked read/poll errors but did not
sample native input-overrun status. A successful read therefore did not establish
that its byte stream retained its framing. The
[ALSA status reference](https://www.alsa-project.org/alsa-doc/alsa-lib/group___raw_midi.html)
provides the status/counter APIs, and the
[Linux raw MIDI implementation](https://github.com/torvalds/linux/blob/master/sound/core/rawmidi.c)
shows a consumed input-byte overrun count. This is separate from the application's
bounded MIDI event queue and its dropped-event count. External implementation
evidence informs the change; it does not prove LMMS runtime behavior.

MidiAlsaRaw now queries native input status before its worker starts, on idle poll
timeouts and on both sides of nonempty input reads. Queries share the existing
native handle mutex. Each status operation retries interruption at most four times;
invalid/error returns or exceptions invalidate observation and fault the client.
Initial failure retires native resources before reporting a running backend. A
positive reported count maps the detected discontinuity to -EOVERFLOW, stops the
duplex worker and latches existing BackendInput delivery failure. The uncertain
chunk is not decoded and its running status is not continued. Pending native-model
input is revoked, with local all-sound-off performed on the owner through the
existing dispatcher. StudioMidiRecording already checks backend health, delivery
failure versions and event drops before publishing a take.

InputBackendStatus now carries overrunObservationAvailable and a 64-bit
reportedOverrunBytes value. midi.inspect/project input-delivery observations and
transport.inspect expose those fields, serialize the count as a decimal string,
and describe its scope separately from dropped application events. Inspection
reads cached state instead of sampling/resetting the native counter. Other
backends default to unavailable observation; zero never asserts loss-free capture.
The conversational runtime is told to preserve that distinction. The existing
explicit terminal-output abandonment and backend transfer provide recovery while
retaining uncertain external note cleanup; no replacement engine was introduced.

Four conditional RawMidiIoTest cases add 22 scenarios. They cover startup and
idle loss, loss detected before or during a successful native read, maximum native
counter size and JSON precision, interrupted/failed/invalid/throwing status calls,
owner-thread note cleanup, refusal to reset a failed client, and native replacement
with output uncertainty followed by explicit input reset. Status/read substitutions
still drive actual native constructors and worker loops. The JSONL harness also
checks that Dummy input reports the counter as unavailable. RawMidiIoTest now has
21 source cases / 65 ALSA and 40 sndio scenarios; all native/process tests remain
uncompiled and unrun.

Source validation passes for 394 changed C++ files and 147 operations, with the
new input-loss evidence and limits recorded separately. These are source inventory,
delimiter, whitespace and Python-syntax checks, not a compiled DAW result. Native
reporting and counter width limit the observation; it is not an exact event count
or complete measurement of physical loss. Input already delivered before a later
fault cannot be revoked. This change stops the duplex backend after observed loss
instead of resuming an uncertain byte stream; replacement may require explicit
output abandonment and cannot guarantee external all-notes-off. Native query
overhead/deadlines, real MIDI recording, device/GUI/plugin/project compatibility,
resource behavior, live inference and the complete autonomous production cycle
remain unverified. The full commission remains active and completion unproven.

### Native WinMM output health and output-only replacement (2026-10-14)

The previous goal turn advanced native ALSA input-loss observation. This turn
re-read all five commission attachments, found no applicable AGENTS.md and
refreshed the build prerequisites. CMake, Ninja, the checked C++ compilers,
qmake/qmake6 and LMMS remain absent from PATH; 17/17 submodules are uninitialized.
The full native application remains the commission's deliverable. No build,
fixture, hardware or full autonomous-production execution evidence is available.

Source review found that WinMM recorded output-send failures but returned success
to MidiPort. A failed note-off or pedal release could therefore erase the local
ledger even though the native destination remained uncertain. Output failures
also shared the input's first-error field, potentially invalidating usable MIDI
input. Separately, runtime transfer used the default input-activity admission
despite WinMM startup accepting a usable output-only client. The
[Microsoft short-message API reference](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutshortmsg)
documents the return-code boundary and possible blocking behavior; the
[output-open reference](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutopen)
documents native output-handle ownership. These references establish API
contracts, without proving behavior of LMMS or a particular driver.

MidiWinMM now attempts each subscribed native output and reports any failed
MMRESULT or normalized exception after completing the other attempts. MidiPort's
existing uncertainty ledger consequently retains failed note/pedal releases and
can retry them. Each owned output keeps its latest send result. Successful retry
clears that endpoint's current error, while success on a different endpoint
cannot conceal an outstanding failure. A separate last-output-error field keeps
capability/open/send/close failures visible after recovery. Input health retains
its own first error and started-input observation. Successful opens with null
handles are rejected before publication in either direction.

The existing midi.inspect and transport.inspect outputBackend fields now expose
WinMM's aggregate owned-output state: active requires at least one handle and
no current send failure. Failed opens remain historical errors when another
owned output is usable. No owned output means inactive with the most recent
output error, if any. There is no software output queue in this backend, so
queue/pending counters remain zero and forced abandonment remains unsupported.
Native factory/startup refusal metadata now also considers output errors when
the input observation supplies none.

Runtime port transfer uses WinMM availability across both directions. A client
with only output can therefore replace the current backend while preserving
native MidiPort models and requested endpoint names. Unusable candidates still
fail before ownership changes. Failed cleanup keeps the old client and activation
ledger for retry; an explicit abandonment request is still refused for WinMM.
No external agent wrapper or alternate music engine was introduced.

A private WinMmMidiApi function table preserves platform function types and
calling conventions while allowing fixtures to exercise actual constructors,
callbacks, sends, close paths and AudioEngine selection without physical devices.
The new conditional WinMmMidiIoTest target links winmm and has a 120-second CTest
timeout. Its 13 source cases / 47 scenarios cover seven channel-message types,
malformed packets, routing, partial multi-destination sends, note/pedal cleanup
and retry, endpoint-specific error recovery, input/output health isolation,
loopback queuing, capability/open/start/null-handle failure ownership, output-only
selection and inspection, unusable candidates, replacement refusal/retry and
callbacks during/after native retirement. All scenarios remain uncompiled and
unrun; the earlier MidiPortTest callback fixtures remain separate.

Source validation passes for 396 changed C++ files and all 147 operations. The
record includes the new fixture inventory, conditional build registration and
explicit verification limits. Its delimiter, inventory, whitespace and Python
syntax checks are not a substitute for a native build or execution. Native
submission is not physical receipt; partial sends may already affect other
destinations and retry can repeat cleanup there. Callback timestamps, SysEx,
automatic hotplug/reconnection, native close-failure retention/retry and hard
send/reset/close deadlines remain unfinished. Native audio/MIDI execution,
GUI/plugin/project compatibility, resources, real inference and the complete
Interpret-to-Preserve workflow remain unverified. This turn made source progress;
the full commission remains active and completion remains unproven.

### Retained WinMM cleanup after native port retirement (2026-10-14)

The previous goal turn made source progress on WinMM send failures and output-only
backend replacement. This turn re-read all five commission attachments and the
current native lifecycle, found no applicable AGENTS.md and refreshed build
availability. CMake, Ninja, the checked C++ compilers, qmake/qmake6 and LMMS remain
absent from PATH; 17/17 submodules are uninitialized. Native compilation, fixture
execution and full-application verification remain unavailable. The entire
native DAW remains the intended deliverable.

The remaining failure was concrete: deleting an instrument or other MIDI port
after a failed native note/pedal release destroyed the only remaining activation
ledger. WinMM retained a send error but no cleanup obligation. A later successful
send could clear that error, and backend replacement with no registered ports
could close the client without retrying the uncertain releases. This matters for
native project editing and persistent agent recovery, rather than only physical
device initialization.

MidiPort now copies its remaining note/pedal channel mask through a noexcept
backend hook before clearing a failed destructor ledger. The mask includes
releases never attempted after an earlier failure. WinMM copies it into each
subscribed owned output, using a bounded 16-bit mask without retaining the retired
port pointer. Other backends keep their existing cancellation behavior through
the default hook. This native interface change requires rebuilding native
components; compatibility with prebuilt components remains unverified.

Before new output on an affected endpoint, WinMM attempts all-sound-off,
sustain-off, sostenuto-off and all-notes-off on each pending channel. Each pass
makes at most 64 cleanup submissions per output. A channel clears only after all
four submissions succeed. Later controller success cannot conceal an earlier
failure, and fully completed channels remain cleared across cooperative
cancellation. A failed endpoint withholds its new event while other subscribed
endpoints are still attempted. Native calls remain outside the device-table
mutex, allowing loopback input to enqueue for the model owner. A reused source
address cannot replace retained state, and retired music is never replayed.

WinMM's transfer-output barrier now flushes every retained endpoint even if the
last source port has retired. A failed flush prevents replacement and preserves
the current client for retry; forced abandonment remains unsupported. Final
shutdown attempts retained cleanup after callback tokens are revoked and before
native handle close, continues other endpoints after a send failure, then still
attempts close. The
[Microsoft short-message contract](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutshortmsg)
supports native submission/error observation but permits a blocking call. The
[native close contract](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutclose)
does not establish successful close after an error. This work does not resolve
failed-close ownership or make a physical-delivery assertion.

The existing outputBackend inspection now gates active on both current error and
retained cleanup. cleanupPendingChannels reports the union of pending channel
numbers across outputs, at most 16; pendingEvents and queueLimit remain zero for
this immediate backend. Studio inspection describes retained cleanup separately
from queued music, and the conversational runtime is instructed to preserve that
distinction and report the shared-channel effect. No operation or alternate
execution engine was added.

Eight added WinMmMidiIoTest cases contain 26 scenarios for note/pedal/failed-note-on
retirement, all remaining channels after an early release failure, each cleanup
controller failing or throwing, partial cancellation, actual studio capability
refusal/retry with no remaining port, shutdown across outputs, source-address
reuse and owner-thread loopback delivery. The target now has 21 source cases /
73 scenarios, all uncompiled and unrun. Source validation passes for 396 changed
C++ files and 147 operations; these inventory, delimiter, whitespace and Python
syntax checks are not a native runtime result.

Cleanup is conservative and can silence other tracks sharing the same native
destination/channel. Repeated passes can repeat already successful controller
submissions. State is process-local and final shutdown is best effort; hardware
may retain notes after failure or process exit. Client-first teardown with still
live ports, native close-failure retention/retry, automatic reconnection, SysEx,
timestamp scheduling and hard driver deadlines remain unfinished or unverified.
Native runtime/hardware, GUI/plugin/project compatibility, CPU/memory behavior,
real inference and the complete Interpret-to-Preserve production cycle remain
unverified. This turn made source progress. The full commission remains active
and completion remains unproven.


### WinMM output retirement when ports outlive the client (2026-10-14)

The previous goal turn retained failed output cleanup after port deletion. This
turn re-read all five commission attachments, inspected the native shutdown path,
found no applicable AGENTS.md and refreshed the build prerequisites. CMake,
Ninja, the checked C++ compilers, qmake/qmake6 and LMMS remain absent from PATH;
17/17 submodules are uninitialized. The requested full native application remains
the scope, and no native build or execution evidence is available.

Source inspection showed the inverse lifetime gap: WinMM closed its handles
before MidiClient's base destructor invalidated surviving MidiPorts and cleared
their output ledgers. Only failed cleanup from already-deleted ports reached
the previous channel-retention path. A live port's held note, overlapping notes,
pedals or partially successful activation could therefore be forgotten when its
client died first. Native close alone did not provide a source-level note-release
operation. Microsoft's
[close reference](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutclose)
describes handle closure, while the
[reset reference](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutreset)
describes silencing notes across every channel. Based on those separate contracts,
this change retains the existing targeted controller-cleanup path for channels
LMMS may have activated, without interpreting close as physical cleanup evidence.

Each owned WinMM output now keeps a second bounded 16-bit channel mask. Positive
note-on and sustain/sostenuto activation reserve a bit before the native call,
including failure/exception uncertainty. Non-activating events do not expand it.
Ordinary note-off or pedal release cannot prove that another source on the same
channel ended, so the mask stays conservative until a complete four-controller
channel cleanup succeeds. Successful cleanup clears both used and pending masks;
new activation or later failed port retirement can create new obligations. The
state remains on the actual native output and contains no MidiPort pointer.

The transfer-output barrier now stages every used channel before ownership can
commit. Failure preserves the old backend and pending channel state for retry,
including when individual port releases already succeeded. Native close stages
the same channels while holding output lifetime, so surviving ports cannot erase
the only record of their possible activations. Each pass remains at most 64
cleanup submissions per output. All pending endpoints are attempted after a
native send failure, and final shutdown still attempts handle close. Successfully
completed transfer cleanup is not repeated at close unless new state was retained.

The client publishes closing state before native cleanup/reset/close: availability
and input/output activity become false. Callback tokens are revoked and old
queued input is invalidated before native cleanup can produce loopback traffic.
Ordinary output on a closed client throws rather than returning success. The
existing internal device-refresh path reopens native registrations, preserves
requested port names and requires explicit input delivery reset before receiving
again. This is lifecycle support, not automatic hotplug enumeration. Native model
settings survive the client-first path; the base invalidation still detaches
surviving ports to Dummy and clears their retired local output ledger.

Eight new WinMmMidiIoTest cases add 31 scenarios for note/pedal/overlap activation,
failed and throwing activation, non-activating messages, actual channel and
destination filtering, observable closing state, loopback/queued-input revocation,
continued native close after cleanup failure, completed-cleanup reuse, studio
capability refusal/retry before transfer commit and native close/reopen. The
fixture now has 29 source cases / 104 scenarios. Earlier assertions were updated
to include conservative channel cleanup at the transfer boundary. All native
fixtures remain uncompiled and unrun.

Source validation passes for 396 changed C++ files and all 147 operations, with
the updated fixture inventory and verification limits in the generated record.
These source inventory, delimiter, whitespace and Python-syntax checks do not
prove a working DAW. Conservative channel cleanup can silence other sources
sharing a destination/channel, including after ordinary note-off success. Native
calls may block, submission does not acknowledge physical silence, final shutdown
is best effort, and the masks do not survive process exit. Cleanup of local voices
already triggered by input is outside this output change. Native close-failure
retention/retry, automatic reconnect, SysEx, timestamps and hard driver deadlines
remain unfinished or unverified. Native execution/hardware, GUI/plugin/project
compatibility, resource behavior, live inference and the full autonomous
Interpret-to-Preserve cycle remain unverified. This turn made source progress;
the full commission remains active and completion remains unproven.

### Local MIDI input cleanup when ports outlive their client (2026-10-14)

Unexpected client destruction previously discarded queued input and rebound
surviving MidiPorts to Dummy, but did not release local voices already activated
by delivered input. The detached ports also lost registered input-recovery state.
Base MidiClient retirement now first revokes every old delivery and detaches all
still-owned models before entering any processor callback. It adopts the shared
delivery records into a process-lifetime Dummy registry, preserves drop/recovery
counters, and exposes `ClientRetired` as input failure code 7. Candidate clients
that registered a model still owned by another backend do not detach that model.

Queued-delivery note-on and sustain/sostenuto activation conservatively mark a
possible local cleanup obligation. Teardown clears undelivered packets and then
runs native all-sound-off on the port's model owner thread. It respects an
existing delivery or recovery scope, so a callback that destroys its own client
does not cause nested processor entry. All client/entry/output locks are released
before cleanup enters native models. Ports may be disabled or change input
channel after activation; cleanup bypasses those filters. Weak model references
allow cleanup callbacks to delete themselves or another already-detached port.

`retirementCleanupPending` remains observable on cleanup failure or export
deferral. Successful cleanup clears that obligation but keeps the retirement
failure until an explicit local reset acknowledges it. The fallback no longer
requires the destroyed backend's health observation for local recovery. This
does not reopen a native backend, connect hardware, restore discarded events or
attach the detached port to the engine's selected client. The studio inspector
and agent guidance state these distinctions. Requested endpoints and native
model settings are preserved; the old output ledger still belongs to the
derived backend's separate shutdown path.

Tracing real InstrumentTrack cleanup also exposed pending NotePlayHandles that
could enter the audio engine after all-sound-off. `removePlayHandlesOfTypes`
now removes matching pending and active handles under the native model guard,
unlinks each before invoking its destructor, removes stale deferred-removal
requests and restarts its search after reentrant completion. Track/type filters
and the option to preserve reserved recording completions apply to both lists.
This closes the queued-note lifetime gap at the native audio layer.

MidiClientRetirementTest adds 14 cases / 26 authored scenarios. Coverage includes
note/sustain/sostenuto activation after input disablement, undelivered input,
cleanup retry after backend loss, owner-thread dispatch from worker teardown,
destruction during note delivery and recovery, callback-driven model deletion,
discarded candidates, previously completed cleanup, export deferral, native
instrument handles before/after rendering, reentrant pending/active handle
removal and preserved reserved recording completion. Two scenarios additionally
require the actual TripleOscillator plugin and inspect native PCM. Those rows
explicitly skip when the plugin is unavailable. No scenario has been compiled
or executed in this environment.

Source inventory/delimiter/whitespace/Python checks pass for 397 changed C++ files
and all 147 operations. The generated evidence record includes the new fixture
inventory and limits. These checks provide source evidence only. Conservative
all-sound-off can affect other voices sharing an instrument, even after normal
note release; it does not prove physical MIDI silence. Derived clients must
already have revoked/joined their sources. Worker teardown requires an available
owner event loop; concurrent destruction of a model or ordinary access to the
retiring client from other threads is not established as supported. Allocation
failure warns and detaches but cannot guarantee registered recovery. The fallback
registry intentionally survives until process exit; each port unregisters its
own entry. Native compilation, hardware, GUI/plugin/project compatibility,
resource behavior, live inference and the complete autonomous production cycle
remain unverified. The full commission remains active and completion unproven.

### Retained WinMM native ownership after close failure (2026-10-14)

The prior goal turn made source progress on local MIDI input retirement. This
turn re-read all five commission attachments and the current source, found no
applicable AGENTS.md, and refreshed build availability. CMake, Ninja, the checked
C++ compilers, qmake/qmake6 and LMMS remain absent from PATH; all 17 submodules
remain uninitialized. Native compilation and execution are still unavailable.

WinMM previously swapped its handle maps into temporary containers during close,
recorded reset/close errors, then discarded the handles even when native closure
had failed. Microsoft's input/output close contracts explicitly allow failure
while buffers remain queued. The process now registers native ownership before
each open, retains a copy of the platform call table, and assigns a unique client
generation. A failed close therefore keeps its handle without allocating during
retirement, retaining a port pointer or relying on the old client's lifetime.
A new client at the same address cannot inherit the old owner's cleanup pass.
Partially constructed devices use the same ownership path.

Shutdown still publishes closing state, revokes every callback token, invalidates
queued input and attempts retained channel cleanup before native close. It marks
all owned resources retired before servicing them. Registry locks are released
around native calls; a separate nonblocking service admission prevents nested
cleanup while allowing inspection and late callbacks. A pass visits only records
present at its start, at most once each. Errors and caught exceptions do not skip
close attempts for other handles. Successful close or a native invalid-handle
close removes ownership; all other outcomes remain retained. Input resets precede
close for successfully started inputs. Output close reporting STILLPLAYING
requests a device reset on its next retry. Pending channel cleanup is retried
before close, preserving completed channels across unsuccessful attempts.

Subsequent WinMM construction, reached by explicit backend selection or the
existing private refresh path, retries pending handles before opening devices.
Unresolved ownership prevents new opens. A failed candidate preserves the current
engine backend. This is separate from the already committed switch that retired
the old client: selection of Dummy can succeed with old WinMM handles still
pending. Read-only transport inspection now exposes `midiBackend.nativeRetirement`
with observation availability, retry activity, pending input/output counts,
pending cleanup channels, current/last native errors and the exact backend name
that triggers retry. It remains observable after selecting another backend.
The agent guidance explains that discovery and historical receipt replay do not
retry cleanup and that zero retirement exceptions do not prove handle closure.

Eight new WinMmMidiIoTest cases add 24 Windows-conditional scenarios for failed
input/output/both closes, error/exception handling, reset failure, retained channel
progress, copied API lifetime, reused client addresses, invalid native handles,
partial startup, reentrant inspection/reopen refusal and committed backend-switch
reporting with refused/successful subsequent selection. The fixture now has 37
cases / 128 authored scenarios. Review also corrected its missing required output
processor override, added an explicit AudioDevice include and replaced two
assertions that invoked cleanup instead of observing delivery failure. All native
fixture and real driver execution remain unrun.

Source checks pass for 397 changed C++ files and all 147 operations; inventories
and verification limits are retained in the generated evidence record. These are
source checks, not a native build or end-to-end acceptance result. The protocol
records the supporting Microsoft close/reset references. Reset can affect every
channel on a device, and channel cleanup can silence other sources. Successful
submission or closure does not establish physical silence. A driver can block
inside a call despite bounded attempt counts; there is no periodic retry worker
or hard native-call deadline. Retained records and history are process-local and
persist until successful retry or process exit. Eventual resource closure, native
hardware/GUI/plugin/project compatibility, CPU/RSS behavior, live inference and
the complete autonomous production cycle remain unverified. The full commission
remains active and completion unproven.

### Complete native MIDI event export and arrangement reads (2026-10-14)

The preceding goal turn retained failed WinMM native handle ownership. This turn
made source progress on the existing Song/project.exportMidi export path. All
five commission attachments and applicable instruction locations were reviewed;
no AGENTS.md was found. The native build constraint persists: CMake, Ninja, the
checked C++ compilers, qmake/qmake6 and LMMS are absent from PATH, and all 17 pinned
submodules remain uninitialized. No native compilation, GUI or process test ran.

Source review found that MidiFile stopped writing at 50 KiB per track while the
exporter reported success. Later notes and note-offs could be silently discarded.
The writer now retains every event, validates format and endpoint bounds, and
uses native 48-PPQ integer ticks. Absolute event times survive repeated encoding;
stable ordering places bank before program and note-off before a new onset at the
same tick, and each event retains its original channel. Long gaps are split with
empty text events so individual delta quantities remain within four bytes.
Existing QSaveFile publication now has cancellation checks during traversal,
repeat expansion, encoding, 64 KiB output chunks and immediately before commit.
Errors preserve the prior destination. Empty projects produce a valid conductor
track instead of a zero-track file.

The exporter previously serialized entire instrument/plugin state merely to read
notes and settings. It now traverses the native models under owner-thread and
engine guards with event pumping suspended. Unloaded instruments no longer cause
a null dereference. Track volume comes from its actual FloatModel; the old XML
reader used the wrong attribute name. Pitch and velocity clamp before narrowing,
muted/disabled/zero-velocity material is omitted, and ordinary clips honor signed
start offsets and trim boundaries. Pattern indices no longer wrap at 255. Repeat
periods use all native instrument rows and the current meter, sparse cells remain
unmodified, and arranged placements preserve phase through overlaps and ties.
Pattern-cell offsets follow InstrumentTrack's local-onset and boundary-retrigger
rules. Each export starts with fresh channel and pattern state.

MidiExportTest compiles the production exporter directly and decodes the actual
result using an independent strict SMF reader. Its 19 cases / 65 authored scenarios
include 20,000 notes with all releases and terminal EOT, repeated encoding, event
ordering/channels, long gaps, invalid values, empty/unloaded models, note-value
bounds, muted/trimmed material, pattern 256, multirow odd-meter periods, signed
phase, overlaps, cell/step boundaries, Unicode and reused exporters, cancelled
publication, event/thread admission, sparse cells and failed destinations. These
fixtures are registered with native-midi/headless/export labels and a 180-second
timeout when midiexport is built. None has been compiled or executed.

Source diagnostics pass for 399 changed/new C++ files and 147 native operations;
Python syntax and fixture/build inventory checks also pass. The agent tool
description and production guidance now state MIDI export's limited semantics.
The protocol records behavior, supporting format reference and verification
limits, and corrects earlier WinMM text that still called native close retention
unfinished after its source implementation.

This remains static editable-note export, not a DSP performance capture. Step
lengths are approximations; effects/samples, tempo/parameter automation, generated
arpeggios/chords, per-note expression, microtuning/key-range synthesis, native MIDI
port processing and meter/key metadata are not exported. Nominal half-open tick
geometry does not reproduce frame-block/inclusive-end playback artifacts. MIDI
channel reuse and same-key overlap can alter receiver behavior. Event and encoded
buffers remain resident; sorts, allocations and native calls have no hard
deadline. Actual import interoperability, audio equivalence, GUI/plugin/project
compatibility, CPU/RSS, provider reasoning and the complete autonomous production
cycle remain unverified. The full commission remains active and completion is
unproven.

### Native MIDI import ownership, framing and recovery (2026-10-14)

The previous turn made source progress on native MIDI export. This turn reviewed
all five commission attachments and current instruction locations, found no
AGENTS.md, and refreshed the worktree and build evidence. The worktree initially
had 401 status entries. CMake, Ninja, the checked C++ compilers, qmake/qmake6 and
LMMS remain absent from PATH; 17/17 submodules, including pinned PortSMF, are still
uninitialized. No native build, parser execution, GUI or audio test occurred.

Review found two native import defects affecting editable project fidelity.
Controller curves were indexed by controller number within an SMF track, so a
second MIDI channel could write its automation into the first channel's model.
Program curves were shared across source tracks by channel number. The RIFF path
also located embedded data but then read again from the outer file header. Import
could therefore report success after decoding the wrong bytes. Raw parser state
was manually owned and its read status was not checked.

The existing importer still uses PortSMF to interpret musical events. A checked
native framing reader now copies file input in 64 KiB chunks, extracts exact
RIFF/RMID data including odd alignment, validates SMF/track/event boundaries and
rejects recognized malformed input before native model creation. Unknown chunks
and unrelated formats keep their normal forwarding behavior. PortSMF sequence
ownership is now RAII-based and its read result is checked. The parser API and
behavior remain unverified against the unavailable pinned build; attempted
upstream source retrieval did not establish executable evidence.

Native construction runs on the model owner thread under the engine guard and
suspends event polling. Channel/controller creation no longer calls processEvents;
a nonmodal progress dialog avoids implicit event dispatch from setValue. Reading,
framing, event conversion, copying and final acceptance check inherited execution
budgets/cancellation. Failure removes newly appended destination/Song tracks,
including new PatternStore rows before their automatically created Song column,
while retaining existing tracks/notes. The same importer can be retried after its
file closes. This is content cleanup, not a reversal of every application or
external side effect.

Source-track/logical-channel keys now separate instruments and automation. Bank
MSB/LSB update a single combined native bank curve. SF2 selection verifies the
loaded plugin and its native models before dereferencing bank/program controls.
Imported endpoints round at native tick resolution and retain the one-tick
minimum for short notes; velocity conversion rounds instead of truncating. Time
and finite-value checks precede unsafe conversions, tempo/meter nodes bypass GUI
quantization, pitch bend scales by the native semitone range, and clip splitting
keeps the maximum outstanding note end through overlapping polyphony. Unsupported
event reporting is aggregated instead of printing each event.

Source review also found an invalid typed FloatModel getter invocation introduced
in the prior exporter change. It now calls the typed value() overload. Matching
calls in StudioPatternTest and StudioTrackMoveTest were corrected. This illustrates
the limit of delimiter/inventory validation: it does not establish C++ type
correctness or successful native compilation.

MidiImportTest compiles the actual importer, framing reader and pinned parser
sources. It authors 15 cases / 53 scenarios for valid/invalid containers, exact
RIFF payloads and padding, running status, SysEx/escape framing, extended/unknown
chunks, SMPTE header admission, independent controller/program/bank ownership,
pitch/timing/velocity conversion, unquantized tempo/meter, sustained polyphony,
range rejection, preserved existing notes on failure, cancellation after partial
model construction, retry, event reentry and foreign-thread refusal. Two cases
require native sf2player. CTest registration has native-midi/headless/import/
recovery labels and a 180-second timeout. All scenarios are uncompiled/unrun.

Source diagnostics pass for 403 changed/new C++ files and all 147 native
operations. The evidence record includes fixture inventories and explicit limits;
Python syntax and whitespace checks pass. Strict malformed-file rejection changes
prior permissive admission. File/parser/event/model storage is resident, with an
INT_MAX-byte input ceiling and current-meter timeline headroom. PortSMF parsing,
native resource/plugin calls, allocation and sorting have no hard deadline. GUI
cancellation cannot interrupt event-suspended construction. Cleanup can retain
modified/error/journal/allocation history and cannot undo MIDI auto-assignment or
external instrument effects; arbitrary synchronous reentrant destruction remains
unsupported. Format-2/SMPTE interpretation, generic CC enable/RPN/NRPN semantics,
controller completeness, successful PatternStore placement, actual audio,
interoperability, GUI/plugin/project compatibility, CPU/RSS, provider reasoning
and the full autonomous production cycle remain unverified or unfinished. The
full commission remains active and completion is unproven.


### Native MIDI import into shared pattern columns (2026-10-14)

The preceding work changed authoritative source files and authored native tests;
that goal turn made progress. The saved edits were inspected after the user's
pause/resume. All five commission attachments were reread and the full application
scope retained. Instruction scans found no AGENTS.md. Build commands remain absent
from PATH and all 17 pinned submodules remain uninitialized. No native compiler,
parser, LMMS process, plugin, GUI or audio execution occurred.

PatternStore import previously appended an extra instrument clip after native
track construction had already created one cell per column. Song-style note
splitting and controller clip creation then turned source time positions into
additional table cells. Tempo and meter automation also went into Song even when
the instrument rows were imported into PatternStore. This violated native table
ownership and made imported content evaluate at the wrong local time.

The production MIDI filter now freezes the selected destination column before
reading/advisory presentation and checks its identity under the engine model
guard. New instrument and automation rows reuse their native cells in that
column. Notes remain one complete local sequence per source track/channel;
controllers, programs, combined banks, tempo and meter use local automation ticks.
Time-zero controller/program values receive curves for pattern restart behavior.
Other columns remain empty/unconnected and old rows are not rewritten. Sparse
existing row tables are refused before native construction because MIDI-cell
initialization can otherwise create missing cells in an existing instrument row.

Fixed imported MIDI-cell lengths carry the complete decoder duration and note/
automation/event extent, rounded to the current native bar. This intends to retain
end-of-track silence; the pinned parser's duration API and behavior remain
uncompiled and unverified. Metadata-only imports create an empty, editable native
instrument row named MIDI sequence length without loading an instrument. This is
needed because PatternStore derives its period from instrument cells. New
imported automation geometry follows the final period, including longer existing
instrument rows. Time conversion reserves headroom for the column's canonical
position as well as native bar rounding.

project.import exposes container=song|pattern and an optional pattern index.
Song remains the default. Pattern imports use an existing explicit/current column
or column zero in an empty store, retaining the current selection when a column
already exists. The generic filter interface dispatches an explicit pattern only
to a filter implementing that destination; the Hydrogen song importer cannot
silently write Song during a pattern request. Its inherited overload remains
visible to avoid overloaded-virtual warnings. Native application and import
plugins require a rebuild for this interface extension. Pattern identity also
participates in queued selector validation. Existing durable import transactions,
history, receipt replay, recovery and post-import agent-batch invalidation remain
the execution path.

MidiImportTest now authors 23 cases / 72 scenarios using the production importer,
framing reader and pinned parser sources. Added cases cover empty/current/explicit
columns, index 256, 7/8 geometry, unchanged existing cells, separated source note
material, local controller/tempo/meter values, trailing silence, automation-only
and empty-duration sequences, a longer preexisting period, time-zero bank/program
curves, invalid/sparse destinations, cancellation after partial notes and retry,
native save/reload, selector identity and structured undo/replay/reopen. Three
SoundFont cases are conditional and the structured filter-dispatch case requires
the built MIDI plugin. Four added StudioAgentRestoreTest rows cover explicit
pattern import through both provider protocols and direct/catalog tool routes,
stale batch calls, fresh observation and continued native edits. This expands that
suite to six cases / 34 scenarios, eight requiring the built importer. Every
native scenario remains uncompiled and unrun.

Source validation and whitespace results are recorded separately in the current
JSON evidence after checking the final changes. These diagnostics cannot prove
C++ type correctness, Qt/MOC linkage, pinned parser interpretation, full sequence
playback, resource/plugin compatibility or audio behavior. Sparse-pattern refusal
and strict malformed admission are intentional compatibility changes with the
source reasons above. Native content cleanup still cannot undo external MIDI/
instrument side effects or all modified/error/journal/allocation history. Input,
parser, events and native models remain resident; native parsing/loading/sorting
and blocked calls have no hard deadline. GUI cancellation cannot interrupt the
event-suspended construction phase. Generic CC enable/RPN/NRPN behavior, format-2/
SMPTE interpretation, controller completeness, actual PatternStore playback and
round trips, legacy project/GUI/plugin compatibility, CPU/RSS, provider musical
judgment and the complete autonomous production cycle remain unverified or
unfinished. The full commission stays active; completion remains unproven.


### Native MIDI CC rack activation and playback continuity (2026-10-14)

The preceding goal turn made progress: PatternStore import source and native
fixtures were updated, and source/whitespace checks passed for 404 C++ files and
147 operations. This turn reread all five commission attachments and current
instruction locations; no AGENTS.md was found. Source review found that generic
MIDI CC imports wrote native knob values while the rack remained disabled. Native
model signals also suppress unchanged values, so simply enabling import could not
restore controller state after notes were cleared or an instrument was replaced.

InstrumentTrack now exposes its existing enable model, tracks used controllers
including explicit zero, and replays only that set. Native playback applies
Internal controller events at its tick/frame offset before notes, while stopped
edits remain immediate. Playing edits defer to a native tick. Rounded byte caching
avoids duplicate equivalent values. Native silence, rack disable/enable, output
channel changes and instrument replacement invalidate delivered state. Internal
channel-mode cleanup is excluded from whole-rack invalidation to avoid repeated
flush loops. The instrument receiver identity is checked after dispatch; changes
from callbacks wait for later playback. Delivery exceptions leave the value
retryable, and playback now releases its track lock through a scope guard.

A bounded activecc attribute persists exact controller use. Loading suspends CC
callbacks while restoring values and plugin state. Invalid indices, malformed
members and oversized lists disable the rack and collect a native project error.
Older files infer controller use from nonzero values and explicit model elements;
zero attributes with no activation information remain ambiguous. Older LMMS
versions do not understand the new attribute. Native app/plugins must be rebuilt
for the extended model interface. The GUI enable control still uses the same
native BoolModel through the public accessor, and headless parameter discovery
continues to expose that boolean and every rack knob.

The production MIDI importer initializes inactive SoundFont-compatible defaults,
marks only actual generic source controllers active, and enables only channels
that use generic CCs. Mapped volume, pan, bank and pitch controls keep their native
mapping. Initial expression and related controller defaults follow FluidSynth's
upstream fluid_chan.c initialization, inspected directly this turn at
https://github.com/FluidSynth/fluidsynth/blob/master/src/synth/fluid_chan.c. They
prevent a later expression change from first applying a zero initial rack value.
Defaults themselves must not activate unused controls; a dedicated initialization
helper suspends that callback. Exact RPN/NRPN ordering and complete MIDI semantics
remain outside the proof of this value-based native rack.

InstrumentMidiCCTest authors 11 cases / 24 scenarios using production native track,
model, event, playback and serialization methods with a deterministic instrument
receiver at the existing createInstrument boundary. Coverage includes used/unused
and zero controls, wire rounding/equivalence, silence/re-enable/replacement/channel
replay, tick/frame timing, channel-mode commands without flush loops, event-free
restore, legacy inference, seven malformed-list cases, failed delivery with
unlocked retry, callback edits, parameter discovery and history/receipt/session
recovery. Four added importer cases / 19 scenarios cover song/pattern generic CC
activation, native pre-event defaults, mapped-control isolation and project reload.
MidiImportTest now has 27 cases / 91 scenarios; its SoundFont and built-filter
requirements remain explicit. All native scenarios are uncompiled and unrun.

The implementation record and fixture inventory remain source evidence only.
No C++ compiler/Qt build, pinned PortSMF execution, real instrument CC behavior,
audio rendering, GUI/hardware interaction or CPU/RSS measurement occurred. A
held native model value cannot preserve repeated identical commands, equal-tick
controller ordering or full 14-bit/RPN/NRPN event semantics. Imported generic
controllers can change audible behavior compared with the previously disabled
rack; this requires compatibility and audio corpus testing. Plugin dispatch can
block and has no hard deadline. Internal CC input is not a hardware delivery
acknowledgment. Arbitrary synchronous track destruction or instrument replacement
while playback holds its locks remains unsupported. These additions advance native
composition, instrument automation and durable recovery; they do not satisfy the
full commissioning acceptance gates. The full goal remains active and completion
is unproven.


### Typed native model API integration review (2026-10-14)

The preceding goal turn made progress on native MIDI CC activation, replay and
persistence, with source validation for 406 C++ files and 147 operations. This
turn reread all five commission attachments and instruction locations, found no
AGENTS.md, and reviewed the current worktree rather than assuming its source
checks established native build readiness. CMake, Ninja, checked C++ compilers,
qmake/qmake6 and LMMS are still unavailable on PATH. No native build or execution
occurred.

Review enumerated 50 candidate lines using explicit float/int/bool template
arguments on value/initValue/minValue/maxValue in changed C++ files. Receiver
declarations were checked against native headers. TypedAutomatableModel<T>
defines non-template typed getters which hide the inherited AutomatableModel
function templates. Calls such as FloatModelPointer->maxValue<float>() therefore
conflict with the declared API. Base AutomatableModel pointers, automation
object references and GUI AutomatableModelView value<T>() calls remain valid and
were retained. TempoSyncKnobModel's setTempoSync overload is valid and retained.

Twenty-one getter calls were corrected in five files. The production
parameter.sync operation had four invalid custom-meter bounds getters on IntModel
and one invalid value getter on TempoSyncKnobModel, which inherits FloatModel.
StudioMidiControllerTest had twelve invalid FloatModel getter calls, including
controller range endpoints and actual-value reads. StudioAutomationRecordingTest
had two invalid maximum-value reads. StudioRuntimeTest had one FloatModel value
read, and VstParameterModelsTest had one FloatModel initial-value read. Each now
uses the native typed method, preserving the intended value and assertions. No
new functionality or test-count claim is attached to these API corrections.

The source validator records the file/call inventory and rejects the specific
known incorrect receiver expressions. Its evidence explicitly remains source
review, not general C++ lookup, compiler or ABI verification. Existing tempo-sync
recovery, controller delivery, automation recording, controller ownership and VST
restoration scenarios exercise these paths when a native build becomes available;
none have run here. The API review does not prove the remaining 406 changed C++
files type-check or link, and cannot establish Qt/MOC compatibility, native
playback/render correctness, plugin/legacy-project behavior, CPU/RSS or autonomous
production acceptance. The full application commission remains active and
completion is unproven.


### Bounded native stored-error previews (2026-10-14)

The previous goal turn made progress by correcting 21 native typed-getter API
calls and validating the source inventory. This turn reread all five commission
attachments and current instruction locations; no AGENTS.md was found. The
current native runtime and tests were inspected for Qt container and size API
issues. CMake declares Qt5 >= 5.15 by default and experimental Qt6 support. Qt's
current official porting guidance confirms QList/QVector size-type differences;
see https://doc.qt.io/qt-6/qtcore-changes-qt6.html. Review enumerated 38 changed-file
size-comparison candidates and checked the native JSON container paths. The
inspected standard/GUI container calls were retained. History-read offsets are
bounded by the verified payload size and existing 16 MiB archive limit, rather
than accepting an arbitrary unbounded offset. This is targeted review, not Qt
build or whole-application type correctness proof.

Source review found a resource issue in storedToolResponse: its 1024-byte error
preview first encoded the entire error QString to UTF-8. A large native failure
therefore required another temporary proportional to the complete error just to
retain a tiny provider preview. The native generator now caps the UTF-16 source
at 1025 code units before conversion. The extra unit retains a surrogate pair
starting at the last eligible code unit. Existing UTF-8 continuation-byte trimming
then retains the same complete-character prefix within 1024 bytes. The size is
narrowed only after the conversion's source has been bounded. Record validation
also rejects non-string and oversized UTF-16 previews before encoding; bounded
multibyte strings still undergo the existing byte-limit check.

StudioToolResultTest now authors 12 cases / 42 scenarios. One added data-driven
case has seven rows: short and exactly bounded ASCII, 3 MiB ASCII, a surrogate
crossing the source-character cap, a surrogate after the byte cap, a three-byte
character crossing the byte cap, and mixed multibyte encodings. Assertions cover
exact Unicode prefixes, the byte limit, native outcome/request/revision fields,
archive digest/size identity, preservation of the full original error and refusal
of oversized ASCII, multibyte or non-string stored previews. Existing durable
archive/publication/recovery/evidence and both provider-protocol cases remain in
the suite. All native cases are uncompiled and unrun.

CMake, Ninja, checked C++ compilers, qmake/qmake6 and LMMS remain absent from PATH.
No native Qt conversion, plugin/runtime/audio execution or memory benchmark
occurred. The input error, full response, archive serialization and verified
payload reads remain resident under existing limits. This bounds one conversion
and its validation; it does not establish a total memory limit, CPU/RSS figures,
Qt5/Qt6 build compatibility, provider context suitability, disk quota or full
recovery/music-production acceptance. The source evidence record is refreshed
with the preview and fixture scope. The full application commission remains
active; completion is unproven.


### Native tool archive preflight and verified reuse (2026-10-14)

The preceding goal turn made source progress on bounded error previews and native
fixture coverage. This turn reread all five commission attachments and refreshed
instruction locations; no AGENTS.md was found. Current publication, recovery and
retirement code was inspected. Native build commands remain absent from PATH and
no C++/Qt, LMMS, plugin, audio or runtime execution occurred.

Tool payloads were published before their matching provider output was checked.
A later inconsistent output could leave earlier new payload files even though
migration or commit failed. Retries also chose a new random filename for the same
serialized record. StudioStorage explicitly derives retirement ownership from
committed references, never directory contents, so a speculative orphan-directory
sweep would violate the existing preservation boundary.

The native runtime now indexes provider output locations once and checks all
matching outputs against every inline tool record before publishing any payload.
Both protocols and duplicate outputs use the same index. JSON formatting may
differ, but structured response content must agree. Small inline mismatches are
now rejected too. Missing current-exchange outputs keep the previous behavior,
since a retained tool record need not have a current message slot. The index
avoids a records-by-input repeated scan and stores only indices/field names;
it does not retain a second set of parsed large result objects.

New payload names use a UUID-shaped rendering of the first 128 bits of the exact
serialized record's SHA-256. References retain the complete SHA-256 and byte size.
An existing target is reusable only if it is a regular non-symlink file, the
bounded archive read verifies the full digest, and both bytes and parsed original
record equal the planned payload. A changed or colliding target is retained and
refused rather than overwritten. Exact reuse leaves its modification time and
file count unchanged. Older random UUID references stay readable. Qt's QUuid
fromRfc4122/toString behavior was checked against the official Qt6 documentation;
this remains uncompiled against either supported build configuration.

Checked reuse reads belong to mandatory metadata publication. That scope suspends
outer operation callbacks and Qt event polling so they cannot mutate the prepared
manifest during verification, matching the existing synchronous publication
boundary. Such reads/writes still have no hard I/O deadline. The full record,
encoded payload and checked read/JSON buffers remain resident up to the existing
16 MiB payload limit. A new payload or a later manifest failure can still leave
unreferenced files. This avoids mismatched-batch publications and duplicate
identical payloads; it does not reclaim old orphan files, delete acknowledged
history, enforce disk quotas or deduplicate other archive families.

StudioToolResultTest now authors 14 cases / 50 scenarios. Two existing legacy
migration rows additionally require unchanged archive identity, modification time
and file count on reuse. Six new batch-preflight rows cover both protocols with a
late value mismatch, invalid JSON or conflicting duplicate reply; failures must
retain the previous manifest and current local composition and publish no tool
payload. Two conflict rows preserve a changed target, refuse reuse and keep the
manifest/project. Existing archive corruption, exact retrieval, publication
rollback, provider pairing, compaction/restart and error-preview scenarios remain
in the suite. All are uncompiled and unrun. The source inventory calculation now
counts each data function directly, including shared protocol rows, rather than
relying on formatting-specific function declarations.

Source diagnostics and fixture inventories are recorded as source-only evidence.
These checks cannot prove real atomic file behavior, Qt/MOC/SDK build correctness,
filesystem race handling, permissions, actual retry/refusal outcomes, CPU/RSS or
native autonomous production. Full provider musical judgment, native audio,
legacy project/plugin/GUI compatibility and complete recovery acceptance still
require execution. The full DAW commission remains active; completion is unproven.

## Owned native archive paths and local build-tool audit

2026-10-14 source continuation; no native compilation or LMMS execution occurred.

Archive publication and verified reads previously followed parent-directory
redirects, while the retirement collector already checked canonical session
ownership. The runtime now applies a shared path guard before opening or creating
payloads for conversations, tool results, request receipts, jobs and contexts.
It checks exact family/UUID/JSON shape, fresh resolution of the session root to
the captured canonical location, a real non-redirected immediate family directory
and a regular non-redirected existing target at its owned canonical path. Parent
symlinks are rejected before mkpath; junction checks are compiled for Qt 5.15 or
later. An explicitly selected root symlink remains valid if its canonical location
is unchanged. Redirected archive parents that were previously followed now fail.
Inline records and ordinary existing UUID references remain supported.

Digest, byte-count, UTF-8/JSON and native archive-identity verification still follow
the ownership check. Startup validates references before replacing the native
project. History selection and receipt replay recheck ownership on reads. Existing
transaction boundaries remain authoritative: a job archive error during request
preparation can acknowledge a failure receipt without replacing its job, whereas
commit or context-bookkeeping publication failure stops the runtime and preserves
its prior acknowledged manifest, with native rollback when needed. No directory
scan, inference of ownership from names, deletion or orphan cleanup was added.

StudioArchivePathTest is registered with a 240-second timeout. Six cases author
44 POSIX scenarios; Windows builds have four cases and 16 scenarios. Matrices
cover all five archive families with target-directory replacement, refusal before
startup native replacement, fresh-read refusal and parent-file publication
obstruction. POSIX rows add file/parent symlinks with identical copied payloads,
publication through parent symlinks, accepted selected-root symlinks, and refusal
of receipt reads and new publication after root retargeting. A deterministic tool
migration case restores inline native records/provider output, then replaces its
content-addressed target with a directory or link and requires preservation and
refusal. Startup and migration refusal retain a local sentinel composition,
tempo, prior manifest and outside bytes. Job publication refusal also checks
failure replay through the retained request ID. All fixtures remain uncompiled
and unrun; Windows junction behavior has no native fixture or execution here.

The guard compares canonical text rather than pinned filesystem handles or inode
identity. It cannot establish safety against simultaneous external replacement,
hard-link aliases or adversarial races, and it does not universally guard manifest,
checkpoint, media or lock I/O. Existing 16 MiB buffering, synchronous publication
without a hard deadline, storage growth and incomplete full-app acceptance remain.

A read-only installed-tool audit extended the earlier PATH check. Common CMake,
LLVM, Qt, MSYS2, MinGW, vcpkg and user-program locations had no usable executable
or SDK. Visual Studio 2019/Shared folders exist, but recursive executable-name
searches there and in the workspace found no cl.exe, cmake.exe, ninja.exe,
clang++.exe or qmake executable. Python's cmake import resolves only the
repository's namespace directory; ninja/PySide/PyQt modules were absent. This was
not an exhaustive disk audit, and no installation, download or escalation was
attempted. Native build tools, Qt SDK and initialized pinned submodules are still
unavailable. Source diagnostics and fixture inventories do not establish Qt/MOC
build correctness, platform persistence, audio/plugin/GUI compatibility, provider
musical judgment, CPU/RSS behavior or full DAW completion. The commission remains
active and completion remains unproven.

## Native CC attempt coherence across input, output and cleanup

2026-10-14 source continuation; no C++ compilation, Qt test, native LMMS, DSP or
hardware execution occurred. The prior archive-path turn made source progress.

The rack's cached value could outlive a competing CC sent directly to the native
instrument through input or the track's output path. At the next tick, an unchanged
model then suppressed its controller even though the instrument had received a
different value. Reset All Controllers likewise left earlier rack attempts valid.
A broad refreshing flag also suppressed cache invalidation for direct native
silence or nested channel-mode input inside a controller callback. Conversely,
clearing the entire cache for every rack mode command would repeat commands on
subsequent ticks. Rack-generated cleanup clears LMMS's pedal state, so retaining
every previous value attempt could leave native sustain inconsistent too.

The existing instrument delivery paths now share CC attempt bookkeeping. Valid
CCs invalidate the affected prior attempt before entering the receiver; reset-all
invalidates every prior attempt. Normally returned attempts on the rack's selected
channel retain their byte value, while other-channel attempts remain conservative
and force active rack values to reassert. Exceptions leave the attempted controller
invalid. A native invalidation generation and QPointer receiver identity prevent
completion from concealing nested CC input/output or native state transitions.
The refresh loop no longer independently marks a controller delivered after
processInEvent returns. The cache deliberately records attempts, not acceptance:
a normally returned false still suppresses an unchanged ignored rack value, as
before, avoiding a repeated unsupported-controller warning on every tick.

Live input does not write or activate project rack models. Existing MidiPort
enable/channel filtering remains before native dispatch. Disabled rack values do
not compete with input. An enabled active rack intentionally reasserts native
model values after changed or other-channel input at the next tick, preserving
native timing/frame offsets and activecc serialization. No external UI controller,
substitute event model or separate generator was introduced.

Native sustain handling now belongs to the same CC dispatch: sustain release and
Reset All Controllers complete retained sustained MIDI-input notes through their
existing recorded-length and midiNoteOff path, also for native output. The reset
message is CC 121 in the [MIDI Association controller table](https://midi.org/midi-1-0-control-change-messages).
This does not establish every device's reset semantics or per-channel sustain:
LMMS retains one track-global pedal state. Reset invalidates again after native
release callbacks, so CC attempts made by those callbacks cannot survive the
later reset. Invalid CC controller/value/channel ranges are refused before native
sustain mutation or receiver dispatch.

Subsequent raw-event review corrected that refusal claim: the initial CC guard
used convenience accessors that mask/narrow parameters, so its source-only checks
did not prove malformed raw fields were refused. The following continuation
replaces them with signed raw-field checks and adds alias fixtures. The 24/53
inventory below records authored cases at this point, never native test passes.

The current rack event is identified by its exact in-scope event address for
channel-mode cleanup. Public native silence and nested input always invalidate
attempts. Exact rack mode cleanup retains completed channel-mode attempts but
invalidates value controllers, allowing one next-tick restoration of pedal/model
state without endlessly resending the mode command. Existing removeIPH behavior
is preserved: incoming mode cleanup passes false and retains the instrument play
handle. The event-address marker is restored by scope guard on return or throw;
it is not a retained event queue. New generation/event fields are transient and
require a full native rebuild; project persistence still uses existing models
and activecc rather than serializing delivery state.

InstrumentMidiCCTest now authors 24 cases / 53 scenarios, adding 13 cases and
29 scenarios. New rows use actual MidiPort filtering, native InstrumentTrack
input/output/refresh/silence, model serialization and native NotePlayHandle
completion with a deterministic receiver. They cover same/changed/other-channel
attempts, disabled or filtered input, disabled rack control, native output, reset
reassertion, sustained-note completion exactly once, nonrepeating rack reset/mode
commands, failed attempt retry, false-return suppression, nested delivery, callback
silence/rack transitions/channel mode, and malformed CC ranges. Mode rows require
native pedal/value restoration once. Existing parameter discovery, native history,
recovery, loading and activation cases remain. No fixture establishes real plugin
acceptance, waveform correctness, external MIDI driver behavior or a live audio
clock; all native cases remain uncompiled and unrun.

Generation invalidation can conservatively replay an outer controller after an
unrelated nested CC. An ignored controller needs a later model/state trigger;
the cache is not a plugin acknowledgment protocol. Native value automation still
does not preserve a complete ordered stream of identical commands, RPN/NRPN or
14-bit pairs. Arbitrary synchronous track destruction or instrument replacement,
throwing/reentrant sustained-note completion callbacks, cross-channel pedal
semantics, actual Qt/MOC/ABI correctness and latency/CPU/RSS are unverified. Native
build tools, Qt SDK and initialized pinned submodules remain unavailable. Source
diagnostics cannot prove legacy project/plugin/GUI/audio compatibility, real
provider musical judgment, full autonomous execution or complete DAW acceptance.
The full engineering commission remains active and completion remains unproven.

## Native MIDI raw-event boundaries and balanced note delivery

2026-10-14 source continuation; no native C++ compilation, Qt test, sanitizer,
LMMS/audio, MIDI driver or hardware execution occurred. The previous continuation
made source changes; it did not establish the authored tests passed.

Review of the actual MidiEvent implementation found a flaw in the prior CC
validation: controllerNumber masks its signed raw field to seven bits, while
controllerValue narrows to uint8_t. Negative, high-byte or oversized raw parameters
could therefore appear valid. The prior malformed controller rows would not all
satisfy their refusal assertions. The guard now checks signed param(0)/param(1)
before using those accessors. Six additional alias/maximum rows bring the CC suite
to 24 cases / 59 scenarios. All are uncompiled and unrun. Source delimiter checks
and case inventories could not detect this semantic mistake.

The same boundary review found native input reading note arrays before validating
keys for onsets, releases, key pressure and note-panning meta messages. Masked
velocity could reinterpret malformed values as valid onsets/releases. Raw pitch,
program and channel pressure also reached their model/receiver paths without
local bounds. A common native-track entry check now validates meaningful signed
fields for seven channel message types and the known LMMS panning extension
before state/index/receiver access. Channel messages use 0..15; note/key pressure
keys use native NumKeys=128 and byte data 0..127; pitch uses 0..16383. Program and
channel pressure validate their first field, leaving irrelevant padding alone.
Input preview -1 channel is retained for note/key pressure/known note-panning,
as used by Piano and PianoRoll. The native panning extension retains its signed
-128..127 range. This does not remove or emulate graphical entry points. System,
SysEx and unknown meta/plugin-specific dispatch retains its existing behavior.

Zero-velocity native note output previously incremented onset counts, marked the
piano key pressed and emitted newNote even though the wire encoder normalized
the same event to release. Output now selects its native release path instead,
consistent with [RFC 6295 Appendix A.6](https://www.rfc-editor.org/rfc/rfc6295.html#appendix-A.6)
and the existing encoder. Unmatched releases previously decremented overlap counts
below zero, corrupting later retrigger accounting. They still deliver a cleanup
release when no voice is counted, but only positive counts decrement. Onsets
saturate at INT_MAX to avoid signed overflow; exact counts above that cardinality
are not preserved. Existing transposed-key checks, zero native receiver release
velocity, chosen output channel and original MIDI-port forwarding remain.

Input/output entries scope native engine changes. Output onset/release scopes the
note mutex and releases it when a receiver callback throws; input no longer keeps
an unbalanced manual requestChangeInModel/doneChangeInModel pair on release failure.
This makes subsequent native cleanup/output possible without asserting rollback
of partially delivered notes, counters, piano state or already-released handles.
Same-key overlap counts and sustain remain track-global rather than per-channel.

InstrumentMidiEventTest is registered with a 180-second timeout and authors eight
cases / 100 scenarios. A deterministic native instrument receiver and actual
InstrumentTrack, Piano, model, NotePlayHandle and signals cover malformed input
and output in two 39-row matrices; fourteen valid seven-type endpoint rows;
native preview and signed panning; lowest/highest input keys; zero-velocity native
release; two stray releases followed by overlapping retriggers and paired release;
and native channel cleanup plus further output after onset/release/pressure
receiver exceptions. If a lock remains held, that process-level test timeout bounds
the failure. The fixture does not use windows or a replacement music model, and
it does not exercise a live clock, rendered PCM, actual plugin processing or MIDI
hardware. All new and existing native scenarios remain uncompiled/unrun.

Scope checks and inventories are source-only evidence. They cannot establish
Qt/MOC/SDK build correctness, exception recovery in real plugin/driver callbacks,
sanitizer safety, arbitrary synchronous reentrant note delivery/model destruction,
all MIDI system/reset semantics, legacy ecosystem compatibility, resource bounds
or complete autonomous production. Native build tools, Qt SDK and initialized
pinned submodules remain unavailable. The complete DAW objective is unchanged;
completion is unproven and the full engineering commission remains active.

## Shared raw CC validation for native bindings and detection

2026-10-14 source continuation; no C++/Qt compilation, native test, LMMS DSP/audio,
MIDI driver, hardware, GUI or provider execution occurred. The previous raw-event
boundary continuation changed authoritative source and authored native fixtures.

The shared native detector already checked signed raw CC parameters. Connected
MidiController input still read controllerNumber/controllerValue directly, whose
masking/narrowing can turn malformed raw parameters into accepted bindings or
values. With an omni channel selection, invalid channels were not independently
rejected either. Normalized last/previous values and interpolation buffers could
therefore change or exceed 1. Controller::currentValue fits scalar values to 0..1,
but AutomatableModel's directly connected sample-buffer path maps controller
samples linearly/logarithmically without that scalar fitting step. A fitted scalar
does not prove a safe or unchanged parameter sample buffer.

MidiEvent now has a const isValidControlChange predicate that checks event type,
channel 0..15 and signed raw controller/value 0..127 before convenience accessors.
The native detector, parameter-owned MIDI controller and instrument CC entry/
dispatch use it. The former local instrument predicate and detector's duplicate
raw checks were replaced by this shared check. Detector/controller entries then
scope native engine changes, apply their existing filters and update detected or
normalized state. Invalid events return before last/previous values or input
valueChanged emission. Valid interpolation, selected/omni channels, CC selection,
first-only/latest detection, source subscriptions and transient-value persistence
retain their existing native paths. The predicate adds no event storage, model,
operation schema, external UI controller or alternate music representation.

StudioMidiControllerTest now authors eleven cases / 49 scenarios. Four new cases
add 22 scenarios: thirteen malformed controller/value aliases, invalid channels
and unrelated type must preserve the actual parameter-owned binding, stored value,
mapped scalar and every normalized buffer sample; four rows check valid channel/
controller filtering; three rows retain finite 0/middle/127 endpoints; two rows
compare uncached native linear/log parameter sample buffers before and after a
malformed value. Actual structured controller.midi.connect creates the bindings,
and native period counters advance both controller and model buffers. Processing
is explicitly stopped during these tests; no fake scalar replaces buffer checks.

StudioMidiLearnTest now authors nine cases / 36 scenarios. Four new rows check
thirteen malformed events before or after a valid detection in first-only or
latest mode. Detection fields and input notifications must remain unchanged;
subsequent valid input passes through the native queued MIDI client fixture,
retains the appropriate detection and copies its binding/value accurately.
Existing detector raw validation and GUI/core behavior are preserved. Existing
input health, timing/cancellation, named-source, binding ownership, history,
replay and native persistence scenarios remain. All native cases are uncompiled
and unrun. The source inventory also checks uniqueness/completeness of these
suite slots and their authored row counts, which remains indirect source evidence.

The shared predicate validates these CC processor entries, not all messages or
producer/backend boundaries. The fixtures use a queued client and manually advanced
native buffers rather than live DSP/driver clocks or physical controls. Engine
guards do not establish safe arbitrary callback destruction or instrument/model
replacement. This does not prove general controller-buffer safety, GUI/plugin/audio
or legacy project compatibility, compiled Qt/MOC/ABI correctness, latency/CPU/RSS,
real provider musical judgment or full autonomous execution. Native build tools,
Qt SDK and initialized pinned submodules remain unavailable. The complete DAW
objective remains active; completion remains unproven.

## Native external MIDI admission during render reservations

2026-10-14 source continuation; no native compilation, Qt test, rendered PCM,
MIDI driver/hardware, GUI, plugin or provider execution occurred. The previous
shared raw-CC continuation made source progress and left all native cases unrun.

InstrumentTrack previously ignored External events while Song exported, but
connected MIDI controllers could change their normalized values and sample
buffers during that export. MidiPort delivered queued events without a common
export/reservation boundary. Checking only current flags would also let an event
queued during, or waiting across, an export apply after it finished. The native
render preparation/restoration window retains an audio-device reservation even
outside Song's export flag, including after restoration failure.

The selected MidiClient now owns shared atomic admission state with a generation:
even admits External input and odd excludes it. Only native model-owned export/
device transitions publish changes. Driver intake reads client-owned atomics;
it does not read Song or audio-device pointers on backend threads. Each queued
packet retains its admitted generation. A delivery tracks observed transitions
and removes stale External packets before queue-capacity checks, preserving
Internal ordering and preventing a stale full queue from overflowing fresh input.
A packet popped before a transition rechecks the generation under native model
ownership before entering its processor, so a completed intervening export is
still detected after current flags reopen. Source classification is trusted native
metadata rather than an authorization boundary.

Admission storage is shared by ports and posted callbacks, avoiding a retained
client pointer after destruction. Fallback retirement delivery adopts the new
client's state. No DSP or native model call occurs while registry/backend locks
are held. Stale cleanup scans happen once per observed transition, not on every
unchanged-generation input. Intentional external discards increment the existing
dropped counter without reporting queue overflow/backend failure or marking an
undelivered note as requiring cleanup. Storage can remain until the next intake
or drain; the existing 1024-packet per-port bound remains. A shared atomic state,
one generation stamp per queued packet and per-delivery observation change transient
layout/allocation and require native rebuilding. Actual memory/layout/platform
atomic behavior and CPU/latency/RSS have not been measured.

AudioEngine synchronizes admission on stored-device acquisition, successful
backend replacement release, post-MIDI-selection restoration reservation, and
both successful restoration paths. Song synchronizes export begin/end and export
initialization failure. Combined reasons remain closed until both are released;
restoration failures keep the stored device and admission blocked. Successful
notification/startup must precede reopening. Model guards around these short
state publications do not span device stop/join/start. Song initialization now
releases its export flag on later initialization exceptions too, without claiming
rollback of every timeline/playback effect. Throwing stopExport retains its
existing export state until successful cleanup.

MidiPort, connected MidiController, AutoDetectMidiController and InstrumentTrack
also refuse direct External input during export or retained device reservation.
Internal native playback/rack events remain usable. Input-failure cleanup refuses
to acknowledge a suppressed all-sound-off during those phases and retains its
failure for retry. Independently managed clients do not receive the selected
client's admission transitions automatically; their direct native processors
still check current render/reservation flags. Supported engine-managed backends
use the selected client's queue/admission path.

MidiInputRenderTest is registered with a 240-second timeout and authors ten cases /
20 scenarios. Four staged rows cover device/export/both/post-export reservations;
other rows cover full stale queues, bursts exceeding capacity without invented
failure, Internal input, four direct processors across two phases, rejected export
range, busy restore and a real AudioDummy restoration notification exception/retry.
A controlled worker holds native model ownership until the main-thread packet is
popped, runs actual Song start/stop export, then lets delivery resume; the old
packet must be dropped even though current export flags have cleared. The PCM
fixture constructs native sample/Mixer/controller state through existing models
and capabilities, renders baseline and reservation-callback traffic WAVs through
audio.render, requires decoded nonzero PCM equality and preserved CC gain, and
accepts fresh input afterward. It does not claim that generated fixture audio is
the commission's deliverable. All scenarios, including PCM/concurrency behavior,
remain authored, uncompiled and unrun.

Source diagnostics and fixture inventory cannot establish compile/Qt/MOC/ABI,
real hardware timing, deadlock freedom, general callback destruction safety,
every MIDI system/reset or third-party producer path, Native DSP/audio correctness,
legacy project/plugin/GUI compatibility, complete recovery or provider musical
judgment. Model ownership/native callbacks still have no hard execution deadline.
Native build tools, Qt SDK and initialized pinned submodules remain unavailable.
The full DAW commission remains active and completion remains unproven.

## Recoverable native export transport cleanup

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred. The previous admission
generation work changed native source and authored unexecuted fixtures.

Song::stop held its scoped requestChangesGuard and acquired a redundant manual
recursive model lock later. A returned timeline or model callback exception could
skip doneChangeInModel, leaving that extra ownership held even after the scoped
guard unwound. A same-thread recursive retry could conceal the leak while another
native worker blocked. Stop now relies on its existing whole-function scope,
without changing the normal transport/timeline/note/controller cleanup sequence.
This does not roll back every side effect of an interrupted stop.

ProjectRenderer treats a returned stopExport exception as a failed execution and
refuses publication. Previously the retained Song export flag also made
restoreAudioDevice report Busy forever, leaving no structured recovery path after
the worker finished. Song now marks transient export cleanup pending on any
returned stop/sync failure and clears it only after successful cleanup. StartExport
rejects an older pending cleanup. Pending state also covers startup that cleared
its export flag but then failed transport cleanup. Native live MIDI stays excluded
for this reason even without a stored device or current export flag.

AudioEngine restoration retains its wrong-thread/running-execution checks and
rejects ordinary active export. Marked cleanup is distinct: after execution ends,
the owner reserves the restoration against reentry and retries native
Song::stopExport before touching/releasing the stored audio device. Standard and
nonstandard retry failures return export-stop-exception and keep pending state
and reservation. A successful stop/sync clears the transport reason, while a
stored device continues excluding live input until notification/restart succeeds.
No-device pending cleanup also uses the existing audio.device.restore operation.
The additional native failure enum is appended rather than renumbering existing
values; transient Song state requires a native rebuild and is not project XML.

Transport and restoration inspection expose exportCleanupPending, pending/failure
and retry eligibility. Pending cleanup is considered by native playback, render,
backend/callback-join and DSP-reset admission. Agent instructions inspect those
fields and use audio_device_restore when eligible. A fresh request ID is necessary
to execute a retry; receipt replay remains historical. Successful later cleanup
does not acknowledge the failed render or replace its earlier failed result.
Active render/context protection remains authoritative, not a license to interrupt
a running worker or externally finalize an encoder during active output handling.

SongExportRecoveryTest is registered with a 240-second timeout and authors seven
cases / 12 scenarios. They use actual native Song/Timeline/model ownership,
MidiController, renderer/capabilities and restoration paths with returned callback
exceptions. A foreign worker must acquire model ownership after an interrupted
stop, which exposes a leaked recursive lock rather than concealing it with an
owner retry. Four rows combine direct/stored-device cleanup with standard and
nonstandard exceptions, require observable failed retry, preserve MIDI values
while pending, then recover and accept fresh input/export. Other rows reject a
real native executing-context reservation, prevent nested restore from a stopped
callback, distinguish ordinary transport failure, and recover pending cleanup
after initialization already cleared export. Native sample-render rows throw from
export stop, require failure and preservation of the existing destination, require
automatic unwind recovery, then render again through the same native capabilities.
They do not substitute fixture audio for the full music-production deliverable.
All native and artifact/lock scenarios remain authored, uncompiled and unrun.

Return-path lock balancing and explicit retry do not establish rollback of
partially changed timeline/playback, automated-value ownership, native notes,
plugins or physical MIDI. Throwing destructors, arbitrary callback destruction/
replacement, asynchronous Qt event-loop exceptions, memory corruption and blocked
native calls remain outside these guarantees. Context reservation tests use the
native API, not an actually hung plugin. Device joins/model callbacks have no hard
deadline. Source checks cannot prove Qt/MOC/ABI/build, lock, artifact/PCM, driver,
GUI/plugin/legacy compatibility, CPU/RSS, real musical judgment or autonomous DAW
acceptance. Native build tools, Qt SDK and initialized pinned submodules remain
unavailable; the full commission stays active and completion remains unproven.

## Guarded native Song automation ownership

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred.

AutomationClip already holds QPointer targets, but Song retained the last pass's
AutomatedValueMap with raw model keys. Track deletion or plugin parameter
replacement between ticks could leave a retired model for the next pass or stop
to dereference. An allocation at the same address could also inherit an ownership
notification intended for the previous object. Song now retains a vector of weak
model references. Native stop copies that guarded list and returns controller
ownership only to surviving objects; project replacement clears the list.

The native current value map still computes curve values and precedence. Before
any recording/controller callbacks, Song copies its values and guarded targets
into a local snapshot. Recording curves are likewise snapshotted as weak
references, and the first recorded model is guarded across controller ownership
notifications. Another target or curve retired by an earlier callback is skipped
without dereferencing the stale raw map key. Surviving current targets are
retained before value application, so a returned callback exception leaves native
stop able to restore their controller ownership. Returning ownership from a prior
pass also uses a guarded local copy. TrackContainer omits null legacy weak targets
from direct and nested pattern collections. Existing native recording format,
linear/logarithmic scaling, curve precedence and live-controller handoff remain.
There is no new agent operation, alternate automation engine or project XML field.

SongAutomationOwnershipTest is registered with a 180-second timeout and authors
ten cases / 15 scenarios. It calls the actual native automation routine under
engine model ownership and uses native Song, track/clip models, PatternStore,
MidiController and ControllerConnection. Rows retire an instrument track between
passes, reconstruct a model at precisely the retired address, remove a curve
while keeping its controlled model, retire a different snapshotted target during
apply, retire a different curve/target during recording and a retained target
during stop, preserve legacy null references without their destruction
subscription, replace a project while an independent old model survives, check
linear/logarithmic curve values, and throw from a controller ownership callback
before recovering through native stop. All remain authored, uncompiled and unrun.

QPointer does not pin a model or synchronize concurrent access; native model
ownership remains required. The public current-value map still has transient raw
keys. These guards do not make self deletion within AutomatableModel::setValue
or AutomationClip::recordValue safe: those native methods may continue accessing
themselves after callbacks. Arbitrary track deletion during subsequent DSP,
reentrant transport/project replacement, throwing destructors and asynchronous
exceptions remain outside this change. New retained storage and per-pass weak
snapshot copies require native rebuild and actual allocation/latency/CPU/RSS
measurement. Source checks cannot establish C++/Qt/MOC/ABI compatibility, native
callback/thread behavior, rendered audio, GUI/plugin/legacy compatibility or full
autonomous production. Native build tools, Qt SDK and initialized pinned
submodules remain unavailable. The complete DAW commission remains active and
completion remains unproven.

Source diagnostics pass across 411 changed/new C++ files and 147 native production
operations. Fixture registration/inventory reports ten cases / 15 scenarios;
Python syntax, touched-file whitespace/newlines and git diff --check pass. These
checks do not compile or execute native code.

## Native persisted controller slot restoration

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred.

Song previously iterated every child node in the saved controller rack. A comment
or other non-element could therefore become a dummy slot and shift subsequent
native ordinal controller bindings. Restoration now iterates elements only.
Controller::create checks type-to-integer conversion; malformed/missing types
return unavailable instead of becoming the zero-valued dummy type. Unknown
numeric types retain the existing unavailable factory result. Every unavailable
element, including repeated explicit dummy types, occupies a distinct temporary
slot. Those slots bypass addController's pointer deduplication because the native
dummy instance is shared. Native project errors report unavailable controllers.

ControllerConnection::loadSettings checks ID conversion, preventing malformed
text, overflow or fractional syntax from silently becoming rack controller zero.
Valid native integer syntax and deferred project binding remain. Finalization
now checks for dummy targets after attempting resolution in the same pass; a link
to an unavailable slot is removed and reported rather than surviving as midpoint
modulation after the dummy rack entries disappear. Other ordinal links resolve
before placeholder removal, and native resaving assigns the surviving controllers
their current rack indices. Unavailable controller behavior cannot be reproduced;
the application reports the missing state instead of claiming compatibility.

Connection finalization snapshots its registry into QPointer references. A
notification that retires an earlier connection no longer shifts the index walk
past a later pending link. This guard covers resolution, not arbitrary controller
or model destruction during the later cyclic-model traversal. Caller-owned native
model synchronization remains required. No new agent operation, project format,
alternate modulation engine or external automation layer is introduced.

ControllerConnectionRestoreTest is registered with a 180-second timeout and
authors eight cases / 37 scenarios. Four native save/annotate/load rows cover
leading, between and trailing comments plus mixed text/processing instructions,
requiring intended volume/pan links to the two actual rack controllers. Eleven
invalid-ID rows require no alias to controller zero, removal and native error
reporting; five valid legacy integer rows retain binding. Nine malformed-type
rows require an unavailable factory result. Five native project rows replace a
controller with an unknown/malformed/missing/dummy/overflow type, retain the later
valid link and stored values, continue editing, then save/reload the repaired
project and require the surviving controller's remapped index. Another row
preserves a controller after two explicit dummy slots. Embedded MIDI restoration
checks channel/controller settings, native input/value mapping and owned lifetime.
A finalization callback retires an already-processed link while another resolves,
requiring the last pending link to resolve too. These are actual native model and
Song persistence paths; all scenarios remain authored, uncompiled and unrun.

This work does not prove complete legacy/plugin/peak-controller compatibility,
transactional restoration, arbitrary synchronous self deletion or reentrant
project replacement, asynchronous Qt exception recovery, physical MIDI or audible
modulation. Weak snapshots do not pin models/controllers or make concurrent access
safe. Source checks cannot establish C++/Qt/MOC/ABI, native GUI/audio behavior,
real provider continuity or allocation/latency/CPU/RSS costs. Native build tools,
Qt SDK and initialized pinned submodules remain unavailable. The complete DAW
commission remains active and completion remains unproven.

Source diagnostics pass across 412 changed/new C++ files and 147 native production
operations; the restore fixture inventory has eight cases / 37 authored scenarios.
Python syntax, touched-file whitespace/newlines and git diff --check pass. PATH
contains no native compiler/build tool or LMMS executable, and all 17 pinned
submodules remain uninitialized. These checks do not compile or execute LMMS.

## Bounded live native warning observations

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred.

Structured project loads already reject native warnings, and StudioRuntime
restores the prior native checkpoint while retaining attempted-load diagnostics
in the failed result. That strict behavior remains. Routine bounded observations
previously exposed only nativeWarningsPresent, leaving the agent without the
current reasons unless it requested a full project/error summary. Project summary
now includes the distinct nativeWarningCount and transient diagnosticGeneration.
The existing project.inspect operation adds section warnings; no new operation or
external diagnostic engine is introduced.

Song exposes its native deduplicated error entries for reads under model
ownership. Error collection increments a process-local unsigned 64-bit generation
when a count changes, and nonempty clearing advances it too. Signed occurrence
counts saturate at their existing native integer maximum instead of overflowing
during a long session. The existing human error summary and native warning sources
remain authoritative. This is live diagnostic state, not project XML or durable
event history; generations can repeat across processes or eventually wrap.

Warning entry pages use existing default 128 / maximum 1024 item limits and a
512-KiB serialized item-array budget. Optional diagnosticGeneration rejects stale
pages. Each entry has its native index, occurrence count, saturation flag and a
bounded message chunk. Nested messagePage uses UTF-16 units with default 1024 and
allowed 2..8192 limits. Native string slicing bounds conversion work per entry;
valid surrogate pairs remain intact and starting inside a pair is refused.
Reading an individual long message keeps its entry offset fixed with limit=1 and
follows nested nextOffset through messageOffset. Top-level nextOffset advances
entries. Reading never clears warnings or asserts successful repair.

Inspection retains native model ownership, suppresses Qt event dispatch, and
checks cooperative cancellation while scanning/skipping entries. If a supplied
work callback changes diagnostics, the generation is rechecked before iterator
dereference and final publication; mixed-generation pages are refused. The agent
prompt learns warning entry/text pagination, rediscovery after changes/restart,
and the distinction between current diagnostics and historical failed-load
nativeWarnings. Receipt replay keeps original observations even after live
warnings clear, while fresh calls describe current state.

StudioWarningInspectionTest is registered with a 240-second timeout and authors
nine cases / 27 scenarios. It uses actual Song diagnostic collection, native
capabilities, runtime receipts/history, strict project load/checkpoint recovery
and the resident agent. Cases cover deduplicated occurrences and generation,
escaped message arrays hitting the byte budget, read-only paging and replay after
clearing, four Unicode chunk widths with JSON round-trip reconstruction, three
generation mutations, thirteen invalid selectors, cancellation while skipping
hash entries without dispatching queued Qt events, controlled work-callback
mutation, and scripted Chat/Responses warning discovery from a compact native
observation. A real saved project has its controller type made unavailable;
structured load must fail with historical diagnostics and restore the original
tempo/controller checkpoint, then replay the old failure while newly collected
live diagnostics remain distinct. All scenarios remain uncompiled and unrun.

These bounds do not cap total native warning storage, full legacy inspection,
human errorSummary, failed-result nativeWarnings, parser or archive memory.
Warning iteration order is valid only for the unchanged same-process generation;
offset paging scans preceding hash entries, so bounded payload is not constant
work. Counts lose exact repetition after saturation, generation can wrap, and
text beyond the supported signed native offset range is refused. Model
synchronization remains required; arbitrary object deletion/reentrant project
replacement is outside this change. Scripted warning discovery is not real
provider musical judgment or proof of compatibility, audio, native resource use
or full autonomous production. Native build tools, Qt SDK and initialized pinned
submodules remain unavailable. The complete DAW commission remains active and
completion remains unproven.

Source diagnostics pass across 413 changed/new C++ files and 147 native production
operations. Warning fixture registration/inventory has nine cases / 27 authored
scenarios. Validator Python syntax, touched-file whitespace/newlines and git diff
--check pass. No native compiler/build tool or LMMS executable is on PATH; all
17 pinned submodules remain uninitialized. These checks do not execute LMMS.

## Native LFO phase precision after delayed reads and seeks

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred.

LfoController catches up an unread oscillator by adding skipped native periods
to its phase before computing the next sample buffer. Previously it narrowed the
period difference to int, then converted elapsed frames to float before dividing
by its duration. Even when the wide native frame product remained correct, the
resulting large float phase could lose the fractional part or swallow its small
per-sample increment. The native waveform loop could then produce a constant or
incorrectly phased buffer. Seek/playback notifications stored absolute-frame
phase directly in the same float state, exposing late marker exports too.

The skipped-period difference now retains the native counter width. Catch-up
computes elapsed phase in double and wraps it before narrowing to the existing
float waveform loop. Native updatePhase likewise wraps double absolute-frame
phase before storing the fractional state. There is no new oscillator, external
generator, project-format field or agent operation. Existing native waveform
definitions, duration/multiplier controls, base/amount sample mapping and
per-period cache remain authoritative. Numeric results affected by the prior
large-phase rounding are expected to change; compatibility/PCM remains unrun.

LfoCatchUpTest is registered with a 300-second timeout and authors five cases /
11 scenarios. Native clock seeding uses a friend helper under engine model
ownership, with the audio device stopped and prior counter restored afterward;
this exercises elapsed-period inputs without claiming an elapsed-time benchmark.
Three rows use delayed normal/fast/slow native speed multipliers and compare the
entire caught-up saw buffer against periodic sample expectations, requiring
variation. A fourth row exceeds int's period range where the native long clock
can represent it, explicitly skipping 32-bit-long platforms. Two rows perform
actual late Song seeks and compare sine/saw samples. Other cases require repeated
same-period reads to stay cached, next-period reads to advance, and caught-up
native parameter sample buffers to retain linear/logarithmic mapping.

Two marker-export rows create actual native sample/Mixer/LFO bindings, render an
early excerpt through audio.render, move it to tick 960000 and render between
960000..960192 at tempo 600 / 48 kHz. A two-second LFO has advanced exactly 1000
cycles at that marker, so decoded finite, varying PCM must match the early
excerpt within the authored tolerance. One row saves/reloads the late native
project and verifies its rack binding before rendering. Native transport,
renderer, persistence and sample decoding remain the execution path. This
fixture is verification material, not the commission's music-production
deliverable. Every waveform/cache/parameter/PCM scenario is uncompiled and unrun.

The native controller/model signed-long clocks, their ultimate overflow/reset
behavior and platform limits remain. Double has finite precision for extreme
frame/time values, and the ordinary waveform loop/fractional state still uses
float. Sample-and-hold noise remains native stochastic state; no random seed,
deterministic-noise promise or complete render determinism is introduced.
Arbitrary callback deletion, blocked DSP, physical output, GUI/plugin/legacy
compatibility, native C++/Qt/MOC/ABI, actual PCM, real provider judgment and
allocation/latency/CPU/RSS behavior require separate verification. Native build
tools, Qt SDK and initialized pinned submodules remain unavailable. The complete
DAW commission stays active and completion remains unproven.

Source diagnostics pass across 415 changed/new C++ files and 147 native production
operations. LFO fixture registration/inventory has five cases / 11 authored
scenarios. Validator Python syntax, touched-file whitespace/newlines and git diff
--check pass. A separate IEEE-754 arithmetic diagnostic records that a phase of
5000.1025390625 at the representative 44.1-kHz two-second oscillator loses its
per-sample step, while the wrapped fractional phase retains a nonzero step. This
is arithmetic evidence only, not execution of the native oscillator or renderer;
its JSON is build/validation/native-lfo-phase-arithmetic-20261014.json. No native
compiler/build tool or LMMS executable is on PATH, and all 17 pinned submodules
remain uninitialized.

## Native controller defaults, time helpers and notification lifetimes

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred.

Controller construction populated its native buffer with normalized midpoint
values but left m_currentValue and m_sampleExact uninitialized. A first read at a
nonzero offset could return the former without an initial frame-zero read, and
parameter sample-buffer classification could inspect the latter. The base now
initializes its cached value to 0.5 and sample-accuracy flag false. LFO, MIDI and
peak implementations retain their existing explicit true selection; native
scalar/buffer mapping and interpolation remain authoritative.

The public runningFrames helper multiplied by the native wide frame count but
returned unsigned int, narrowing after 32-bit frame rollover. runningTime divided
the integer frame count by the integer sample rate before its float return,
discarding fractional seconds. runningFrames now returns f_cnt_t, and runningTime
divides in double before its existing float return. The in-tree GUI modulation
throttle still uses its existing frame modulo. The changed public C++ return
width requires native rebuilding and out-of-tree consumer API/ABI review; no
binary compatibility or plugin execution is claimed.

Native per-period controller notifications walked the raw registry while direct
callbacks could erase or append its entries. Retiring an earlier entry could skip
a later one; retiring a later entry or reallocating the registry could invalidate
the walk. triggerFrameCounter now captures weak references before callbacks and
skips retired targets. Newly registered controllers begin notification on the
next pass. Existing registry naming, native signals and the post-notification
period advance remain. A returned exception still leaves the counter unadvanced;
this does not roll back prior callback effects or constitute general Qt recovery.

ControllerStateTest is registered with a 240-second timeout and authors seven
cases / 17 scenarios. Three rows construct the actual native base over different
prior-storage patterns and require defined first nonzero-offset reads, midpoint
buffers and explicit sample-accuracy opt-in. Two rows attach native Dummy
connections to linear/log models and require scalar mapping without sample
classification after native interpolation settles. Four rows require native
LFO/MIDI controllers to retain actual per-sample reads before an initial frame-zero
read, after advancing the native period. Three clock rows preserve fractional
seconds and frame values beyond unsigned-int range using controlled native clock
inputs. Notification rows retire an earlier/later controller or create one during
dispatch, then require remaining targets exactly once and newly registered ones
on the next pass. A direct exception case requires the clock unchanged and a
later successful pass. The final case executes actual AudioEngine periods,
retires another Song rack controller after mixing and requires the remaining
notification and no DSP failure in that and the next period. All remain authored,
uncompiled and unrun; controlled clocks are not elapsed-time benchmarks.

Weak snapshots protect this notification traversal under caller-owned model
synchronization. They do not pin models/controllers, protect concurrent raw
registry mutation or arbitrary self deletion/reentrant project replacement, or
make native setters safe after deleting themselves. Per-period snapshot
allocation/reference work, cache initialization and public return-width changes
require native compiler/Qt/MOC/platform, GUI/plugin/legacy and allocation/latency/
CPU/RSS verification. Native signed-long period overflow/reset limits and the
float time return's precision remain. No audible PCM, physical timing or real
provider musical judgment is established. Native build tools, Qt SDK and pinned
submodules remain unavailable. The complete DAW commission stays active and
completion remains unproven.

Source diagnostics pass across 416 changed/new C++ files and 147 native production
operations. Controller-state fixture registration/inventory has seven cases /
17 authored scenarios. Validator Python syntax, touched-file whitespace/newlines
and git diff --check pass. No native compiler/build tool or LMMS executable is
on PATH, and all 17 pinned submodules remain uninitialized. These checks do not
compile or execute the native controller, engine or tests.

## Native value-buffer freshness after explicit DSP retry

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred.

A failed native period can compute controller and parameter buffers before a
later play/effect/mix exception prevents the normal period counters from
advancing. Explicit reset previously cleared only the DSP failure latch. If a
user repaired a parameter or sent new MIDI before retry, a buffer still marked
current for that unchanged period could supply failed-period values again.
Successful reset of a real failure now invalidates both native value-buffer
families lazily, without rewinding or advancing musical clocks.

Controller and AutomatableModel each have a transient unsigned 64-bit buffer
generation and a per-instance successful-generation stamp. Native cache reads
require both period and generation to match. Controller scalar/sample-buffer
access and every successful parameter-buffer outcome update the stamp only
after native computation returns, including linked, interpolated and null
results. A returned recomputation exception therefore cannot validate stale
data. Connection-owned MIDI controllers are covered even though they are absent
from the rack notification registry. No alternate controller/parameter system,
project XML field or agent operation is introduced.

AudioEngine reset retains owner/stopped/disarmed/export/device admission and its
failure-count checks. Only a successful compare/exchange clearing a nonzero latch
advances the cache generations, under native model ownership. No-fault reset is
idempotent; refusals and failed compare/exchange leave caches intact. Existing
audio.processing.reset and inspection remain the structured interface. Receipt
replay remains historical and cannot clear a newer fault or refresh live caches;
fresh reset requests must pass current admission. Ordinary native interpolation
and its endpoint convention remain unchanged.

DspCacheRecoveryTest is registered with a 240-second timeout and authors eight
cases / 15 scenarios. Four rows run an actual native play job that reads a bound
parameter buffer then throws a standard/nonstandard exception, with linear/log
mapping. They require a silent latched period and unchanged controller clock,
observe stale old data before reset, send new native MIDI, explicitly reset,
require refreshed controller and parameter samples, and run subsequent finite,
nonzero native output periods. The expected first refresh follows native MIDI
interpolation, with the endpoint settling on the next period. Two rows refresh
cached null/manual interpolation outcomes after parameter repair; two exercise
native linked-model samples. Other cases preserve healthy caches, reject resets
from a foreign thread or while playing/device-reserved before later eligible
refresh, and throw during buffer recomputation before successfully trying again.
The runtime case preserves history and the old reset receipt while requiring
a new fault/live cache to remain until a fresh reset. A native LFO case requires
one recomputation under the unchanged clock and subsequent same-period caching.
Controlled components inject failures through native APIs; they are verification
fixtures, not a substitute music-production deliverable. All remain authored,
uncompiled and unrun.

Cache freshness is not transactional DSP recovery. Native phase/envelope and
interpolation state may already have advanced before a fault and can advance
again on recomputation. This neither rewinds plugin internals nor replays failed
audio, and custom overrides/private plugin caches remain outside the guarantee.
Unsigned generation can eventually wrap, signed-long clocks keep their native
lifetime limits, and native owner synchronization is still required. Added
Controller/AutomatableModel fields change layout and require complete rebuilding
and external API/ABI review. Comparisons/recomputation require actual native
allocation/latency/CPU/RSS measurement. No C++/Qt/MOC, real period/output, physical
hardware, GUI/plugin/legacy or provider musical-judgment evidence is established.
Native build tools, Qt SDK and pinned submodules remain unavailable. The complete
DAW commission stays active and completion remains unproven.

Source diagnostics pass across 417 changed/new C++ files and 147 native production
operations. DSP-cache fixture registration/inventory has eight cases / 15 authored
scenarios. Validator Python syntax, touched-file whitespace/newlines and git diff
--check pass. No native compiler/build tool or LMMS executable is on PATH, and all
17 pinned submodules remain uninitialized. These checks do not compile or execute
native cache recovery, output or tests.

## Live native linked-control coherence and inspection

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred.

AutomatableModel stores linked controls in a native ring. Joining a group could
prime a null/sample cache before the relationship changed; splitting it or
rewiring/retiring a controller invalidated only one model. Other members could
therefore retain inherited samples after their source disappeared, or fail to
observe new samples until a later parameter period. These native transitions
now invalidate every affected member, including both resulting rings on unlink
and inherited followers on controller/ownership changes. Unrelated interpolation
is preserved, and musical clocks are unchanged. Native controller precedence,
raw-unit fitting and ownership policy remain authoritative.

Link admission now checks connected controller dependencies across the proposed
merged group before copying values, and again after value callbacks. Direct,
indirect or group-member feedback is refused. Weak target/source references
avoid merging a retired other model after the callback; an already merged ring
is not merged twice. This does not make the value setter safe after deleting
itself or roll back earlier callback/value effects. linkToModel now returns a
native success boolean, and the GUI drop path accepts/ignores accordingly; GUI
drop and context unlink acquire engine model ownership. C++ return-contract
changes require rebuilding and external API/ABI review; GUI execution is unrun.

Existing parameter.list reports linkedParametersPresent. parameter.inspect adds
linkedOffset/linkedLimit pages (default 128, maximum 1024) with peer IDs excluding
self, scale/writable/automatable/inspectable flags, linkedParameterCount and
linkedPage continuation metadata. Peer arrays use the existing 512-KiB budget.
The native ring is traversed without materializing a complete peer vector for
inspection. Current project discovery identifies which live peers are valid
project handles; others remain visible but explicitly uninspectable. The prompt
follows peer pages and inspects valid handles separately for detailed locations.
This adds no new native operation and keeps the existing catalog/declaration
architecture; the production operation count remains 147.

Native linked persistence is NOT IMPLEMENTED. Source inspection confirms that
AutomatableModel::saveSettings writes values, automation IDs/scales and controller
connections without writing linked-ring relationships. Project XML, checkpoint
undo/redo and reopening therefore do not preserve these native groups. This is
an existing native lifecycle limitation, not a verified compatibility result.
The inspector/prompt expose runtime-only persistence. Mutating parameter.link/
unlink operations remain absent until native serialization/restoration can make
them durable. Implementing that lifecycle and safely preserving/remapping groups
through project/preset/clone/history paths remains required for the full goal;
live metadata and historical receipts do not satisfy it.

LinkedParameterTest is registered with a 300-second timeout and authors thirteen
cases / 21 scenarios. They cover immediate linear/log inherited samples after
primed caches, splitting merged groups, rewiring and owned-controller retirement,
same-period ownership changes, direct/indirect/group-member feedback before value
copy, callback-introduced feedback and another model's retirement, and unrelated
interpolation preservation. A 1050-peer native group pages through actual runtime
receipts while retaining history/revision; invalid selectors and non-project
peers are checked. Cancellation must refuse partial output without dispatching
queued Qt events. Scripted Chat/Responses jobs discover native controls, follow
peer pages and retain the runtime-only limitation. They verify routing/inspection
contracts, not real provider musical judgment or a music-production deliverable.
Every scenario remains authored, uncompiled and unrun.

Peer payload limits do not bound existing selected-parameter names/paths/choices
or current project discovery allocations. Counting scans the full ring under
cooperative cancellation; structural feedback checking materializes group
vectors and traverses controller dependencies. No constant work, hard deadline,
allocation/latency/CPU/RSS or complete native resource proof is established.
Native owner synchronization remains required; arbitrary synchronous self
deletion, reentrant project replacement and concurrent raw mutation remain
outside guarantees. Native persistence, C++/Qt/MOC/ABI, GUI/drop, buffer/audio,
plugin/legacy compatibility and full autonomous production require further work
and execution. Build tools, Qt SDK and initialized pinned submodules remain
unavailable. The complete DAW commission stays active and completion remains
unproven.

Source diagnostics pass across 420 changed/new C++ files and 147 native production
operations. Linked-parameter fixture registration/inventory has thirteen cases /
21 authored scenarios. Validator Python syntax, touched-file whitespace/newlines
and git diff --check pass. No native compiler/build tool or LMMS executable is on
PATH, and all 17 pinned submodules remain uninitialized. These checks do not
compile or execute native links, GUI controls, buffers or agent fixtures.

## Checked native whole-project linked-control persistence

2026-10-14 source continuation; no native C++/Qt compilation, tests, driver,
hardware, GUI, audio/PCM or provider execution occurred. This source work
supersedes the prior runtime-only finding for complete project/checkpoint paths;
other serializer lifecycles remain incomplete.

NativeLinkedControls provides document-bound SaveScope and RestoreScope. The
save scope discovers native project/control owners, including tracks, effects,
mixer channels/routes, headers, rack and connection-owned controller models,
and snapshots their complete linked rings. Generic model serialization writes
reference-bearing value nodes and scope-local native_link_ref values. After
ordinary native serializers finish, the scope verifies that every captured
member serialized, the live ring did not change and every reference marker came
from a registered native model node. It writes an ordered nativeLinkedControls
version-1 graph using only local references. Unsupported/missing serializers or
members refuse publication rather than silently dropping relationships; existing
destinations remain protected by DataFile's serialization diagnostics/writer.

Local references are independent of saved/reused journal IDs. RestoreScope
registers actual model receivers while their fields load, detaching reference-
bearing members before applying individual values. After native controller
finalization, it validates graph version, uniqueness, complete groups and actual
restored node provenance. Rings are installed directly in their saved order,
without copying one member's value over another. This preserves heterogeneous
stored values and native inherited-controller precedence. Controller dependencies
are checked against the proposed rings; a rejection/returned exception restores
the prior edges before native loading reports the error. Successful installation
invalidates affected parameter caches. This is graph rollback, not transactional
restoration of every project/plugin side effect.

Song integrates scopes with complete project save/load. Project clearing unlinks
live project models, including reused headers/mixer controls, before destruction
or reset. Saved future metadata excludes the old nativeLinkedControls container,
so a subsequent save cannot republish stale group definitions. StudioRuntime's
existing checkpoint, undo/redo, reopening and incomplete-load rollback paths use
the native project serializer/loader, without a sidecar control engine. Inspection
and prompt metadata now say checked-native-project and identify coverage limits.
Production operation count stays 147; mutating parameter.link/unlink remain absent.

LinkedProjectPersistenceTest is registered with a 300-second timeout and authors
eleven cases / 20 scenarios. Three native round trips cover cross-track,
mixer-route and header groups with exact stored values. A scoped reader retains
new journal identities while linking the new receivers, not the original models.
A three-member/two-controller ring checks inherited precedence and exact order.
Eight malformed graph rows require native diagnostics without partial rings.
Further cases reject restored controller feedback, preserve groups through native
runtime checkpoint undo/redo/reopening and historical receipt replay, remove
reused-model links on legacy load, roll back an incomplete structured load to its
prior group, refuse unsupported-member publication while keeping file bytes,
replace graph metadata after unlink, and cancel without publication or live group
changes. All remain authored, uncompiled and unrun. The existing live-inspection
fixtures now describe checked project scope rather than universal persistence.

Preset, track/clip/plugin clone and native per-object journal integration remains
unscoped and required. Models whose serializers omit linked fields or use
unregistered temporary documents cannot be preserved by this complete-project
scope; the save rejects them. The versioned extension requires a native rebuild,
and older LMMS versions do not restore its relationships. Actual serializer,
plugin, legacy/GUI, native C++/Qt/MOC/ABI, checkpoint and audio compatibility is
unverified. Discovery and DOM validation materialize/scan native roots/documents
under cooperative checks; no hard deadline or allocation/latency/CPU/RSS bound is
proven. Arbitrary callback self deletion, reentrant project replacement and
concurrent raw mutation remain outside guarantees. Real provider musical judgment
and full autonomous production remain outstanding. Native build tools, Qt SDK
and initialized pinned submodules remain unavailable; the full DAW commission
stays active and completion remains unproven.

Source diagnostics pass across 423 changed/new C++ files and 147 native production
operations. Linked-project fixture registration/inventory has eleven cases /
20 authored scenarios. Validator Python syntax, touched-file whitespace/newlines
and git diff --check pass. No native compiler/build tool or LMMS executable is on
PATH, and all 17 pinned submodules remain uninitialized. These checks do not
compile or execute native serialization, restoration, checkpoints or tests.


## Checked native linked groups in track presets and clone settings

2026-10-14 source continuation; no native compilation, fixture execution,
GUI/plugin/audio/hardware or provider execution occurred. This extends the
preceding whole-project implementation to closed serialized track settings.

NativeLinkedControls now accepts explicitly selected model roots. Track scopes
include native instrument/sample settings, effect chains and connection-owned
MIDI controller controls. Presets exclude clip-owned controls. Shared rack
controllers are not expanded into a portable track graph; an unrelated internal
rack group cannot prevent a track preset save. Any captured ring still requires
every member to serialize, including members outside the selected roots.
Cross-track/mixer/rack rings and omitted plugin controls therefore refuse
publication through the native DataFile diagnostic rather than lose links.
Shared non-MIDI controller bindings keep their existing native rack ordinal
references; these changes do not make those bindings portable across projects.

Track cloning scopes its native settings document and validates complete groups
before creating a replacement. Restoration maps local references to the copied
receivers under KeepIdentityScope; original models and identities remain
independent. Resident clips still use the existing native direct-copy path.
Capture includes the source track's clip-owned groups, so groups omitted by the
settings document refuse track cloning instead of silently disappearing. This
is an explicit remaining limitation, not completed clip-copy persistence.

Track.savePreset centralizes portable mute/solo normalization in both legacy
attributes and reference-bearing model nodes. Modern presets write an empty
version-1 graph when there are no groups, distinguishing intentional absence
from legacy files. Loading a modern preset clears existing internal settings
rings before applying the declared groups; clip-owned rings remain intact.
Legacy unmarked files retain prior native group behavior. Instrument replacement
restores destination mute/solo and mixer channel before installing the incoming
rings, avoiding value propagation through the newly installed groups. Track MIDI
port preservation removes only the direct track port node, preserving embedded
connection-owned MIDI controller ports that the recursive removal could delete.

LinkedTrackPersistenceTest is registered with a 300-second timeout and authors
ten cases / 12 scenarios. Coverage includes independent clone groups/IDs/values,
cross-track refusal before creation, native and structured preset entry points,
destination identity/mute/mixer/clip preservation, modern empty versus legacy
graph behavior, failed publication retaining destination bytes, clip group
preservation on preset load and refusal on track clone, embedded MIDI controller
versus destination track port settings, unrelated shared rack groups, and native
runtime undo/redo plus malformed preset checkpoint rollback. Every scenario is
authored, uncompiled and unrun.

Standalone clip copies, plugin-only copies and native per-object journals remain
unscoped. Structured parameter.link/unlink mutations remain absent pending the
remaining lifecycle work. Missing serializers, unavailable plugins and temporary
unregistered documents may reject portable groups. Native/GUI failed loads can
leave partial settings changes with diagnostics; the structured runtime retains
its existing whole-project checkpoint rollback. Discovery/ring/DOM work allocates
and scans native state with cooperative checks; no hard deadline or resource
bound is established. Arbitrary callbacks, self deletion, reentrant project
replacement and concurrent raw mutation remain outside guarantees. Native
C++/Qt/MOC/ABI, legacy/GUI/plugin, audio, CPU/RSS and real provider production
acceptance remain unverified. The complete DAW goal stays active; completion is
unproven.

Source diagnostics pass across 424 changed/new C++ files and 147 native production
operations. Linked-track fixture registration/inventory has ten cases / 12
scenarios. Validator Python syntax, touched-file whitespace/newlines and git diff
--check pass. Native build tools and an LMMS executable remain absent from PATH,
and all 17 pinned submodules remain uninitialized. Source diagnostics do not
compile or execute native persistence or the authored fixtures.


## Checked native clip control persistence and copying

2026-10-14 source continuation; no native compiler/Qt/MOC, tests, GUI/plugin,
audio/hardware or provider execution occurred. This supersedes the preceding
unscoped finding for tracked clip control copying. The complete DAW commission
remains active and completion remains unproven.

Source inspection found that MIDI, sample, automation and pattern-reference clip
serializers wrote only evaluated mute scalars. Native solo had no clip parent in
the ordinary constructor and was not serialized. These omissions prevented
whole-project linked-control graphs from including those fields. All four native
serializers now save/load mute and solo through AutomatableModel. Solo belongs to
its clip for discovery. Stored values, generic model references, native controller
connections and ordered rings now participate in project/checkpoint restoration,
including groups spanning clips and track settings. The automation clip retains
its legacy mute key; other clip types retain muted. Legacy scalar attributes are
also written, while control/connection nodes carry metadata=1 so older MIDI-clip
readers do not interpret them as notes. New native MIDI parsing also explicitly
excludes these control nodes. No older executable was run to verify compatibility.

Tracked native direct copies capture a small clipcontrols document before any
new clip is constructed. SaveScope discovers owned controls and embedded MIDI
controller models and requires every captured ring member to serialize. Closed
mute/solo and serialized owned-MIDI control groups can be copied. External or
omitted members refuse copying. Native constructors still retain resident sample
PCM, independent notes/detuning and curve nodes/targets. After construction,
KeepIdentityScope and RestoreScope load only copied controls and install exact
rings onto the new receivers, keeping stored values and fresh identities. They
finish before a pattern replacement retires its old cell. A failed later control
restore discards the newly owned result. Each source clip is preflighted before
whole-track/column construction, and track settings exclude separately copied
clip groups. Cross-clip/track groups persist in complete projects but portable
track/clip copies still refuse groups beyond each copied scope.

The four tracked clone entries share this native checked path. Legacy same-track
PatternStore clone append behavior is retained; explicit copyTo still requires
an existing replacement cell for a PatternStore destination. Native MIDI GUI
clone callers handle a refusal without dereferencing a null result and restore
their journal state; a failed second split clone discards the first. Inline curves
without tracks retain their direct native AutomationClip constructor path,
including Note detuning copies during DSP, rather than receiving GUI-owner
thread restrictions. Their linked-control copying remains unscoped.

Native XML copyStateTo now scopes the full state document, refuses incomplete
source groups before changing the destination, keeps destination identity and
position and clears replaced control links before loading individual values.
Replacing a control binding deletes its previous connection, including owned
MIDI controller/port state. The XML path retains existing asset reload behavior;
it is not the resident-PCM direct-copy path. A later XML/native load error can
leave partial changes; structured runtime operations retain whole-project
checkpoint rollback. Standalone GUI clipboard serialization, plugin-only copies
and per-object journals remain unscoped, and structured parameter.link/unlink
operations remain absent pending the remaining lifecycle work.

LinkedClipPersistenceTest is registered with a 300-second timeout and authors
eleven cases / 30 scenarios. Eight direct copy/clone rows cover four native clip
types, independent identities/rings, heterogeneous stored values and retained
notes/PCM/curve content. Four project serializer rows check reference restoration,
legacy scalar fallbacks/metadata and absence of spurious MIDI notes. Further
coverage includes cross-clip/track project groups, five native/structured boundary
refusal entries, omitted owned fields before track/clip construction, exact
pattern replacement with an externally linked old cell, three song-track/pattern
row/column clone entries, independent owned MIDI controller copying and retirement,
four state-copy rows preserving destination identity/position and clearing groups,
inline Note detuning independence, and real StudioRuntime undo/redo/refused-copy
checkpoint rollback. Every scenario remains authored, uncompiled and unrun.

Shared non-MIDI controller bindings retain native rack ordinal references and do
not become portable across projects. Raw copy constructors, untracked inline
curves, unavailable plugin state and unregistered/omitting serializers are not
covered by this checked tracked-control scope. Synchronous deletion/callbacks
still require native model ownership; arbitrary self deletion, reentrant project
replacement and concurrent mutation remain outside guarantees. Control documents
and owner/ring discovery allocate and scan native state under cooperative checks;
no hard deadline, native latency/allocation/CPU/RSS proof is established. Native
build, GUI/legacy/plugin compatibility, actual PCM/render comparisons, hardware
and real provider musical judgment/full autonomous production remain outstanding.

Source diagnostics pass across 427 changed/new C++ files and 147 native production
operations. New fixture registration/inventory has eleven cases / 30 authored
scenarios. Validator Python syntax, touched-file whitespace/newlines and git diff
--check pass. Native build tools and an LMMS executable remain absent from PATH,
and all 17 pinned submodules remain uninitialized. These checks do not compile
or execute native controls, copy constructors, graph persistence or fixtures.


## Native clipboard and drag selection transfer with linked controls

2026-10-14 source continuation; no native compilation, Qt/MOC fixtures, GUI,
OS clipboard/drag, plugin/legacy, audio/hardware or provider execution occurred.
The preceding turn changed authoritative clip serializer/copy source state; this
continuation extends checked persistence to complete selected clip payloads.
Per-object native journals remain a separate incomplete lifecycle. The complete
DAW commission stays active and completion remains unproven.

Inspection found that ClipView serialized each selected clip without a shared
linked-control scope, while TrackContentWidget restored each clip independently.
NativeClipTransfer now owns selection serialization/loading in core. Both
headless callers and GUI adapters use the same native clip serializers, project
models and graph implementation. Save validates a selected anchor, common native
container, live unique source clips, track/state types, owner thread and recording
state. It discovers selected clip/owned-MIDI controls and writes ordinary clip
state plus the existing placement metadata into one DataFile. One SaveScope
requires complete selected ring coverage. Groups spanning selected clips can be
preserved; members outside selection or omitted by serializers refuse publication
through the DataFile diagnostic. Unresolved controllers and non-MIDI controllers
outside the current rack also refuse saving rather than producing an empty binding.

Loading checks document type, clip container/count, native receiver track/state
compatibility, destination ownership/recording and target positions before
construction. A KeepIdentityScope retains new identities; one RestoreScope and
native resource reader cover all imported clips, allowing document-local refs to
bind groups across receivers. Dummy/unresolved or unregistered bindings refuse
loading. Complete graph verification occurs before results publish to the caller.
Prepared clips stay under unique ownership. A native roster cleanup also removes
clips attached by a factory that throws before returning its pointer. Cleanup
avoids allocating roster copies during unwinding. Automatic journal entries are
disabled during loading and restored after cleanup, avoiding incidental history
changes from temporary setters. Existing caller-managed history remains separate.

ClipView now delegates copy/drag data construction to core. Failed documents are
not sent to the clipboard or drag source. Copy reports success to cut, so failed
serialization preserves originals. Paste delegates construction/group restoration
to core and keeps the old GUI selection on refusal. GUI layout/type checks rerun
at paste even after drag admission; checked metadata and widened track/position
arithmetic reject invalid or overflowing positions before index/conversion use.
The existing same-bar decision and quantized placement rules remain. ClipView's
copy return contract changes from void to bool and requires native rebuilding.

NativeClipTransferTest is registered with a 300-second timeout and authors eleven
cases / 38 scenarios. Eight native/reparsed-byte selection rows cover all four
clip types, cross-clip rings, fresh identities, positions and retained native
notes/decoded embedded samples. Three publication refusals cover external and
omitted members plus an unregistered rack controller, preserving destination file
bytes and source groups. Ten rejected-document rows cover malformed graph and
container/reference/state/type/count/position data and unresolved controllers,
requiring cleanup of every prepared clip. Four before/after standard/nonstandard
factory failures verify attached-clip cleanup and native journal flag/history
preservation. Further cases cover partial construction cancellation without event
dispatch, legacy unmarked data, source/destination sample recording, two foreign
thread entries, unrelated shared rack groups, native runtime checkpoint reopening
and six invalid selections. All are authored, uncompiled and unrun. These are
core tests with actual native models; they do not exercise OS clipboard or GUI
widgets, native hardware, provider music evaluation or complete production.

This core API adds no production operation/declaration; current clip.clone remains
its existing single-clip structured operation. Broader agent selection exposure
and per-object journal topology remain required work. Inline note curves and
plugin-only copy lifecycles remain unscoped. Shared non-MIDI controller bindings
retain rack ordinal meanings and do not become portable across projects.
Automation targets retain existing native ID resolution rather than selection
remapping. Native XML asset reload and legacy PatternStore append semantics are
retained; the caller retires the clicked old cell after successful paste.
Prepared-clip cleanup does not roll back all native playback/plugin/global
automation or arbitrary callback side effects. Existing GUI per-object undo does
not yet preserve linked topology. The structured runtime retains its independent
whole-project checkpoint recovery. Native model ownership remains required;
arbitrary self deletion/reentrant replacement/concurrent mutation and throwing
destructors remain outside guarantees. DOM, selection, source discovery, asset
loading and cleanup can allocate/scan native state with cooperative checks; no
hard deadline or allocation/latency/CPU/RSS bound is proven. Native executable,
GUI/plugin/legacy, OS clipboard/drag and actual rendering/provider acceptance
remain unverified.

Source diagnostics pass across 432 changed/new C++ files and 147 native production
operations. Fixture registration/inventory has eleven cases / 38 authored
scenarios. Validator Python syntax, touched-file whitespace/newlines and git diff
--check pass. Native build tools and an LMMS executable remain absent from PATH,
and all 17 pinned submodules remain uninitialized. Source checks do not compile
or execute native selection transfer or its fixtures.


## Structured native selection copying and agent target continuity

2026-10-14 source continuation; no native C++/Qt/MOC compilation, fixture,
GUI/plugin/legacy, actual audio/hardware or real provider execution occurred.
This extends the preceding native selection transfer implementation to agent
execution. The complete headless DAW commission remains active and completion
is unproven.

clip.selection.copy exposes a strict items array containing 1..4096 distinct
native source clips and their destinations. Each item has source container/track/
clip plus optional targetContainer/targetTrack. Arrangement destinations require
position; pattern destinations require an existing targetClip and replace:true,
without position. Type, exact integer/index/container, nested field, geometry,
recording, source uniqueness, destination-cell uniqueness and owned pattern row
checks run before construction. Selected sources cannot be replaced. Items can
span song and pattern containers. Receipts preserve input order and report actual
final native destination indices and positions. The operation is mutating and
uses the existing StudioRuntime checkpoint/history/rollback/receipt pipeline.
Catalog count is now 148 native operations; ten supplemental agent operations
remain within the 128-declaration budget through native catalog routing.

NativeClipTransfer adds direct copy alongside its existing clipboard save/load.
It captures only copied controls into a shared native graph, then constructs
content using the same native constructors as individual clip copying. Clip's
constructor dispatch is factored into a private shared method. Resident sample
buffers and playback settings, independent notes/detuning and curve nodes,
existing automation target pointers and native pattern-reference meanings remain
native. No asset decoding/reload is introduced into direct copying, and accepted
resident buffers survive removal of the original file. Generic clipboard loading
retains its XML asset behavior. Copied controls and controller bindings restore
under kept identity onto the new receivers; ordered cross-clip groups remain
independent of original controls. Omitted/outside members refuse copying.

Native targets may specify explicit pattern replacement cells. The loader tracks
weak receiver-cell identities while preparing all copies, validates the complete
control graph and cancellation before retiring originals, then swaps each copy
into its owned column. Pattern rows retain their other cells. Earlier failures
clean prepared copies and preserve original cells. Automatic journal suppression
and roster cleanup from the prior turn remain. Direct native failures after
commit begins can still leave partial changes; structured runtime checkpoint
rollback supplies project recovery. Callback, playback/plugin/global side effects
are not fully transactional and arbitrary self deletion/reentry/concurrency or
throwing cleanup/destructors remain outside guarantees.

Review also found that a returned clip-added callback exception during Clip's
base constructor could leave a half-constructed pointer in the track roster.
Both ordinary and copy base constructors now use scoped removal until successful
construction. The new fixture throws on the second registration notification,
requiring no registered partial clip and cleanup of the first prepared copy.
This is source correction, not proof that arbitrary Qt callback exceptions are
safe; native Qt/platform execution remains required.

Every nested source track/clip and destination track/cell gets a separate weak
identity in StudioCapabilities.selection. Existing queued objects that retire or
shift indices cause the runtime to skip the remaining inference batch, for both
individual declarations and studio_execute catalog calls. Unresolved future
selectors keep existing behavior. Agent instructions explain the new operation,
conditional destination requirements, complete control groups, receipt order and
retained automation targets. Musical planning and persistent job/tool records
remain in the same native runtime; no external GUI automation was added.

StudioClipSelectionCopyTest is registered with a 300-second timeout and authors
twelve cases / 47 scenarios. Four native type rows verify retained content,
independent groups and identities. Joint pattern replacements, mixed containers
with owned MIDI bindings, twenty invalid/boundary item configurations, resident
audio after original file removal, actual runtime undo/redo/reopening/cached
receipts/refusal rollback, four nested identity changes, four planned Chat/
Responses direct/catalog copy routes, eight stale-batch provider routes, partial
pattern preparation cancellation/event exclusion, returned constructor
notification failure and schema/declaration limits are authored. The provider
routes are scripted native harness contracts, not real language-model musical
planning/evaluation or production acceptance. Every scenario is uncompiled/unrun.

Native scope limits remain: inline curves and per-object journals lack complete
linked persistence, shared rack references retain ordinal semantics, automation
targets retain native source bindings instead of selection remapping, unsupported
serializers/controls refuse portable copying, and arbitrary callbacks or one huge
native copy cannot be preempted by cooperative checks. DOM/owner/group/note/cleanup
work may allocate and scan without a hard deadline or native CPU/RSS proof.
Actual compiler/ABI, GUI/legacy/plugin, rendered PCM, driver/hardware, real provider
reasoning/listening/revision and full autonomous DAW acceptance remain missing.

Source diagnostics pass across 433 changed/new C++ files and 148 native production
operations. New fixture registration/inventory has twelve cases / 47 authored
scenarios. Validator Python syntax, touched-file whitespace/newlines and git diff
--check pass. Native compiler/build tools and an LMMS executable remain absent
from PATH, and all 17 pinned submodules remain uninitialized. These checks do
not compile or execute native selection copying, history or agent fixtures.


## Native parameter link and unlink mutations in the agent runtime

2026-10-14 source continuation; no native C++/Qt/MOC compilation, fixture,
GUI/plugin/legacy, actual audio/hardware or provider execution occurred. This
supersedes prior absence of structured link/unlink mutations for the native
complete-project transaction path. Per-object native journal topology remains
incomplete. The full DAW commission remains active and completion unproven.

parameter.link requires distinct current parameter and peer handles and joins
existing native control rings through AutomatableModel.linkToModel. One native
parameter discovery/plugin refresh happens before retaining either receiver,
avoiding retirement during a second refresh. A weak membership snapshot tracks
actual QObject lifetimes, not just addresses. Every merged member must be
currently discovered, writable, support native automation/controller input,
use linear/log scaling and have a finite stored value. Checks precede native
value copying and rerun after callbacks; a replacement allocated at an earlier
model's address cannot inherit its discovered identity. Native feedback and
receiver-lifetime checks remain authoritative. Root and peer are separate weak
queued selections, so retirement invalidates a planned call in either protocol.

Native joining copies peer's stored raw value to the selected model and its old
peers, fitting each to its own range/step. This retains heterogeneous units and
native direct/inherited controller precedence rather than inventing normalized
mapping. Already joined calls are idempotent with changed:false. parameter.unlink
detaches one writable discovered control, preserving stored values and bindings
while the remaining ring stays linked; already detached calls are idempotent.
Both use native cache invalidation without advancing musical clocks. parameter.set
now checks writability across its linked group before ordinary or special native
program selection, preventing writes through a disabled/output peer. Cooperative
file/resource scopes cover link/unlink/set with events suspended during mutation.

Both new operations are mutating entries in the native capability catalog and
use the existing StudioRuntime project backup, post-save, rollback, undo/redo,
reopening and receipt pipeline. Complete serializer coverage remains mandatory;
discovery alone is not persistence proof. A post-save with an omitted linked
member rejects publication and restores the native project. Earlier callback
feedback/admission failure can retain copied values/controller/ring effects in
direct native calls; the runtime supplies checkpoint rollback and invalidates
old handles. A failed backup of an already unsupported group still prevents
runtime mutation; direct native recovery remains separate. Per-object journal
persistence is not claimed.

Agent instructions document handles, raw-unit fitting, controller precedence,
detachment, writable peers, checkpoint failure and handle rediscovery. Tool
selection reserves parameter_link and parameter_unlink alongside discovery and
audio declarations; catalog routing still exposes every native operation. The
catalog is now 150 native plus ten supplemental agent operations, bounded to
128 declarations without reducing native availability.

StudioParameterLinkTest is registered with a 300-second timeout and authors
fifteen cases / 36 scenarios. Coverage includes merging/splitting existing groups,
idempotence and retained values, two immediate inherited MIDI buffer scales with
unchanged controller periods, exact raw-unit fitting of heterogeneous member
ranges, three direct/indirect/group feedback refusals before signals/value copying,
six input/readonly/scale/external-member admissions, readonly linked-peer write
refusal, ten malformed/historical handle cases, callback feedback rollback,
callback policy change refusal and address reuse, native history/reopening/cached
receipts, omitted serializer post-save rollback, two receiver retirements, four
planned Chat/Responses direct/catalog link/unlink paths, cancellation/event
exclusion and declaration limits. Generic live models are used only where testing
live admission; persistence cases use actual native track/controller serializers.
Every case is authored, uncompiled and unrun. Scripted provider paths verify
routing/planning records, not real provider musical decisions or production.

Weak provenance does not pin models or make arbitrary self deletion, reentrant
project replacement, raw concurrent mutation, blocked callbacks or throwing
cleanup safe. Native feedback checks materialize/traverse groups/controller
relationships, and discovery scans current roots; bounded receipts do not imply
constant work, hard deadlines or native allocation/latency/CPU/RSS evidence.
Inline/plugin-only copies and per-object journals remain incomplete. Native
compiler/ABI, GUI/legacy/plugin, actual rendering/driver/hardware and real provider
reasoning/listening/revision/full production acceptance remain unverified.

Source diagnostics pass across 434 changed/new C++ files and 150 native production
operations. Fixture registration/inventory has fifteen cases / 36 authored
scenarios. Validator Python syntax, touched-file whitespace/newlines and git diff
--check pass. Native build tools and an LMMS executable remain absent from PATH,
and all 17 pinned submodules remain uninitialized. Source checks do not compile
or execute native linking, project recovery or agent fixtures.


## Native linked value propagation under callback retirement

2026-10-14 source continuation; no native C++/Qt/MOC compilation, fixture,
GUI/plugin/legacy, actual audio/hardware or provider execution occurred. The
preceding turn changed authoritative agent link/set source; this continuation
addresses the native setter those operations and automation use. The complete
DAW commission remains active and completion remains unproven.

AutomatableModel.setValue previously advanced its raw cursor through a linked
ring after setValueInternal emitted synchronous callbacks. Retiring the current
peer left a dangling cursor; detaching it could leave a self-loop that never
returned to the initiator; retiring the receiver made later accesses invalid.
The setter now uses a weak receiver across checkpoint/initial value notification,
returns when it retires, and avoids constructing a visited map when no linked
peers remain. Linked traversal keeps weak visited identities and captures its
next receiver before each notification. After callbacks it never dereferences
the previous sender. A separate uint64 topology generation increments on joins,
unlinks and direct native graph application/rollback. When topology changes,
traversal resumes at current live unvisited peers. Another model at a retired
address has a fresh weak lifetime and can receive the remaining write.

This retains traversal of the current native ring: newly joined groups remain
eligible, detached future controls are omitted, and remaining controls are not
lost when the current peer retires or detaches. Current native nested setter
order, raw-unit fitting and signal counts remain. Exceptions still propagate;
earlier values/signals are not reversed in direct calls. The topology generation
does not advance musical time, invalidate unrelated interpolation or change
buffer generation policy. NativeLinkedControls marks direct edge installation
and rollback too, so a graph restored during a notification cannot leave the
outer setter on an old ring. StudioCapabilities.parameter.set guards the receiver
before returning readback. A retired receiver refuses success and the existing
StudioRuntime checkpoint rollback restores the native project.

LinkedValueOwnershipTest is registered with a 300-second timeout and authors ten
cases / 17 scenarios. Manual/automated fitting and signal counts, retirement of
current/next/last/initiating models, current/future detachment, an initial callback
joining a group, a mid-loop merge preserving nested values, reused visited
addresses, native scoped graph restore during notification, standard/nonstandard
returned callback exceptions followed by another write, native/runtime receiver
retirement and receipt recovery, and 1050-peer exactly-once unchanged-ring writes
are authored. SongAutomationOwnershipTest adds an actual native song automation
scenario whose linked peer deletes its own track during propagation; remaining
models must receive the value. That fixture now has eleven cases / 16 scenarios.
All remain authored, uncompiled and unrun; counts and source checks are not
native execution, timing or production evidence.

Native model owner synchronization remains required. Weak identities do not pin
objects or guard arbitrary parent/plugin deletion, all other setters/callers,
journal serializers, reentrant project replacement, raw concurrent mutation,
throwing destruction or blocked callbacks. The narrow setValue return path
avoids receiver/peer accesses after their own notification; this is not a claim
that arbitrary C++ or Qt callback self deletion is universally safe. Single
unlinked writes retain the no-visited-map path, while linked writes allocate weak
entries and unchanged rings have linear cursor traversal. Topology changes can
rescan groups; unrelated topology activity can also force rescans. Repeated
callback mutation can be unbounded and the generation has a finite uint64 wrap
limit. Native allocation/latency/CPU/RSS and audio-thread suitability remain
unverified. Per-object journal and inline/plugin-only linked copy persistence
remain incomplete; actual GUI/legacy/plugin, compiler/ABI, PCM/hardware and real
provider reasoning/listening/revision/full production acceptance remain missing.

Source diagnostics pass across 435 changed/new C++ files and 150 native production
operations. New fixture registration/inventory has ten cases / 17 scenarios,
and the existing automation fixture has eleven / 16. Validator Python syntax,
touched-file whitespace/newlines and git diff --check pass. Native build tools
and an LMMS executable remain absent from PATH, and all 17 pinned submodules
remain uninitialized. Source checks do not compile or execute native setter
lifetimes, automation propagation or project recovery.


## Native setter caller lifetime and journalling recovery

2026-10-14 source continuation; no native C++/Qt/MOC compilation, fixtures,
GUI/plugin/legacy, actual audio/hardware or provider execution occurred. The full
agent-native DAW commission remains active and its completion remains unproven.

The preceding setter change guarded linked propagation, but callers could still
read a retired receiver after an emitting call returned. AutomatableModel now
uses weak receiver checks in setInitValue after setValue, and in setRange after
setStep and the fitted value write. Initialization's scoped journalling restore
uses the weak receiver, so it does not access a retired model during return or
exception unwinding. A surviving model retains its previous journalling flag.

Generic XML loading guards legacy curve lifetime across load/value callbacks and
the receiver between node value assignment and scale assignment. Replacement
ControllerConnection state is loaded before native model attachment. A unique
owner cleans preparation failure; ownership is released to the native model
before attachment emits. The previous weak binding is deleted by a scope guard,
including after returned attachment exceptions, and receiver lifetime is checked
after that cleanup because QObject destruction can itself notify callbacks.
This corrects prior replacement leakage and avoids accessing the new connection
through a retired model. Preparation changes attachment notification timing and
requires native compatibility checks; it assumes exclusive native model ownership
of the previous binding. Direct failures can leave the new binding attached.

TempoSyncKnobModel.calculateTempoSyncTime replaces manual journalling restoration
with a weak scoped guard. Returned exceptions restore the surviving receiver's
prior flag. Checks after value and mode signals stop later receiver accesses;
the original arithmetic and mode-notification/bookkeeping order remain. Tempo
loading checks after the base and meter loads, and setScale checks after tempo
recalculation before emitting scaleChanged. MeterModel reset checks after both
value setters and numerator curve clearing; loading checks between numerator and
denominator. StudioCapabilities.parameter.sync checks the tempo receiver after
each meter write and native synchronization before result construction. Receiver
retirement refuses a success receipt; the existing StudioRuntime checkpoint
rollback restores serialized native state and invalidates former model handles.

ModelSetterOwnershipTest is registered with a 300-second timeout and authors
twelve cases / 27 scenarios: initialization retirement, standard/nonstandard
exceptions with enabled/disabled journalling and a later successful initialization,
range property/value retirement, node load retirement before scale assignment,
prepared binding attachment retirement and old-binding destruction retirement,
attachment failure cleanup followed by another load, meter reset/load numerator
retirement, tempo value/mode/scale retirement, standard/nonstandard tempo callback
exceptions and journalling recovery, tempo base/custom-meter load retirement,
native/runtime numerator/denominator/synchronization retirement in a real LFO
controller, and ordinary native fitting, tempo arithmetic, signals and journal
state. All remain authored, uncompiled and unrun. They establish no executed
memory-safety, recovery, musical, timing or production result.

These guards cover accesses after returned callbacks in the named callers. Native
model ownership/synchronization remains required. Weak references do not pin
models, repair arbitrary callee/parent/plugin deletion or make all C++/Qt self
deletion safe. Throwing destructors/cleanup, concurrent raw mutation and unrelated
project/plugin loaders remain outside this continuation. Direct failures can
retain partial values, modes, meter, scale, journal and callback effects; runtime
recovery depends on full native serialization. Native compiler/ABI, GUI/legacy/
plugin, PCM/hardware, latency/CPU/RSS, provider reasoning/listening/revision and
full autonomous production acceptance remain missing. Per-object linked journals
and inline/plugin-only copy persistence remain incomplete.

Source diagnostics pass across 437 changed/new C++ files and 150 native production
operations; the new fixture inventory is twelve cases / 27 scenarios. Validator
Python syntax, touched-file whitespace/newlines and git diff --check pass. Native
build tools and an LMMS executable remain absent from PATH, and all 17 pinned
submodules remain uninitialized. Source checks do not compile or execute the
native setters, tempo synchronization, controller loading or project recovery.


## Native compound linked value journal

2026-10-14 source continuation; no native C++/Qt/MOC compilation, fixture,
GUI/plugin/legacy, actual audio/hardware or provider execution occurred. The
preceding turn made setter-source progress. The complete DAW commission remains
active, and completion remains unproven.

Inspection confirmed that native per-object value checkpoints serialized only the
initiating AutomatableModel. Restoring that scalar called the ordinary linked
setter, overwriting distinct peer values. For example, a receiver with maximum
60 can be linked to peers storing 70 and 90. After a value edit to 20, undo must
restore 60/70/90; broadcasting the saved receiver value instead loses peer state.
StudioRuntime full-project checkpoints already follow a separate path and do not
repair native ProjectJournal's object-preserving history.

AutomatableModel now overrides saveJournalState/restoreJournalState. A linked
checkpoint stores the complete ordered ring with each model's full native ID,
lifetime stamp, raw stored float and fitting signature (minimum, maximum, step,
scale and strict-step policy). Float strings use max_digits10. An atomic uint64
sequence assigns each model lifetime a nonzero stamp; address and native ID reuse
cannot impersonate a retired peer. Exhaustion throws before wrap at UINT64_MAX.
This adds native model layout and construction cost and requires rebuilding native
components and native ABI/platform checks. No project XML format is changed.

Restoration acquires native model ownership and suppresses event polling. It
validates format, receiver identity, complete ordered membership, all native ID
registrations/lifetimes, finite fitted values and unchanged fitting signatures
before mutation. Missing, duplicated, reordered, joined, detached, retired or
reidentified peers refuse restoration. A legacy scalar checkpoint refuses to
restore into a currently linked receiver. Unlinked models keep the existing
serialization path. MixerControlModel retains its own specialized compound solo
journal overrides.

The restore prepares all weak entries before changing state, checks cooperative
cancellation before commit, and publishes the complete raw-value vector before
any changed-peer dataChanged signal. Ordinary setValue propagation would destroy
the distinct saved peer values, so this path writes native fields directly,
preserves old values for interpolation and invalidates same-period buffers.
Membership/lifetime/fitting and final values are rechecked after notifications;
topology changes can trigger an additional ordered-group scan. A per-model flag
refuses nested restoration of the same ring. A weak scoped rollback restores
surviving raw values, prior interpolation values and changed flags on returned
exceptions or rejected callbacks, and clears restoration flags. ProjectJournal's
existing logic preserves history stacks and journal state on refusal/exception;
a later retry can succeed after a throwing notification is disconnected.

This rollback does not reverse emitted callbacks, plugin or derived/external
effects, changed topology/fitting, retirement or other native state, and emits no
compensating notifications. Weak references do not pin objects. Native owner
synchronization is required; arbitrary callee/parent/plugin self deletion and
concurrent raw mutation remain unsupported. This is value history, not native
link/unlink topology undo or full model/plugin-state restoration. Controller
configuration, automation, initial values and tempo modes remain current. Native
per-object history is in memory; durable StudioRuntime history remains separate.

LinkedValueJournalTest is registered with a 300-second timeout and authors thirteen
cases / 35 scenarios: distinct per-peer values with undo/redo and stable native
objects/IDs, complete vector visibility during signals, same-period interpolation
refresh, seven changed membership/order/fitting refusals, thirteen invalid XML
states, reused peer address and native ID with a different lifetime, standard and
nonstandard notification exceptions followed by successful retry, receiver/peer
retirement plus detachment/value mutation with surviving-value rollback, nested
direct restoration refusal, cancellation without timer dispatch, exact float
precision with retained owned MIDI binding, scalar history before/after joining,
real instrument-track owners and a 1050-peer native journal. All are authored,
uncompiled and unrun. Review also corrected two typed initValue calls in the
previous ModelSetterOwnershipTest fixture; that fixture remains unrun.

Snapshot XML and weak entries require memory proportional to ring size. Normal
snapshot/restore traversal is linear; topology activity during notification may
cause repeated scans. Native allocation, atomic cost, hard deadlines, latency,
CPU/RSS and audio-thread suitability remain unmeasured. Per-object topology undo,
inline/plugin-only linked copies, actual GUI/legacy/plugin, C++/Qt/MOC/ABI,
PCM/hardware and real provider reasoning/listening/revision/full production
acceptance remain incomplete or unverified.

Source diagnostics pass across 438 changed/new C++ files and 150 native production
operations. The new fixture inventory is thirteen cases / 35 scenarios. Validator
Python syntax, report JSON, fixture whitespace/newlines and git diff --check pass.
Native build tools and an LMMS executable remain absent from PATH, and all 17
pinned submodules remain uninitialized. Source checks do not compile or execute
native compound value journals or establish musical/full-system acceptance.


## Native user-facing link and unlink history

2026-10-14 source continuation; the preceding turn made native value-journal source
progress. No native C++/Qt/MOC compilation, fixtures, GUI/plugin/legacy, actual
audio/hardware or provider execution occurred. The complete agent-native DAW
commission remains active and completion remains unproven.

Inspection confirmed that user-facing unlink bypassed native history and link
could record only its initial scalar copy. A merge needs both old groups in one
checkpoint, and undoing detachment needs peers no longer reachable from the
initiating model. Native value checkpoints alone cannot represent these partitions.

NativeControlLinkJournal is a core native entry point for user-facing edits.
StudioCapabilities parameter.link/unlink and native GUI drag/drop/unlink actions
delegate to it. AutomatableModel.linkToModel/unlink remain primitives for internal
plugin binding, constructor/destructor cleanup and XML restoration. A changed edit
with global and receiver journalling enabled captures both complete old groups,
suppresses incidental scalar checkpoints during the primitive mutation, and
publishes one prepared checkpoint after obtaining the complete current state.
Idempotent edits and disabled journalling do not add history. Failed initial edits
or publication may retain direct native/callback effects without a checkpoint;
the entry point reports failure and StudioRuntime's full-project transaction
continues to supply structured rollback. This is not an all-effects transaction.

The transient nativeControlLinkJournal format has desired and expected sections.
Each is a permutation of outgoing edges over the complete fixed model set, with
full native IDs, the existing nonreusing lifetime stamps, max_digits10 raw values
and fitting signatures. Parsing verifies complete disjoint rings and matching
affected lifetimes. Expected edges must match current native topology, so an
untracked join, split, reorder, retirement or fitting change refuses replay rather
than silently replacing another edit. A counterpart captures current values and
edges over the original fixed set, including detached peers, and expects the
desired state after the impending replay. This gives redo the same complete scope.

ProjectJournal dispatches this native format through its counterpart/restore
methods, while ordinary object value journals and specialized mixer solo journals
retain their existing dispatch. addPreparedJournalCheckPoint validates native
registration, nonempty state and serialization diagnostics, appends one checkpoint,
clears redo and retains the existing 100-entry limit. A const DataFile.content
accessor supports inspection without changing the prepared DOM. The project XML
graph format and 150 structured production operations are unchanged.

Replay acquires native ownership, suppresses event polling, validates and allocates
all weak state before mutation, checks cooperative cancellation, and marks the
affected controls as restoring. It uses NativeLinkedControls.applyGroups to
install native ordered rings with controller-feedback rejection. Complete raw
values are installed before changed-peer signals, with interpolation predecessors
and same-period buffers refreshed. Each notification is checked for invalidated
identity/topology/fitting/value state; native feedback is checked again afterward.
Native journalling remains suspended through replay, and prior journal flags are
restored on return or exception. Nested unrelated edits made by callbacks do not
receive independent native checkpoints inside this compound edit.

A weak rollback detaches affected surviving controls from callback-created groups,
restores their prior raw/interpolation/changed state, and reconnects prior rings
while skipping retired members when controller bindings are unchanged. If any
binding/controller changed during the failed replay, affected controls remain
detached rather than reconstructing a possible feedback ring. Retired objects are
not recreated. Callback, derived/plugin/external effects and changed bindings are
not reversed, and rollback emits no compensating notifications. Refusal/exception
retains the native history stacks; repair or another supported recovery path may
be necessary before a retry can succeed. Weak identities do not pin objects;
arbitrary parent/plugin deletion, concurrent raw mutation, callee reentry and
throwing cleanup remain unsupported. Native owner synchronization is required.

ControlLinkJournalTest is registered with a 300-second timeout and authors thirteen
cases / 29 scenarios: merging two ordered groups with distinct values, two/three
control detachment undo/redo, interleaved topology/value history, idempotence and
disabled journalling, four untracked membership/fitting/lifetime refusals, feedback
created by a new controller binding before recombination, standard/nonstandard
notification exceptions followed by retry, peer/receiver retirement and external
join rollback, changed-binding failure leaving safe detached controls, ten malformed
snapshot cases, counterpart roundtrip and cancellation without timer dispatch,
structured native edits retaining instrument-track owners, 1050-peer detachment
history and destructor cleanup without history. All remain authored, uncompiled
and unrun; no native memory-safety, compatibility, audio or musical result is proved.

This adds in-memory user-facing native topology history alongside the previous
value history. Durable StudioRuntime project history is separate. Controllers,
automation, initial values, tempo modes and plugin/derived state are not restored
by this raw control graph journal. Resident XML/state allocation, native graph
processing and repeated state checks can be quadratic; no hard deadline, latency,
CPU/RSS or audio-thread suitability is established. GUI drop/unlink, C++/Qt/MOC/ABI,
legacy/plugin, PCM/hardware and real provider reasoning/listening/revision/full
production acceptance remain unverified. Inline/plugin-only linked copies remain
incomplete, and the full commission is not complete.

Source diagnostics pass across 441 changed/new C++ files and 150 native production
operations. New fixture inventory is thirteen cases / 29 scenarios. Validator
Python syntax, report JSON, touched-file newlines, fixture whitespace and git diff
--check pass. Native build tools and an LMMS executable remain absent from PATH,
and all 17 pinned submodules remain uninitialized. Source checks do not compile or
execute native graph history, GUI actions or structured recovery.


## Shared native graph transition planning

2026-10-14 source continuation; the preceding turn made native topology-journal
source progress. No native C++/Qt/MOC compilation, fixtures, GUI/plugin/legacy,
actual audio/hardware or provider execution occurred. The complete DAW commission
remains active and completion remains unproven.

Inspection found repeated work in NativeLinkedControls.applyGroups, shared by
project/copy restoration and the new native topology journal. It captured the
same old ring for every selected member, then repeatedly searched the shrinking
ring for each selected model's predecessor while detaching. Large controller
groups incurred quadratic edge work before controller-dependency validation.

The helper now validates unique nonempty proposed positions and captures each
affected old ring once in a weak model/next edge map. Remaining peers' outgoing
links are planned from those immutable old edges: selected models are detached,
and an unselected model skips the following selected run to its next unselected
peer. Each removed run has only one remaining predecessor, preserving outside
peer order without repeatedly traversing the ring. After the plan is complete,
a cooperative check runs and every captured weak model/next identity and current
edge is revalidated before mutation. Controller bindings are gathered afterward,
so a cancellation/admission callback cannot leave feedback checks using an old
binding. Each distinct controller is checked once per proposed group rather than
repeating its full group scan for duplicate bindings.

The prepared remaining links and proposed ordered groups are installed without
model notifications or raw-value propagation. Feedback rejection restores the
complete old edge map, including unselected peers; successful transitions
invalidate all affected buffers through the existing policy. Topology generations
still advance on installation/rollback, and musical clocks are unchanged. Distinct
saved references that load into the same native receiver are now rejected before
the edge installation phase. Empty graphs preserve unrelated native groups.

Under expected constant-time hash operations, edge capture, remaining-link
planning and installation use linear work in the affected old/selected controls,
with proportional resident edge, weak-reference and controller-list storage.
This is a source-level algorithm assessment, not executed timing or memory
evidence. Controller.hasModel dependency traversal and distinct-controller/member
checks can still dominate or be quadratic or worse; construction, repeated native
load/unlink operations and journal notification validation retain their separate
costs. Allocation and native traversal have no hard deadline. Native owner
synchronization and valid existing rings remain required. Weak guards cover the
admission check, not concurrent mutation, arbitrary callee/parent/plugin deletion
or throwing cleanup. Earlier XML loads, callback effects and binding changes are
not reversed by graph-edge rollback.

NativeGraphTransitionTest is registered with a 300-second timeout and authors
seven cases / 15 scenarios. Four layouts cover interleaved/contiguous selection,
multiple old rings and an unrelated ring, checking remaining-peer order, unchanged
raw values and no extra model signals. Other cases cover full old-edge rollback
on controller feedback with outside peers, late cancellation/outside-peer
retirement/detachment/controller rewiring at the final cooperative boundary,
1050/4096-control native graph reconstruction from two old rings, duplicate/late
controller bindings, an explicit empty graph and two saved references aliasing
one receiver. The late-boundary fixture first counts checks from a successful
native finish, then injects at its final check without fixing XML node/check
counts. All fixtures remain authored, uncompiled and unrun; no native performance,
memory-safety, compatibility, PCM or musical result is established.

This change shares the existing graph schema and 150 structured production
operations. Full GUI/legacy/plugin, C++/Qt/MOC/ABI, actual audio/MIDI hardware,
latency/CPU/RSS and real provider reasoning/listening/revision/full production
acceptance remain unverified. Inline/plugin-only linked copies and other remaining
commission requirements remain incomplete; this is not completion of the DAW.

Source diagnostics pass across 442 changed/new C++ files and 150 native production
operations. New fixture inventory is seven cases / 15 scenarios. Validator Python
syntax, report JSON, touched-file final newlines, fixture whitespace and git diff
--check pass. Native build tools and an LMMS executable remain absent from PATH,
and all 17 pinned submodules remain uninitialized. Source checks do not compile
or execute the shared graph transition or establish full-system acceptance.


## Inline note control discovery, persistence and whole-clip copying

2026-10-14 source continuation; the preceding turn made graph-transition source
progress. No native C++/Qt/MOC compilation, fixtures, GUI/plugin/legacy, actual
audio/hardware or provider execution occurred. The complete DAW commission remains
active, and completion remains unproven.

Inspection confirmed that DetuningHelper and its untracked AutomationClip are
outside the QObject ancestry used by model discovery. Project graph capture and
whole-clip control snapshots omitted them, and ordinary constructor cloning copied
scalar curve controls without connections or topology. Inline save skipped default
or empty curves, and zero-length notes skipped detuning entirely. The standalone
constructor path also used a live controller-derived value and could insert a
default point when rebinding a copied empty curve.

NativeLinkedControls.clipModels now visits existing note helpers and inline curves,
including owned MIDI controller controls. Project discovery includes those models
for song and PatternStore clips; track discovery includes them only when clips are
requested, preserving preset boundaries. None of this discovery creates lazy
curves. StudioCapabilities adds these roots to parameter discovery, locates direct
helpers/curve controls by container/track/clip/note, and exposes helper and curve
mute/solo handles in full/section detuning inspection. Existing parameter link/set/
inspection can address them as current project handles. The persistence-scope
descriptor now distinguishes these paths from remaining standalone/plugin copies.

InlineAutomation saves versioned inlinecontrol metadata for its native scalar,
scale and controller connection. When an existing curve is present, full note
serialization retains its native points/tangents and mute/solo state even if it is
empty/default. Zero-length notes retain detuning state too. A hasCurve marker
preserves a lazy helper without manufacturing a curve on reload. Invalid versions,
duplicate/contradictory curve metadata and missing curve state refuse loading.
Legacy inline curve nodes continue to load. Older readers ignore the new metadata;
they do not reproduce newly persisted scalar/controller/linked state. This adds
XML for default/empty curves and can increase project size.

Clip control snapshots now include a nativeInlineControls note roster for MIDI
clips, alongside existing clip controls. They serialize each helper and its curve
controls without serializing curve points or replacing native constructors. Load
validates version, count, unique bounded index and note key/position/length before
applying metadata. Whole-clip clone/copy, native selection copying and full XML
copy/clipboard paths thereby restore groups spanning copied notes, or multiple
selected clips, onto fresh native models. Foreign members refuse partial copies;
complete project graphs can include owners outside the clip. Source notes, curves
and graph identities remain independent from the destination. Native constructor
copies still retain their own note/point/tangent ownership.

Inline curves have a private binding operation that replaces targets/pending IDs
with their actual helper without inserting a point into an empty map. Inline
load/deep-copy uses it instead of retaining source object IDs. Deep-copy scalar
initialization uses storedValue rather than controller-derived output. Ordinary
shallow Note copies still share their detuning pointer; no XML or discovery was
added to that playback constructor. Standalone Note.clone/DetuningHelper/untracked
AutomationClip constructor graph copying and selected-note GUI clipboard remain
outside portable graph scopes. Their DSP/GUI behavior requires native execution.

SaveScope records the note DOM owner of inline serialization. If the same helper
appears under different editable notes and either subtree contains native graph
references, publication refuses: native load creates separate receivers and cannot
restore that shared QObject identity. This includes linked owned-MIDI descendants,
not just the scalar. Unlinked helper sharing keeps the prior by-value persistence
behavior. This refuses ambiguous graph publication before writing a project or
constructing a partial clip copy.

LinkedInlinePersistenceTest is registered with a 300-second timeout and authors
fourteen cases / 25 scenarios: whole-project model discovery and graph roundtrip,
five native clone/copy/state/selection/clipboard paths with fresh identities and
curve independence, external-owner graph persistence with partial-copy refusal,
groups spanning two selected MIDI clips, independent owned MIDI state and stored
scalar copying, legacy/lazy/empty/zero-length notes, lazy inspection and current
locations/handles, five malformed metadata refusals before helper value changes,
runtime undo/redo/reopening, unchanged shallow Note sharing and empty deep-clone
rebinding, structured native links, cancellation before destination publication,
PatternStore-to-song copying, and refusal of linked shared editable helpers. All
are authored, uncompiled and unrun; they prove no native DSP, compatibility,
performance, musical or full-system behavior.

Native owner-thread synchronization remains required. Direct failures can retain
partial destination/model/controller effects, and arbitrary callback/callee/parent/
plugin deletion or concurrent mutation remains unsupported. Whole-project runtime
checkpoints continue to provide structured recovery. Discovery/location scans
notes and builds model lists; graph/roster XML remains resident, with no native
hard-deadline, CPU/RSS or latency measurement. Standalone/selected-note/plugin-only
copy graph gaps and other commission requirements remain incomplete. Actual
C++/Qt/MOC/ABI, GUI/legacy/plugin, PCM/hardware and provider reasoning/listening/
revision/full production acceptance remain unverified.

Source diagnostics pass across 444 changed/new C++ files and 150 native production
operations. New fixture inventory is fourteen cases / 25 scenarios. Validator
Python syntax, report JSON, touched final newlines, fixture whitespace and git diff
--check pass. Native build tools and an LMMS executable remain absent from PATH,
and all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute inline graph restoration, native copies or the complete DAW.


## Native selected-note transfer and scoped clip journals

2026-10-14 source continuation; the preceding turn made inline-persistence source
progress. No native C++/Qt/MOC compilation, fixtures, GUI/plugin/legacy, actual
audio/hardware or provider execution occurred. The complete DAW commission remains
active and completion remains unproven.

Inspection confirmed that piano-roll paste loaded each note and then called
MidiClip.addNote, which deep-cloned that individual note again. A graph restored
across prepared notes would be lost by this second copy. Copy/cut also lacked a
selected-note graph scope, and native clip checkpoints omitted linked topology.

NativeNoteTransfer is a core API for one live MIDI clip's selected notes and one
destination, callable without GUI objects. NativeLinkedControls.noteModels collects
the selected helpers, existing curves and owned MIDI controls. Capture snapshots
the source pointer roster and note identity tokens, rejects duplicate/foreign or
out-of-range note state, and uses SaveScope over the complete selected control set.
Missing members and linked shared editable helpers refuse serialization. Full
clipboard capture writes a versioned native note-list. Direct copy snapshots only
controls, retaining native Note/curve constructors for resident points/tangents.
Explicit source origin and destination position determine placement.

Load validates document/list shape, note attributes, native key/volume/pan/type
bounds and widened endpoints before creating models. Legacy unversioned lists and
tone/octave fields remain accepted, with intermediate integer bounds checked before
the legacy native loader can overflow. Connection XML is walked before loading:
only owned MIDI controller factories and currently available native rack references
are admitted. Other embedded factories could otherwise create an unregistered
controller under Song before later validation rejected it. Current native rack
ordinal semantics remain; this is not portable cross-project controller identity.

Prepared Notes are held by unique ownership, with fresh native control identities
and graph restoration completed before insertion. MidiClip.appendPreparedNotes
prepares and sorts the combined pointer roster before swapping it into the clip,
then transfers ownership without cloning again. Source note lifetime tokens and
weak clips are rechecked across cooperative admission/preparation boundaries.
Inherited cancellation is retained while event polling is suppressed. This does
not pin QObject owners or support arbitrary callbacks changing model state, callee/
parent/plugin deletion, throwing cleanup or concurrent mutation.

Clip now overrides native saveJournalState/restoreJournalState with a versioned
nativeClipJournal wrapper containing its complete native body and closed linked
graph. Native history keeps the Clip object while note/helper controls can be
recreated. Legacy journal bodies retain their loader. Selected-note insertion
prepares a complete clip checkpoint before editing when global/receiver journalling
is enabled. Missing coverage or cross-owner links refuse insertion rather than
silently losing topology in undo; disabled history bypasses this checkpoint.
Invalid journal headers refuse before loading. Malformed bodies/graphs or returned
callback failures during restore can leave partial state; the journal is not a
whole-project transaction and cannot reverse outside automation, plugin or external
effects. Broader structured recovery remains in StudioRuntime project checkpoints.

Piano-roll clipboard/cut/paste delegates to the core. Failed copying preserves the
clipboard, cut checks successful publication/source identity/checkpoint before
removing notes, and paste changes selection only after success. GUI bar-origin and
timeline quantization conventions remain. Initial preparation failures release owned
notes. Allocation or notification failures after ownership insertion can leave a
complete native edit committed, with a checkpoint when history was enabled; the
caller receives failure without a result roster. Arbitrary GUI/post-commit owner
retirement is not universally safe. A structured agent operation for selected-note
copying remains to be exposed; no native operation was added this turn.

NativeNoteTransferTest is registered with a 300-second timeout and authors twelve
cases / 35 scenarios: constructor-based graph/point/attribute preservation, modern
and two legacy clipboard layouts, seven invalid selections, twelve malformed
document/time/key/controller cases, native graph undo/redo on the same Clip,
incomplete enabled-history checkpoint refusal, post-insertion notification failure
with usable undo, cancellation/destination retirement without published results or
timer dispatch, source selection retirement including an aliased input roster,
foreign-thread refusal, 1050-note graph transfer and four invalid clip journal
headers. All remain authored, uncompiled and unrun, proving no native behavior,
audio, compatibility, timing or full-system acceptance.

Complete XML, model lists, prepared and combined rosters, and identity tokens remain
resident. Selection membership checks can be quadratic, sorting/allocation/native
calls have no hard deadline, and CPU/RSS/latency remain unmeasured. XML uses existing
native precision while direct constructor copying preserves curve values. Standalone
Note.clone/other duplication gestures/plugin-only graph copying, the structured
selected-note operation and other commission requirements remain incomplete. Actual
C++/Qt/MOC/ABI, GUI/DSP/legacy/plugin, PCM/hardware and real provider reasoning/
listening/revision/full production acceptance remain unverified.

Source diagnostics pass across 447 changed/new C++ files and 150 native production
operations. New fixture inventory is twelve cases / 35 scenarios. Validator Python
syntax, report JSON, touched final newlines, fixture whitespace and git diff --check
pass. Native build tools and an LMMS executable remain absent from PATH, and all 17
pinned submodules remain uninitialized. These source checks do not compile or
execute native note transfer, clip history or the complete DAW.

### Selected-note agent operation continuation (2026-10-14)

This continuation makes source progress on the active complete DAW commission.
It exposes the existing native selected-note transfer through `note.selection.copy`;
completion remains unproven. No native build, fixture, GUI, DSP, plugin, audio,
hardware or provider execution was obtained.

The operation accepts one source MIDI clip, 1..4096 distinct current note indices,
one existing destination MIDI clip and a destination-relative position. Optional
source/destination containers select song or pattern rows. The earliest selected
note anchors placement; relative timing, native note/curve content and complete
closed inline control groups use NativeNoteTransfer. Sources remain, including
same-clip copies. Pattern destinations append into existing cells. Native recording
admission, controller factory/rack validation and graph coverage remain in the core.

Source selection captures indices and lifetime tokens and checks them again after
cooperative admission. Receipts map request-order source indices to current sorted
destination indices and positions. NativeNoteTransfer now retains the lifetime
token of every prepared note and validates all result notes against the current
destination roster after returned insertion and song-modified callbacks. Retirement or address reuse
refuses a successful result. This check does not make arbitrary callback mutation
or receiver/callee retirement safe. Direct post-commit failures can retain edits;
StudioRuntime supplies the existing whole-project recovery/history boundary.

Queued Selection now retains a separate token for every resolved selected note,
alongside weak source/destination track and clip objects. Deleted/replaced notes
and owners invalidate pending calls; unresolved future indices retain existing
batch semantics, and same-object assignment preserves lifetime identity. This adds
native Selection layout and requires a complete rebuild. Both Chat and Responses
declare the operation individually and expose catalog routing. Seven protected
native declarations, 151 native operations and ten agent operations still use the
existing 128-declaration inference budget. Prompt guidance covers placement,
closed groups, request-order receipts and rediscovery after sorting.

StudioNoteSelectionCopyTest is registered with a 300-second timeout. Fourteen
cases / 49 scenarios cover request-order/sorted receipts, same-clip insertion,
three song/pattern directions, sixteen malformed/boundary requests, recording and
open-group refusal, owned MIDI binding copies, durable receipt replay/undo/redo/
reopening, returned insertion exception and insertion/song-notification retirement recovery, five queued identity
changes, future note creation, four scripted protocol/direct/catalog plans, eight
stale-batch replan routes, cancellation without event dispatch, a 1050-note linked
selection, and schema/tool declarations. These are authored, uncompiled/unrun
fixtures. Scripted provider injection proves no actual musical reasoning, listening,
evaluation, revision or autonomous production.

Selection limits do not bound full native rosters, serialized control inventories,
callbacks or allocations. Membership scans can be quadratic; sorting and native
calls have no hard deadlines. CPU/RSS/latency remain unmeasured. Rack ordinals,
serializer coverage, standalone Note clones/other duplication gestures/plugin-only
copying, outside automation/plugin effects and native callback lifetime contracts
retain the previous limitations. Full C++/Qt/MOC/ABI, legacy/plugin/GUI/DSP, actual
PCM/hardware and real-provider complete DAW acceptance remain unverified.

Source diagnostics pass across 448 changed/new C++ files and 151 native operations.
Validator Python syntax, report JSON, fixture inventory/whitespace, touched final
newlines and git diff --check pass. Native build tools and an LMMS executable remain
absent from PATH, and all 17 pinned submodules remain uninitialized. These source
checks do not compile or execute the new operation or complete the commission.

### Shared native note shifting continuation (2026-10-14)

The previous continuation made source progress by exposing native selected-note
copying. This continuation addresses transposition and time shifting that still
lived in piano-roll methods. The complete DAW commission remains active and
completion unproven. No native compilation, fixtures, GUI/plugin/DSP, actual
audio/hardware or provider execution was obtained.

MidiClip.shiftNotes now owns the shared operation, implemented in core
NativeNoteShift.cpp and used by both piano-roll shift shortcuts and the structured
note.selection.shift capability. The clip/note/inline helper/curve/controller
identities remain in place. No Note cloning or XML round trip performs the edit;
links outside the selection remain native links. Native per-note key clamping is
preserved using widened integer arithmetic. Negative time shifts stop the minimum
selected position at zero, including unsorted input and INT_MIN. Any shifted
positive endpoint beyond MaxSongLength refuses the whole edit before writing.
Step/zero-length notes retain length and type.

The core requires an idle registered clip on the model owner thread, acquires the
native engine changes guard and suspends event pumping while retaining inherited
cooperative cancellation. It snapshots the selected and complete clip rosters,
captures selected note lifetime/scalar state, validates membership/uniqueness and
native bounds, and prepares a stable sorted pointer roster before publishing
history. Selected notes are checked again after cooperative work and checkpoint
serialization. Native scalar setters and the roster swap run together under the
instrument track lock before notifications. Empty/no-op core calls publish no
edit or history. The exported method/result type require a complete native rebuild.

Enabled native history uses the existing complete clip graph checkpoint before
mutation. Cross-owner graph coverage can refuse a checkpoint; disabled history
leaves those links intact. Post-commit returned callback exceptions can leave a
native edit applied, with the checkpoint available when enabled. The result stays
unsuccessful. Returned callbacks retiring/replacing selected notes or the clip
refuse successful results after length, data and song-modified notifications.
Callback scalar changes are not undone or asserted equal to requested values.
Arbitrary callee/parent/plugin deletion, reentry, concurrent mutation, throwing
cleanup and external side effects remain unsupported; this is no universal native
transaction. StudioRuntime retains its whole-project recovery boundary.

The structured operation accepts one song/pattern MIDI clip and 1..4096 distinct
current note indices, with bounded signed semitones and/or ticks. It validates all
arguments before editing, rechecks selected lifetimes and returns request-order
receipts containing sourceNote/current sorted note index/position/key, requested
deltas, actual appliedTicks and changed. Note indices can change when selected
notes cross unselected ones. Shared queued provenance tracks source owners and
each selected note token; existing stale-selection handling skips affected queued
calls and replans. Same-object edits retain lifetime identity by design.

Both Chat and Responses declare note_selection_shift individually and retain
studio_execute catalog routing. Eight protected native declarations, 152 native
operations and ten agent operations fit the existing 128-declaration inference
budget. Prompt/schema/protocol guidance explains clamping, actual time movement,
receipt ordering, overflow refusal and rediscovery after sorting. Structured no-op
calls retain the runtime's ordinary mutating-operation revision semantics.

StudioNoteSelectionShiftTest is registered with a 300-second timeout. Seventeen
cases / 55 scenarios author in-place graph/curve/outside-link and step/zero-length
retention, four unsorted/integer clamp cases, stable ties/unselected notes, eight
native admission failures, no-op history/notifications, clip undo/redo, open-graph
checkpoint refusal and disabled-history transfer, usable undo after notification
exception, five recording/ownership/cancellation/retirement failures, sixteen
invalid structured requests, song/pattern sorted receipts, durable session undo/
redo/reopening/replay, three returned notification recovery cases, four scripted
shift/inspect protocol/tool routes, four stale-index replan routes, 1050 linked
notes and schema/tool budget. These fixtures are uncompiled/unrun. Scripted
inference supplies no actual musical reasoning, listening, evaluation or revision.

The selection bound does not bound the complete source roster, helpers, checkpoint
XML, allocations or native callbacks. Membership scans can be quadratic; stable
sorting/native serialization/work have no hard deadline. GUI now uses these
explicit ownership/recording/history/upper-time bounds instead of its unchecked
local loops; compatibility and resource cost need native verification. No
quantization/stretch/length/velocity transforms were added. CPU/RSS/latency,
C++/Qt/MOC/ABI, GUI/legacy/plugin/DSP, PCM/hardware and real-provider full production
acceptance remain unverified.

Source diagnostics pass across 450 changed/new C++ files and 152 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These checks do not compile
or execute the shift implementation or prove complete DAW acceptance.

### Native in-place quantization continuation (2026-10-14)

The previous turn made source progress by sharing native phrase shifting between
the piano roll and structured runtime. Current inspection found that piano-roll
quantization still removed each selected note, made a shallow copy and inserted a
deep clone. That replaced native note/helper identities and could lose linked
control relationships. This continuation addresses that path and exposes native
quantization to the agent. The full DAW commission remains active and unproven.
No native compilation, fixture, GUI/plugin/DSP, audio/hardware or provider
execution was obtained.

MidiClip.quantizeNotes now edits current notes in place. Shift and quantize wrappers
share private editNoteSelection planning/admission/checkpoint/commit/notification
handling in NativeNoteShift.cpp. Selected and complete clip rosters, selected
lifetime/scalar snapshots, cooperative checks, stable sorted roster preparation,
complete enabled clip checkpoint, scalar writes/roster swap under the native track
lock, and post-notification lifetime checks remain common. The plan now includes
old/new lengths, which are revalidated before writing. Complete roster lifetime/
position snapshots are rechecked before committing prepared order, including
unselected notes moved by a cooperative callback. Note, helper, curve, controller
and linked graph identities remain in place. Outside links are retained
by disabled-history native edits; enabled clip checkpoints require closed coverage.
The native runtime retains whole-project recovery for structured failures.

QuantizeMode selects positions, lengths or both. Native Note.quantized supplies
the exact existing float rounding and negative-length truncation behavior. Bounded
source/grid values and a compile-time MaxSongLength integer bound keep native
rounding intermediates representable; complete endpoint admission occurs before
checkpoint publication or writing. Original zero-length notes skip position and
length changes. Rounded zero lengths become one grid unit. Negative step lengths
can become positive under the existing native routine; note type remains unchanged.
No curve resampling/stretching or duplicate-note merging was added.

PianoRoll.quantizeNotes now selects its native mode and delegates to the core. It
does not remove or recreate notes, and uses one final native notification instead
of per-note remove/add notifications. Empty/no-op native calls publish no edit or
history. These deliberate identity/notification changes require GUI/legacy/plugin/
DSP compatibility checks. The shared core adds exported native APIs and private
planning declarations, requiring a complete native rebuild and shift regressions.

The note.selection.quantize operation accepts one existing song/pattern MIDI clip,
1..4096 distinct current note indices, a positive bounded tick grid and optional
mode (both by default). Strict fields and scalar bounds are checked before edits.
It shares per-note/owner queued provenance with shifting/copying and returns
request-order receipts with source/current sorted indices, position, key and
length, plus grid/mode/changed. Quantized positions can cross unselected notes;
affected queued calls are skipped and replanned. Same-object edits retain lifetime
identity. No-op structured calls retain ordinary mutating-operation history rules.

Both Chat and Responses declare note_selection_quantize individually and retain
studio_execute routing. Nine protected native declarations, 153 native operations
and ten supplemental agent operations fit the 128-declaration inference budget.
Prompt/schema/protocol guidance documents mode/grid, native negative and zero
length behavior, retained identities, overflow refusal and sorted receipts.

StudioNoteSelectionQuantizeTest is registered with a 300-second timeout. Sixteen
cases / 67 scenarios author graph/curve/outside-link and note lifetime retention,
twelve explicit rounding/mode/grid/step/zero-length expectations, nine native
admission failures, three endpoint refusals, no-op history/notifications, clip
undo/redo, open-graph checkpoint refusal and disabled-history retention, six
recording/ownership/cancellation/retirement/unselected-movement cases, seventeen malformed structured
requests, song/pattern sorted receipts, session undo/redo/reopening/replay, three
returned notification recovery cases, four scripted quantize/inspect routes, four
stale-index replan routes, 1050 linked notes and schema/tool bounds. All are authored,
uncompiled/unrun; scripted decisions prove no real musical reasoning, listening,
evaluation, revision or production.

Returned notification exceptions or retirement can leave a complete/partial native
edit and its checkpoint when enabled. Scalar callback edits are not reversed or
asserted equal to requested values. Arbitrary self/callee/parent/plugin deletion,
reentry, concurrent mutation, throwing cleanup and external effects remain outside
the common native contract. Full rosters/maps/checkpoint XML remain resident;
membership scans can be quadratic, native sorting/allocation/serialization/callback
work has no hard deadline and CPU/RSS/latency are unmeasured. Native C++/Qt/MOC/ABI,
GUI/legacy/plugin/DSP, actual PCM/hardware and real-provider full musical production
acceptance remain unverified.

Source diagnostics pass across 451 changed/new C++ files and 153 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Native build tools and an LMMS executable remain absent
from PATH, and all 17 pinned submodules remain uninitialized. These checks do not
compile or execute quantization or prove complete DAW acceptance.

### Native selected-note reversal continuation (2026-10-14)

The previous turn made source progress on native in-place quantization. Current
inspection found that native MidiClip reversal still used an unchecked loop with
an ordinary journal checkpoint and had no structured operation. This continuation
shares the existing note-edit admission/history path and exposes reversal to the
agent. The complete DAW commission remains active and unproven. No native
compilation, fixtures, GUI/plugin/DSP, audio/hardware or provider execution occurred.

The checked MidiClip.reverseNotes overload delegates to the common selection edit
kernel in NativeNoteShift.cpp. Native geometry is retained: take the minimum
selected position and maximum selected end, then compute each new start as latest
end minus its old offset from the earliest start minus its own duration. Bounds
and arithmetic use widened intermediates before native integer assignment. Keys,
lengths, types, note/helper/controller identities, curve contents and graph links
are retained in place. Inline points are not mirrored or resampled. Zero/negative
step lengths follow native geometry; a mixed signed-length selection that would
place notes outside the song range refuses the whole edit before writing.

Shared owner-thread/registered-clip/idle-recording admission, selected and full
roster snapshots, lifetime/scalar/position revalidation across cooperative hooks,
prepared stable sorting, complete enabled clip graph checkpoint, scalar writes/
roster swap under the native track lock, length/data/song notifications and
post-notification lifetime checks apply to reversal too. Empty/single/symmetric
unchanged native selections publish no edit/history. The existing void method
delegates and logs refusal with qWarning rather than assuming an available song
or mutating native error state on a foreign thread. The piano roll uses the checked
overload and its existing native error reporting. A complete native rebuild is
required for the exported overload and shared planning changes.

The note.selection.reverse capability accepts one song/pattern MIDI clip and
1..4096 distinct current note indices with strict fields. It shares queued note/
owner provenance and returns request-order source/current sorted indices,
positions, keys and lengths plus changed. Reversal generally reorders indices;
affected queued calls are skipped and replanned. Same-object lifetime identity
remains stable. Both Chat and Responses declare note_selection_reverse individually
and retain studio_execute routing. Ten protected declarations, 154 native operations
and ten supplemental agent operations use the existing 128-declaration inference
budget. Prompt/schema/protocol guidance covers geometry, retained curve contents,
bounds and index rediscovery. Structured no-op calls keep the ordinary runtime
mutating-operation revision semantics.

StudioNoteSelectionReverseTest is registered with a 300-second timeout. Fourteen
cases / 51 scenarios author graph/curve/outside-link and note identity retention,
double reversal through checked and legacy core entries, six explicit duration/
overlap/tie/zero/step geometry expectations, seven malformed/native overflow
failures, unchanged selection history/notifications, clip undo/redo and outside
coverage refusal/disabled-history retention, six recording/ownership/cancellation/
retirement/unselected-movement cases, thirteen malformed structured requests,
song/pattern sorted receipts, durable session replay/undo/redo/reopening, three
returned notification recovery cases, four scripted reverse/rediscover routes,
four stale-index replan routes, 1050 linked notes and schema/tool budget. Graph
inspection accounts for reversed note ordering. All are authored, uncompiled/unrun;
scripted decisions supply no real musical reasoning, listening or production proof.

The guarded path adds owner/recording/upper-time/history admission, stable ordering
and clip length/notification updates to the old reverse loop; native GUI/plugin/
legacy/DSP compatibility requires execution. Enabled clip history needs closed
serializer coverage. Returned post-commit exceptions/retirement can retain complete
or partial native edits and their checkpoint when enabled; structured recovery
uses whole-project restoration and may invalidate handles. Scalar callback edits,
arbitrary self/callee/parent/plugin deletion, reentry, concurrent mutation, throwing
cleanup and external effects retain the shared limitations. Full rosters/tokens/
maps/XML remain resident, membership can be quadratic, native allocations/sorting/
serialization/callbacks have no hard deadline, and CPU/RSS/latency are unmeasured.
C++/Qt/MOC/ABI, actual PCM/hardware and real-provider complete musical production
acceptance remain unverified.

Source diagnostics pass across 452 changed/new C++ files and 154 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Native build tools and an LMMS executable remain absent
from PATH, and all 17 pinned submodules remain uninitialized. These source checks
do not compile or execute reversal or prove complete DAW acceptance.

### Shared native note-length editing continuation (2026-10-14)

The previous turn made source progress on native selected-note reversal. Current
inspection found that trim/fill and minimum/maximum length commands still edited
notes directly in the piano roll. This continuation moves all four modes into the
native MIDI clip model and exposes them through note.selection.length. The complete
DAW commission remains active and unproven. No native compilation, fixture,
GUI/plugin/DSP, actual audio/hardware or provider execution occurred.

MidiClip.adjustNoteLengths delegates to the existing shared in-place selection
kernel in NativeNoteShift.cpp. Maximum/minimum use the native signed length
comparison and supplied bound. Trim only shortens to the first strictly later
note start among all clip notes, leaving terminal notes unchanged. Fill only
extends to the first start at/after the original note end, or to the selected
phrase's maximum original end rounded up to a complete current bar when there is
no reference. These retain the prior GUI's sorted start/end comparison semantics,
including same-start notes, overlap skipping, exact ends, zero lengths and negative
step lengths. Bound changes can change sign without changing note type. Positions,
keys, helpers/controllers/curves, note identities and graph links remain in place;
stored inline curve points are not stretched/resampled.

Reference positions come from the complete clip snapshot, including unselected
notes and other keys. A sorted position vector plus lower/upper bound searches
prepares candidate lengths independently of selection/request ordering. Fill uses
widened tail/candidate arithmetic and snapshots ticks per bar, which must remain
unchanged through cooperative admission and checkpoint serialization. Reference
positions, candidate lengths and positive endpoints must fit native song bounds;
an overflowing final bar refuses the whole edit before writing. Shared full-roster
lifetime/position and selected scalar checks, native track locking and prepared
stable sorting/commit remain in force. No-op/empty core calls publish no edit or
history. A native rebuild is required for the exported API/planning declarations.

PianoRoll.fitNoteLengths and constrainNoteLengths select the native mode/bound and
delegate. The GUI retains its selected-or-all convention and last-note signed bound.
The new path adds explicit owner/registered-clip/recording/time/history admission,
stable sorting and final clip length/data notifications to the prior local loops.
Those behavioral changes need native GUI/plugin/legacy/DSP compatibility evidence.
Complete enabled clip graph checkpoint coverage is required; disabled history
retains outside links and structured runtime recovery uses full project checkpoints.

The note.selection.length operation takes one song/pattern MIDI clip, 1..4096
distinct current indices and an explicit mode. Maximum/minimum require signed
length; trim/fill forbid that field. Strict field/type/mode/bound checks precede
editing. Request-order receipts report source/current sorted indices, position,
key, resulting length and changed; bounded modes echo their bound. Source indices
normally stay stable because positions do not move. Shared queued provenance
retains every resolved selected note and owner identity, with existing stale-call
skip/replan behavior. Structured no-op calls follow ordinary mutating revisions.

Both Chat and Responses declare note_selection_length individually and retain
studio_execute routing. Eleven protected declarations, 155 native operations and
ten supplemental agent operations fit the existing 128-declaration inference
budget. Prompt/schema/protocol guidance documents signed bounds, reference and tail
rules, preserved types/curve contents and whole-edit endpoint refusal.

StudioNoteSelectionLengthTest is registered with a 300-second timeout. Seventeen
cases / 75 scenarios author identity/curve/outside-link retention, eight signed
constraint expectations, ten trim/fill reference comparisons, selected-phrase tail
and unchanged queued identity, ten native malformed/bound failures, endpoint and
final-bar refusal, no-op history/notifications, native undo/redo/coverage refusal,
seven recording/ownership/cancellation/retirement/reference/meter-change cases,
eighteen malformed structured requests, song/pattern request-order receipts,
durable session replay/undo/redo/reopening, three returned notification recovery
cases, four scripted length/inspect routes, four stale-selection replan routes,
1050 linked notes and schema/tool budget. All remain authored, uncompiled/unrun;
scripted decisions provide no real musical reasoning, listening or production proof.

Signed native step/playback semantics, incomplete serializer coverage, native
notification/cleanup contracts and outside plugin/automation effects retain the
shared limitations. Returned post-commit exceptions/retirement can retain native
edits and a checkpoint when enabled; scalar callback edits are not reversed.
Arbitrary self/callee/parent/plugin deletion, reentry, concurrent mutation, throwing
cleanup and external effects remain unsupported. Whole-project recovery can
invalidate handles. Full rosters/tokens/reference vectors/maps/checkpoint XML stay
resident; reference planning uses sorting/binary search but membership can be
quadratic. Native allocation/serialization/callbacks have no hard deadline and
CPU/RSS/latency remain unmeasured. C++/Qt/MOC/ABI, actual PCM/hardware and real-provider
complete musical production acceptance remain unverified.

Source diagnostics pass across 453 changed/new C++ files and 155 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Native build tools and an LMMS executable remain absent
from PATH, and all 17 pinned submodules remain uninitialized. These source checks
do not compile or execute length editing or prove complete DAW acceptance.

### Native selected-note glue continuation (2026-10-14)

The previous turn made source progress on native note-length editing. Current
inspection found that glue still grouped/deleted notes directly in the piano roll,
had no structured operation and could calculate begin()+1 on an empty selection.
This continuation moves glue into the native MIDI clip model and exposes it to the
agent. The complete DAW commission remains active and unproven. No native build,
fixture, GUI/plugin/DSP, actual audio/hardware or provider execution occurred.

MidiClip.glueNotes in core NativeNoteGlue.cpp uses native same-key overlap/touch
geometry and maximum group extent. Selection order does not determine equal-start
ties: selected notes first follow native clip roster order, then stable key/start
sorting. Each earliest group note survives with its own position/key/type/volume/
pan/helper/controller/curve identities and settings. Zero and signed lengths retain
native comparisons. Unselected notes are not consumed and cannot bridge gaps.
Consumed notes retire normally; their sound-design state is discarded, not merged
into the survivor. Native destruction detaches their links when control receivers
actually retire; shared ownership can retain helpers. Surviving curve points remain
as stored without resampling/stretching. These are native glue semantics, not a
promise to preserve every consumed note's settings in one resulting note.

The core requires owner-thread registered/idle clips, uses the native engine guard
and suppresses event pumping while retaining cooperative cancellation. Full roster
lifetime/position/key/length snapshots and selected bounds/uniqueness are checked
across cooperative work and checkpoint serialization. Groups, mappings, survivor
lengths, combined stable-sorted roster and consumed-note ownership capacity are
prepared before a complete enabled clip graph checkpoint. Survivor scalar writes
and roster replacement run under the native track lock. Only after ownership
leaves the clip are consumed notes retired through local unique ownership, followed
by type/length/data/song notifications and survivor lifetime checks. Preparation
failure does not apply the planned native edit. Empty/single/disjoint core calls
publish no edit/history. The exported result maps each input to its survivor and
reports consumed count; failures return an empty result.

PianoRoll.glueNotes now delegates, retaining selected-or-all behavior and native
error reporting. The safe empty path avoids the old iterator calculation. Compared
with the old GUI loop, tie selection is deterministic, admission/history boundaries
are explicit and retirement/notifications are batched. Full native rebuild and
GUI/plugin/legacy/DSP compatibility checks remain necessary.

The note.selection.glue operation accepts one song/pattern MIDI clip and 1..4096
distinct current note indices with strict fields. Shared queued provenance retains
every selected source lifetime and clip owner. Request-order receipts map original
sourceNote to original survivingSourceNote and current sorted note index, merged
status and position/key/length. Top-level removed counts consumed selected notes;
survivors counts their distinct surviving groups, excluding unselected notes.
Rediscovery is required after consumption/index shifts.
Existing stale-call handling skips affected queued selections and replans; retained
source identities can remain valid for later reads. Structured no-op calls retain
ordinary mutating-operation revision semantics.

Both Chat and Responses declare note_selection_glue individually and retain
studio_execute routing. Twelve protected declarations, 156 native operations and
ten supplemental agent operations fit the existing 128-declaration inference
budget. Prompt/schema/protocol guidance documents survivor settings, consumed-state
disposition, grouping/tie rules and receipt/index semantics.

StudioNoteSelectionGlueTest is registered with a 300-second timeout. Sixteen cases /
56 scenarios author survivor value/curve/identity retention and consumed helper/
curve retirement with outside-peer detachment, nine geometry/tie/zero/step cases,
selection/chain/key boundaries, empty/single/disjoint history/notifications, seven
native malformed/bound failures, complete clip undo/redo restoring consumed graphs,
outside checkpoint refusal and disabled-history behavior, six ownership/recording/
cancellation/retirement/cooperative-change cases, thirteen malformed structured
requests, song/pattern request-order mappings, durable session replay/undo/redo/
reopening, three returned notification recovery cases, four scripted glue/inspect
routes, four consumed-selection replan routes, 1050 linked notes and schema/tool
budget. All remain authored, uncompiled/unrun. Scripted inference proves no real
musical reasoning/listening/revision/production.

Enabled clip history requires closed serialized control coverage; native undo can
recreate note/helper identities and retains native XML precision. Returned
post-commit notification/cleanup failure can leave partial/complete edits and their
checkpoint; structured runtime uses whole-project recovery and may invalidate
handles. Arbitrary self/callee/parent/plugin deletion, reentry, concurrent mutation,
throwing destructors/cleanup and outside automation/plugin effects remain unsupported.
Full rosters/tokens/group maps/prepared vectors/checkpoint XML remain resident;
membership and linked cleanup can be quadratic. Allocation/sorting/serialization/
destruction/callbacks have no hard deadline and CPU/RSS/latency remain unmeasured.
C++/Qt/MOC/ABI, actual PCM/hardware and real-provider complete musical production
acceptance remain unverified. Native split/knife paths and other full commission
requirements still need work and verification.

Source diagnostics pass across 455 changed/new C++ files and 156 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Native build tools and an LMMS executable remain absent
from PATH; all 17 pinned submodules remain uninitialized. These checks do not compile
or execute glue or prove complete DAW acceptance.

### Native selected-note split continuation (2026-10-14)

The previous turn made source progress on native gluing. Current inspection found
that vertical splitting shortened source notes before cloning right halves and
used per-note addNote copying, without a selected control-graph scope. This
continuation prepares native right fragments and graph restoration before editing
left notes, and exposes note.selection.split. The complete DAW commission remains
active and unproven. No native build, fixture, GUI/plugin/DSP, actual audio/hardware
or provider execution occurred.

NativeNoteTransfer.split supplies the shared core, with a checked MidiClip overload
and delegating legacy vertical split entry. It accepts one cut tick, keeps original
left notes/controls in place and returns one right fragment or null per input.
Only strict crossings of positive-length notes split. Noncrossing/exact-boundary/
zero/negative lengths remain unchanged. Native copy semantics retain keys, type,
volume/pan and selection flags. Right inline curve points keep their existing
relative timing, without rebase/resampling for continuity; they may restart at the
new note start. No continuous audio or curve behavior is inferred.

The core requires an idle registered clip on the model owner thread and uses the
native engine guard with event pumping suppressed. Full roster lifetime/position/
length/key/selection snapshots are revalidated across cooperative work. Actual
crossing notes are captured through the existing native controls-only transfer
scope. Native constructors preserve resident points/tangents, and copied controls
restore onto fresh receivers under KeepIdentityScope/RestoreScope. Right groups
are independent of the retained left groups. Links outside the crossing set refuse
copying, including selected peers that do not cross this cut. Rack references keep
current native ordinal semantics; owned MIDI controllers are independently copied.

Prepared note ownership, left/right lengths, copied graph and the combined stable
sorted pointer roster are complete before the enabled whole-clip graph checkpoint.
Left length updates and roster insertion run under one native track lock, followed
by ownership release without another Note clone. Type/length/data/song notifications
precede final lifetime checks on every selected original and every new fragment.
Failures clear outputs; returned post-commit failures can retain complete/partial
edits and a checkpoint. No-op/empty core calls publish no edit/history and return
null per-input fragment slots. Local input/output vector aliasing is snapshotted
before result clearing. A complete native rebuild is required for the exported
API/header changes and altered native split path.

The note.selection.split operation takes one song/pattern MIDI clip, 1..4096
distinct current indices and a bounded clip-relative position. Strict field/type/
index checks and source lifetime validation precede editing. Request-order receipts
include original sourceNote, current left/unchanged index/position/key/length and
split status, plus right index/position/length only for actual cuts. Insertion can
shift unchanged later indices. Shared queued provenance skips retargeted selections;
existing unresolved future-target semantics allow inspection of newly created
fragments. Structured no-op calls retain ordinary mutating-operation revisions.

Both Chat and Responses declare note_selection_split individually and retain
studio_execute routing. Thirteen protected declarations, 157 native operations and
ten supplemental agent operations fit the existing 128-declaration inference
budget. Prompt/schema/protocol guidance explains strict crossings, retained left
identities, right graph closure, native curve timing and receipt/index semantics.
The sloped knife and delete-short-ends operation still uses its separate legacy
path; this continuation does not claim its conversion or GUI acceptance.

StudioNoteSelectionSplitTest is registered with a 300-second timeout. Seventeen
cases / 66 scenarios author input/output aliasing, source identities/selection and
independent copied graph/point/value state, owned MIDI controller independence,
nine explicit geometry/boundary/step cases, three crossing-group closure failures,
nine malformed/native boundary failures, no-op history with outside clip links,
legacy-entry clip undo/redo preserving both graphs, direct returned notification
failure with usable undo, seven ownership/recording/cancellation/retirement/roster/
selection-flag changes, seventeen malformed structured requests, song/pattern
sorted left/right/unchanged receipts, durable session replay/undo/redo/reopening,
three returned notification/left/right retirement recovery cases, four scripted
split/future-fragment routes, four shifted-unchanged-index replan routes, 1050
linked split notes and schema/tool budget. All are authored, uncompiled/unrun;
scripted inference supplies no real musical reasoning/listening/revision proof.

Enabled clip history requires closed whole-clip serialized coverage as well as
crossing-set copy coverage. Native constructor/controller preparation and cleanup
can have ordinary process effects; scalar callback changes are not reversed.
Arbitrary self/callee/parent/plugin deletion, reentry, concurrent mutation, throwing
cleanup and outside automation/plugin effects remain unsupported. Structured
whole-project recovery may invalidate handles; history can recreate note/helper
identities and XML precision remains native. Full rosters/tokens/control XML/clones/
prepared vectors remain resident, membership and repeated revalidation can be
quadratic, and constructors/graph/native callbacks have no hard deadline. CPU/RSS/
latency, C++/Qt/MOC/ABI, GUI/plugin/legacy/DSP, actual PCM/hardware and real-provider
complete musical production acceptance remain unverified.

Source diagnostics pass across 456 changed/new C++ files and 157 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Native build tools and an LMMS executable remain absent
from PATH; all 17 pinned submodules remain uninitialized. These checks do not compile
or execute splitting or prove complete DAW acceptance.

### Checked native knife continuation (2026-10-14)

The previous turn made source progress on vertical splitting and explicitly left
the sloped knife/delete-short-ends path separate. Current inspection confirmed
that the actual piano-roll gesture still used this path, removing originals and
creating per-note clones without a crossing-group graph scope. This continuation
converts that gesture and its keep-longer option to checked native cutting and
exposes note.selection.knife. The complete DAW commission remains active and
unproven. No native compilation, fixtures, GUI/plugin/DSP, actual audio/hardware
or provider execution occurred.

NativeNoteTransfer.split and knife now delegate to a common private cut kernel.
Line geometry retains native float slope, evaluation at the half-key centre,
minimum-exclusive/maximum-inclusive key filtering, horizontal no-op and tick
truncation. Tick/key endpoint differences widen before conversion to float, and
finite/native-int representability is checked before narrowing the intercept.
Full signed integer endpoints support off-clip/off-keyboard gesture coordinates;
only strict crossings inside bounded positive-length notes are applied. Planning
uses immutable captured scalar fields, with full roster lifetime/scalar/selection
revalidation before model mutation, including after cooperative work.

Both-halves mode retains originals as left notes and constructs fresh independent
right control groups with per-note cut positions. The existing closed crossing-set
copy policy applies. Keep-longer mode copies no notes/controls: it updates the
original length and, when the right interval is longer, its position. Left wins
equal lengths. Note/helper/controller/curve identity and existing outside links
remain, unlike the former replacement loop. No stored curve points are rebased or
resampled; relative shapes can restart at the retained/copied start. Enabled native
history still requires complete closed whole-clip coverage even without copying.

Prepared position maps let the complete combined roster sort before moving retained
right intervals. Native checkpoint publication precedes track-locked position/
length updates and ownership insertion. Source/right lifetime checks remain after
type/length/data/song notifications. Outputs include right-or-null and cut-or-minus-
one slots per input; failures clear both arrays. Empty/horizontal/no-crossing core
calls publish no edit/history. Vertical split keeps its existing checked/legacy
entry and output semantics through this kernel. All prior split fixtures remain
registered as regressions, but none has run.

MidiClip.splitNotesAlongLine has checked and delegating legacy entries. The actual
piano-roll gesture calls the checked overload and reports native errors while
still resetting gesture state on refusal. Original identity retention, stable
sorting, batched notifications and explicit owner/recording/time/history admission
are deliberate changes requiring complete native rebuild and GUI/plugin/legacy/
DSP compatibility verification. Exported KnifeResult holds per-input right notes
and cut positions; the common private options are not a second project model.

The note.selection.knife capability accepts one song/pattern MIDI clip, 1..4096
distinct current note indices, two signed-int position/key endpoints and optional
keepLongest (false by default). Strict types/fields/native int bounds precede
editing. Request-order receipts include original/current indices, actual position/
length/key and cut status, plus cutPosition and kept interval only for cuts and
right fragment indices when created. Echoed endpoints, cut count and changed are
reported. Shared queued provenance skips retargeted selections after insertion or
retained-note movement and replans; future-fragment targets keep existing semantics.
Structured no-op calls retain ordinary mutating-operation revisions.

Both Chat and Responses declare note_selection_knife individually and retain
studio_execute routing. Fourteen protected declarations, 158 native operations and
ten supplemental agent operations fit the existing 128-declaration inference
budget. Prompt/schema/protocol guidance covers row filtering, signed endpoints,
longer/tie rules, closure, retained control timing and sorted receipts.

StudioNoteSelectionKnifeTest is registered with a 300-second timeout. Seventeen
cases / 72 scenarios author aliased input/result handling, retained identities/
flags and independent copied graph/curve state, keep-longer owned controller and
outside-link retention, twelve explicit geometry/row/direction/truncation/extreme/
step cases, three longer/tie choices, seven native admission failures, three
copy-closure versus keep-longer cases, no-op history, legacy-entry native history
in both modes, seven ownership/recording/cancellation/retirement/roster/flag cases,
seventeen malformed requests, four song/pattern/mode receipt cases, durable session
undo/redo/reopening/replay, three returned notification/retirement recovery cases,
four scripted cut/future-fragment routes, four keep-longer reordered-index replan
routes, 1050 linked cut notes and schema/tool budget. All remain authored,
uncompiled/unrun; scripted decisions prove no real musical reasoning/listening/
evaluation/revision/production.

Native float interpolation/truncation remains rather than ideal rational geometry.
Preparation/controller factory and returned callback/cleanup side effects retain
the shared limitations. Post-commit failure can leave edits/checkpoint; scalar
callback edits are not reversed and structured whole-project recovery can invalidate
handles. Arbitrary self/callee/parent/plugin deletion, reentry, concurrent mutation,
throwing cleanup and outside automation/plugin effects remain unsupported. History
can recreate note/helper identities and XML precision stays native. Full rosters/
tokens/control XML/clones/maps remain resident, membership/revalidation can be
quadratic, native constructors/graphs/callbacks have no hard deadline, and CPU/RSS/
latency remain unmeasured. C++/Qt/MOC/ABI, actual PCM/hardware and real-provider
complete musical production acceptance remain unverified.

Source diagnostics pass across 457 changed/new C++ files and 158 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute knife editing or prove complete DAW acceptance.

### Native step cloning and ordering continuation (2026-10-14)

The previous turn made source progress on checked native knife editing. Current
inspection found that cloneSteps doubled m_steps before editing and copied only
key/length/pan/volume into default step notes, omitting detuning/curves/controllers
and linked topology. This continuation prepares complete native step copies and
exposes step.clone. It also corrects an ordering invariant in previously prepared
note edits. The complete DAW commission remains active and unproven. No native
build, fixture, GUI/plugin/DSP, actual audio/hardware or provider execution occurred.

Note.lessThan orders by position ascending, then key descending. The projected
shift/cut sorts had only used position. Both now retain the full native order;
transposition sorts use proposed keys before writing. Exact key/position ties stay
stable. New shift and vertical-split cases insert a further native note afterward,
checking the required sorted invariant and the resulting order. Shift coverage is
now eighteen cases / 56 scenarios and split coverage eighteen cases / 67 scenarios.
The same-position tie fixture now uses equal keys. Large varied-key split/knife
setup now retains a link anchor because inserting a higher key at the same position
changes the roster's first pointer; the knife's expected first key's cut length
was corrected. These fixtures remain uncompiled/unrun.

Checked MidiClip.cloneSteps snapshots count, meter and the full native roster's
lifetime/scalar state. It chooses the first Step note on each original grid point
in ascending source-step order. Widened inversion/placement uses the exact native
floor grid rather than an assumed constant offset, including nonintegral tick/step
ratios. Regular, off-grid, outside-prefix and extra duplicate originals are retained
without duplication. Existing mapped destination Step notes refuse before copying;
this replaces the old loop's possible scalar overwrite. Count doubling, native
arithmetic-product, song-span and rounded-bar bounds are admitted before writing.

Complete right/copy construction uses the native selected-note transfer scope:
resident curve content stays in constructors and controls-only metadata restores
independent closed groups on fresh receivers. Owned MIDI controllers copy
independently; rack references retain native ordinal meaning. Note key/type/length/
volume/pan and stored curve timing are retained; new copies are unselected as in the
old default step slot, while originals retain identities/selection. Copy closure
requires every linked member among the chosen first-per-grid sources, so connections
into omitted notes or other rows can refuse. No curve resampling is added.

Private transfer restoration now accepts a target step count and an optional
admission validator. It rechecks source count/meter/roster across construction and
checkpoint serialization. Prepared insertion permits an empty clone batch with a
count update and commits m_steps and the sorted ownership roster under one track
lock after the complete enabled clip checkpoint. Ordinary clipboard/copy defaults
retain their previous nonempty and selected-copy behavior. All selected original
lifetimes, copied lifetimes, final count/meter and expected note roster size are
checked after returned notifications. Failure returns an empty result; post-commit
native failure can retain edits/checkpoint. A complete native rebuild and transfer
regression execution remain required.

The existing zero-argument cloneSteps slot delegates to the checked API and logs
refusals; existing clip-menu and pattern-editor callers use it without GUI automation.
Native GUI bounds follow song/arithmetic limits; structured step.clone limits the
target to 4096 within core admission. It takes track/clip and optional container,
uses current row content after earlier edits, and returns fromCount/count/copied
plus source-step-order sourceNote/sourceStep and current step/note/position/key/
length receipts. Track/clip queued provenance detects replaced owners while allowing
step.set then step.clone in one batch. The pattern editor's multi-row loop remains
sequential and has no new whole-pattern transaction.

Both Chat and Responses declare step_clone individually and retain studio_execute
routing. Fifteen protected declarations, 159 native operations and ten supplemental
agent operations fit the existing 128-declaration inference budget. Prompt/schema/
protocol guidance documents current-row behavior, copy closure, collision refusal,
unselected copies, empty rows and receipt/index semantics.

StudioStepCloneTest is registered with a 300-second timeout. Fifteen cases / 47
scenarios author complete independent note/control/curve/controller copying,
first-Step/duplicate/regular/off-grid behavior, a nonintegral grid, empty and
regular-only row history, eleven count/meter/arithmetic/collision/copy/history
boundaries, legacy native count/graph undo/redo, seven ownership/recording/cancel/
retirement/count/meter changes, seven malformed structured requests, song/pattern
receipts, durable session replay/undo/redo/reopening, three returned notification/
original/copy retirement recovery cases, four scripted step-set/clone/future-copy
routes, four replaced-row replan routes, 1050 cloned notes and schema/tool budget.
All are authored, uncompiled/unrun; scripted inference proves no real musical
reasoning/listening/revision/production.

Full-content copies, collision refusal, count/roster publication and batched native
notifications differ from the former scalar loop and need GUI/plugin/legacy/DSP
compatibility evidence. Enabled clip history needs closed whole-clip coverage;
history can recreate note/helper identities and XML precision remains native.
Preparation/factory/callback/cleanup effects retain ordinary ownership limits and
post-commit failures can leave edits/checkpoint. Structured whole-project recovery
can invalidate handles. Arbitrary self/callee/parent/plugin deletion, reentry,
concurrent mutation, throwing cleanup and external effects remain unsupported. Full
rosters/tokens/grid maps/control XML/clones remain resident; membership/revalidation
can be quadratic, native constructors/serialization/callbacks have no hard deadline
and CPU/RSS/latency remain unmeasured. C++/Qt/MOC/ABI, actual PCM/hardware and real-
provider complete musical production acceptance remain unverified.

Source diagnostics pass across 458 changed/new C++ files and 159 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute step cloning or prove complete DAW acceptance.

### Native step resizing continuation (2026-10-14)

The previous turn made source progress on checked step cloning. Current inspection
found that step.resize still called addSteps/removeSteps in per-bar loops, with
intermediate ownership/length/data notifications, no native clip checkpoint and
unchecked meter use before modulo. This continuation replaces that path with one
native row edit and connects GUI add/remove slots to the same implementation. The
complete DAW commission remains active and unproven. No native build, fixture,
GUI/plugin/DSP, audio/hardware or real-provider execution occurred.

Checked MidiClip.resizeSteps prepares a retained roster and removed ownership list
before any write. Growth changes only the count. Shrink removes all Step notes at
exact removed grid cells in [target, oldCount), including duplicates; regular notes,
off-grid Step notes and outside-old-prefix notes retain identities, selection and
full content. This matches the old setStep(false) all-duplicates rule. Widened exact
floor-grid inversion avoids scanning each cell or assuming an affine offset on
nonintegral grids. Native positive counts are supported; GUI add/remove use current
one-bar deltas and preserve the at-most-one-bar remove no-op.

Owner/idle-recording admission, source/target positive counts, meter, optional target
limit, signed integer multiplication, song-span and rounded-bar lengths are checked.
Retained regular-note ends and one-bar pattern Step extents are also bounded before
updateLength. Full roster pointer/lifetime and scalar snapshots, count and meter
are revalidated after cooperative work and complete enabled clip checkpointing.
All ownership capacity is prepared before the checkpoint. Count and retained roster
publish under one track lock, then removed notes destruct with normal native link
and controller cleanup, followed by native type/length/data/modified notifications.
Returned count/meter, retained lifetimes and roster size are checked. Failure keeps
the result empty; native post-commit failure can retain edits/checkpoint. Equal-count
native no-ops avoid history and notifications. Structured session mutation/revision
behavior remains ordinary.

Structured step.resize retains whole-native-bar admission and a 4096 target maximum,
now rejecting invalid meter state before modulo. It returns fromCount/count/removed/
changed and resolves current row contents after earlier edits. Track/clip provenance
detects replaced owners, while subsequent queued note targets removed by shrink are
skipped through existing note identity provenance and replan. Both Chat and Responses
declare step_resize individually and retain studio_execute catalog routing. Sixteen
protected native declarations, 159 native operations and ten supplemental agent
operations fit the 128-declaration provider limit. Prompt/schema/protocol guidance
documents growth, removed-cell semantics and note-index rediscovery.

StudioStepResizeTest is registered with a 300-second timeout. Seventeen cases / 54
scenarios author all-duplicate grid removal/retained identity/linked cleanup and one
complete notification, empty extension, native no-op history, two native count/graph
undo-redo paths, legacy slots, nonintegral grid behavior, eleven native bounds/history
refusals, seven recording/owner/cancel/retirement/meter/scalar admission changes, nine
structured bounds/owner refusals, song/pattern receipts, durable replay/undo/redo/
reopening, three returned failure recovery paths, twelve scripted Chat/Responses
direct/catalog current-row/stale-row/stale-note routes, 1050 linked-note shrink and
schema/declaration bounds. All remain uncompiled/unrun. Scripted inference proves
no real-provider musical reasoning, listening, revision or complete production.

The source verifier initially failed on its scenario inventory arithmetic: its
shared protocol-row helper needed eight additional expansions for three data
methods. Correcting the accounting produced 54 authored scenarios. This was a
source-verifier failure, not execution evidence about native resizing.

New checkpoint/notification/modified behavior in existing GUI add/remove slots
requires GUI/legacy/plugin/DSP compatibility execution. Enabled native history
requires complete closed whole-clip control coverage, and restoration can recreate
note/helper identities. Pattern-editor multiple-row loops remain sequential without
a whole-pattern transaction. Native direct post-commit failures can retain edits;
structured whole-project restoration can invalidate handles. Arbitrary self/callee/
parent/plugin deletion, reentry, concurrent mutation, throwing cleanup and external
effects retain existing unsupported limits. Full rosters/snapshots remain resident;
native linked cleanup can be quadratic, and constructors/serialization/callbacks/
destruction have no hard deadline or measured CPU/RSS/latency. C++/Qt/MOC/ABI, actual
PCM/hardware, existing-project/plugin compatibility and real-provider complete
musical production acceptance remain unverified.

Source diagnostics pass across 459 changed/new C++ files and 159 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute step resizing or prove complete DAW acceptance.

### Native step toggling continuation (2026-10-14)

The previous turn made source progress on checked native step resizing. Current
inspection found that step.set still used unchecked default insertion or repeated
per-note deletion with intermediate notifications, while GUI clicks had separate
enable/disable paths and a manual checkpoint only on disable. This continuation
shares the prepared row-edit kernel with toggling and routes GUI click/wheel
creation through checked native editing. The complete DAW commission remains active
and unproven. No native build, fixture, GUI/plugin/DSP, audio/hardware or actual
provider execution occurred.

MidiClip.resizeSteps now delegates to private editSteps with typed options. Checked
MidiClip.setStep uses the same full scalar/lifetime/count/meter snapshots, idle
registered owner admission, widened arithmetic/span/rounded-length bounds, complete
enabled clip checkpoint and track-locked prepared publication. Resize behavior and
its seventeen cases / 54 scenarios remain authored and uncompiled/unrun.

Enabling a cell preserves the first existing Step note in native roster order and
leaves all existing duplicates/content intact. If none exists, a default unselected
native Step is prepared at the exact widened floor-grid position, with the existing
default key/volume/pan, DefaultTicksPerBar/16 length and no detuning helper. Its full
combined pointer roster sorts by native position/descending key before checkpoint
publication. Disabling prepares all Step duplicates at the cell for removal while
retaining regular/off-grid content and the unchanged row count. Removed notes
destruct normally and detach control links before one native data notification.

The shared kernel prepares ownership capacity before the checkpoint, revalidates
after cooperative work and checkpoint serialization, and commits count/roster under
one track lock. Returned enabled/retained identities, enabled cell geometry, meter,
count and expected roster size precede result success. Errors clear result output;
native post-commit failures can retain edits/checkpoint. Existing enabled or absent
disabled cells return native no-ops without checkpoint/notification. Structured
mutations retain ordinary session revision behavior and broader whole-project
recovery, which can invalidate prior handles.

Legacy setStep delegates to the checked entry. GUI clicks now use that entry for
both directions and no longer create a second manual checkpoint; wheel creation
uses checked enable before its existing volume adjustment. Both UI event paths
reject negative cells before native lookup. Low-level addStepNote and existing-note
wheel volume editing retain their previous paths. This changes GUI history,
notification and modified behavior and requires actual GUI/legacy/plugin/DSP
compatibility evidence. Idle model ownership is required; toggling during native
project loading or note recording is refused.

Structured step.set limits the current row to 4096 steps inside native admission,
avoids subtracting from an unchecked old count to establish the cell bound, and
returns step/enabled/changed/removed plus current note/position when enabled. Row
contents and bounds are resolved at execution, allowing an earlier resize followed
by setting a new cell in the same batch. Existing track/clip owner provenance skips
replaced rows; queued note identities removed by disabling trigger skip/replan.
Both provider protocols individually declare step_set and retain studio_execute.
Seventeen protected native declarations, 159 native operations and ten supplemental
agent operations fit the 128-declaration budget. Prompt/schema/protocol guidance
documents current-row semantics, all-duplicate removal and index rediscovery.

StudioStepSetTest is registered with a 300-second timeout. Eighteen cases / 60
scenarios author default creation/one publication, existing-first duplicate content,
all-duplicate removal and retained graph/identity, two native no-op and two undo/redo
paths, legacy entry, nonintegral floor grid, twelve native bounds/history refusals,
eight recording/owner/cancel/retirement/meter/scalar/count admission changes, nine
malformed structured requests, song/pattern receipts, two durable replay/history/
reopening paths, four returned insertion/removal/retirement failures, twelve scripted
Chat/Responses direct/catalog future-cell/stale-row/stale-note routes, 1050 duplicate
removals and schema/declaration bounds. All remain uncompiled/unrun; scripted
inference proves no real musical reasoning, listening, evaluation or production.

Enabled native clip history requires complete closed whole-clip serializer coverage;
native/structured restoration can recreate note/helper identities. Existing shared
limits on arbitrary self/callee/parent/plugin deletion, reentry, concurrent mutation,
throwing cleanup and external side effects remain. Full rosters/scalars/lifetimes
and prepared pointer sorting remain resident; native linked cleanup can be quadratic,
and serialization/callbacks/destruction have no hard deadline or measured CPU/RSS/
latency. C++/Qt/MOC/ABI, actual PCM/hardware, existing-project/plugin compatibility
and real-provider complete musical production acceptance remain unverified.

Source diagnostics pass across 460 changed/new C++ files and 159 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute step toggling or prove complete DAW acceptance.

### Native selected-note removal continuation (2026-10-14)

The previous turn made source progress on checked native step toggling. Current
inspection found repeated per-note removal in selected cut/delete, a raw structured
note.remove path with a second update/notification, and raw note.clear without one
prepared native edit. This continuation adds checked batch removal and exposes
note.selection.remove. The complete DAW commission remains active and unproven.
No native build, fixture, GUI/plugin/DSP, audio/hardware or actual-provider execution
occurred.

MidiClip.removeNotes snapshots the input selection before any roster mutation,
including when the selection aliases m_notes. Registered idle owner admission,
missing/duplicate roster ownership and null/duplicate/foreign selected notes are
checked. Full scalar/lifetime/count/meter snapshots are revalidated after cooperative
checks and a complete enabled clip checkpoint. Retained pointer and removed ownership
capacity are prepared before history. One track-locked roster swap transfers removed
ownership to local cleanup; normal destruction detaches controls before native
type/length/data/modified notifications. Remaining note identities, selection and
control/curve content stay in place, with step count and clip metadata retained.
Post-notification roster size, count/meter and surviving identities precede the
returned removed count. Failure leaves removed zero; post-commit native failures can
retain edits/checkpoint and structured whole-project recovery can invalidate handles.

Empty native selections/clears avoid history and notification. Nonempty edits require
valid native count/meter, arithmetic products and rounded length paths. Selected
malformed geometry can be deleted; malformed retained geometry refuses before writes.
The edit does not require closed selected control groups for deletion: normal shared
ownership can detach deleted controls from surviving external peers, while helpers
retained by another native owner can outlive a deleted note. Enabled native history
still requires complete closed whole-clip serializer coverage and can refuse groups
spanning other clips. This differs from structured whole-project snapshot coverage.

The checked clearNotes overload uses current m_notes. Legacy GUI MidiClip.clear now
delegates without a separate manual checkpoint; raw zero-argument clearNotes remains
for project loading/internal lifecycles. Piano-roll selected delete and cut delegate
to removeNotes instead of repeated removeNote, retaining clipboard publication and
explicit selected lifetime checks before cut removal. A clipboard can remain copied
when deletion refuses. Low-level removeNote and mouse erase gestures retain their
prior paths. New GUI history/publication/modified behavior requires actual GUI,
existing-project/plugin and DSP compatibility checks.

Structured note.selection.remove requires 1..4096 distinct current note indices from
one MIDI clip, verifies indexed lifetimes during admission, and returns removed plus
request-order original sourceNote receipts without dereferencing retired notes.
Existing note.remove and note.clear also use checked native removal and return a
removed count. Whole clear resolves current contents after earlier batch edits.
Existing weak owner/per-note queued provenance skips moved/replaced identities while
allowing future notes created earlier in the batch. Both Chat and Responses declare
note_selection_remove individually and retain studio_execute routing. Eighteen
protected declarations, 160 native operations and ten supplemental agent operations
fit the 128-declaration budget. Prompt/schema/protocol guidance documents invalidated
indices, current clear semantics and ordinary structured mutation revisions.

StudioNoteSelectionRemoveTest is registered with a 300-second timeout. Seventeen
cases / 53 scenarios author survivor identity/control/curve content and outside-peer
cleanup with one complete notification, deletion of selected malformed geometry,
aliased complete clear/metadata/count, empty native history, five invalid/coverage
refusals, eight owner/recording/cancel/retirement/meter/scalar/count changes, three
native undo/redo and legacy clear paths, ten malformed structured requests, song/
pattern request-order receipts, two existing single/whole-clip interfaces, two durable
replay/history/reopening paths, three returned notification/survivor/count failures,
twelve scripted Chat/Responses direct/catalog future-note/current-clear/stale-index/
stale-owner routes, 1050 linked removals and schema/tool bounds. All remain
uncompiled/unrun. Scripted inference proves no real-provider musical reasoning,
listening, evaluation, revision or complete production.

Full roster/scalar/lifetime snapshots and membership sets remain resident; linked
destruction can be quadratic. Native serialization/callbacks/destruction have no hard
deadline or measured CPU/RSS/latency. Existing limits on arbitrary self/callee/parent/
plugin deletion, reentry, concurrent mutation, throwing cleanup and external effects
remain. Native/structured history can recreate note/helper identities. C++/Qt/MOC/
ABI, actual PCM/hardware, existing-project/plugin compatibility and real-provider
complete musical production acceptance remain unverified.

Source diagnostics pass across 461 changed/new C++ files and 160 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute note removal or prove complete DAW acceptance.

### Native note property-edit continuation (2026-10-14)

The previous turn made source progress on checked batch note removal. Current
inspection found that note.edit wrote position/length before parsing later key,
volume and panning fields, so a direct invalid request could leave partial edits.
It also had no checked native property transaction/clip checkpoint and the piano-roll
value dialog set fields in loops. This continuation adds checked native property
editing, exposes note.selection.edit and shares it with existing single-note edits
and the GUI numeric value dialog. The complete DAW commission remains active and
unproven. No native build, fixture, GUI/plugin/DSP, audio/hardware or actual-provider
execution occurred.

MidiClip.NoteEdit contains optional absolute position/length/key/volume/panning.
MidiClip.editNotes applies supplied fields uniformly through the shared native
selection kernel, preserving omitted fields, identities, native type/selection and
inline curve/control content. Stored curve timing remains unchanged. Native signed/
zero lengths are supported; structured explicit lengths retain their existing
positive bound. Structured volume/pan accept bounded numbers, then truncate toward
zero into native integer fields after admitting the original full numeric value.
Both single and batch structured entries parse all supplied fields before mutation.

Property edits require idle registered model ownership, positive count/meter,
representable native step multiplication/song span, final selected endpoints,
retained geometry and rounded length paths. Supplied fields can repair malformed
selected geometry when the final state is admitted. All planned values and complete
projected position/descending-key order precede the enabled full clip checkpoint.
One track lock publishes fields and the sorted roster, followed by normal native
length/data/modified notifications. Selected scalar changes, selected/unselected
retirement, count/meter and roster-size changes after notifications refuse success.
Failure leaves the changed result false; native post-commit failure can retain
fields/checkpoint. Structured whole-project recovery can recreate identities and
invalidate handles. Native empty/equal edits avoid checkpoints and notifications;
structured mutations retain ordinary revision semantics.

Shared transform roster snapshots now include every note's identity, position, key,
length, volume, pan, type and selection, replacing the former position-only roster
snapshot. Admission checks this full state and idle note recording across cooperative
work/checkpoint serialization. An unselected key could previously change after
prepared sorting without invalidating admission; new coverage changes a same-tick
unselected key after sorting and requires rejection before selected fields publish.
This expands admission for shift/quantize/reverse/length as well as new property
edits and needs their native regression execution.

Structured note.selection.edit requires 1..4096 distinct current indices from one
MIDI clip. It returns changed and request-order sourceNote/current note/scalar
receipts; existing note.edit returns one corresponding receipt. Absolute position
can intentionally place multiple notes at the same tick, while existing shift
remains the relative operation. Weak owner/note provenance skips stale indexed
identities and allows missing future notes created earlier in a batch. Current
indices must be rediscovered after ordering changes. Chat and Responses declare
both note_edit and note_selection_edit individually and retain catalog routing.
Twenty protected native declarations, 161 native operations and ten supplemental
agent operations fit the 128-declaration limit. Prompt/schema/protocol guidance
documents uniform absolute fields, numeric conversion, no-ops and receipt semantics.

Piano-roll numeric velocity/panning dialogs now capture weak clip and note lifetime
state before opening, validate it after the modal returns, and call editNotes for
the complete chosen note list. Empty inputs refuse before dereferencing the first
note; last-used numeric defaults update only after success. Other low-level drag,
scribble and wheel editing retain their previous paths. New dialog history,
notification/modified behavior and broader transform admission require actual GUI,
existing-project/plugin and DSP compatibility evidence.

StudioNoteSelectionEditTest is registered with a 300-second timeout. Eighteen cases /
65 scenarios author all-field one-publication/identity/type/selection/control
retention, three omitted signed/zero/type paths, native key ordering plus later
insertion, three native no-op paths, eleven bounds/history/later-note refusals,
selected malformed geometry repair, nine owner/recording/cancel/retirement/meter/
scalar/count changes including post-sort unselected-key mutation, two native history
paths, eleven malformed structured calls including partial-write regressions, song/
pattern request-order receipts and native fraction conversion, existing single edit,
two durable history/replay/reopening paths, four returned notification/selected/
unselected/scalar failures, twelve scripted Chat/Responses direct/catalog future-
note/stale-index/stale-owner routes, 1050 linked-note edits and schema/declaration
bounds. All remain uncompiled/unrun. Scripted inference proves no actual provider
musical reasoning, listening, evaluation, revision or complete production.

Enabled native clip history requires complete closed whole-clip serializer coverage.
Full rosters/scalars/lifetimes/maps and prepared sorting remain resident; repeated
membership/revalidation can be quadratic. Native virtual setters/serialization/
callbacks have no hard deadline or measured CPU/RSS/latency. Existing limits on
arbitrary self/callee/parent/plugin deletion, reentry, concurrent mutation, throwing
cleanup and external side effects remain. Native/structured restoration can recreate
note/helper identities. C++/Qt/MOC/ABI, actual PCM/hardware, existing-project/plugin
compatibility and real-provider complete musical production remain unverified.

Source diagnostics pass across 462 changed/new C++ files and 161 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute property editing or prove complete DAW acceptance.

### Native note-batch creation and schema continuation (2026-10-14)

The previous turn made source progress on checked note property editing. Current
inspection found that note.add still inserted through raw single-note addNote,
without a checked complete clip checkpoint, and no native structured new-note batch
was exposed. This continuation adds checked native scalar-note construction and
note.batch.add, and routes the existing single-note operation through the same core.
The complete DAW commission remains active and unproven. No native build, fixture,
GUI/plugin/DSP, audio/hardware or actual-provider execution occurred.

MidiClip.NoteSpec carries absolute position/length/key/volume/panning and native type.
MidiClip.addNotes snapshots the input values and full current roster scalar/lifetime
state, admits registered idle model ownership, missing/duplicate roster ownership,
result index capacity, positive count/meter, native integer-product and rounded
length paths, and every new/retained endpoint before native construction. Native
callers can use bounded signed/zero lengths and Step type. New notes are native
unselected Notes with no initial detuning/control helpers; existing content is never
converted into scalar input. Existing curve/control copying remains NativeNoteTransfer.

Fresh ownership/identity and request-order output capacity are prepared under
cooperative checks and full original revalidation. The existing appendPreparedNotes
sorts the complete roster by native position/descending key before one enabled full
clip checkpoint, validates through serialization and publishes under one track lock.
Existing notes precede new exact key/position ties and new exact ties keep input
order. Ordinary native type/length/data/modified notifications follow. Original
identities/scalars/selection, new identities/scalars/type, count/meter and roster size
are checked afterward. Failure leaves output empty; native post-commit failure can
retain edits/checkpoint, while structured whole-project recovery can invalidate
handles. Native empty input avoids history/notification and clears output.

Structured note.batch.add requires 1..4096 strict nested items with position,
positive length and key; optional volume/panning use native defaults and bounded
integer truncation. All inputs are parsed before native publication. Unknown nested
fields refuse. Duplicate inputs create independent notes. Added/current index/scalar
receipts follow input order, and step count remains unchanged. Existing note.add
uses this core and retains its original single note-index result with no GUI
quantization. Weak owner provenance skips replaced receivers while allowing earlier
current-row changes and missing future note targets. Both provider protocols declare
note_add and note_batch_add individually and retain catalog routing: 22 protected
declarations, 162 native operations and ten supplemental agent operations fit the
128-declaration limit. Prompt/schema/protocol guidance documents explicit inputs,
new-note defaults, preserved resident content and index rediscovery.

Inspection also found a concrete schema defect in the preceding selected removal
and property-edit continuations. Their dispatchers expected notes arrays, but the
generated schemas still assigned the generic integer type because those operations
were absent from the selection-schema condition. This would reject their normal
array requests before dispatch. Both now receive bounded distinct-index array
schemas and property-edit fields receive explicit native bounds. Existing removal
and edit fixtures now assert array type/bounds/uniqueness; source validation checks
that every note.selection operation is included in the shared array schema. Earlier
source checks did not detect the defect; no native execution was ever claimed.

StudioNoteBatchAddTest is registered with a 300-second timeout. Seventeen cases / 62
scenarios author input-order outputs and complete one-publication original control/
controller/curve/selection retention, duplicate identities/native tie order, three
native signed/Step paths, empty native history, twelve new/retained/coverage bounds,
eight recording/owner/cancel/retirement/meter/scalar/count changes, native undo/redo,
twelve malformed nested requests, song/pattern receipts and numeric conversion,
original single receipt/native history, two durable replay/history/reopening paths,
four returned notification/new/old/scalar failures, twelve scripted Chat/Responses
direct/catalog future-note/current-row/stale-owner routes, 1050 insertions and nested
schema/declaration bounds. All remain uncompiled/unrun. Scripted inference proves
no actual provider musical reasoning, listening, evaluation or complete production.

Two older source-verifier assertions initially failed because their unscoped first
append/return searches now matched the new constructor instead of transfer restore.
Both searches now scope the intended NativeNoteTransfer.restore function. These were
source-verifier scope failures, not native execution results.

Raw addNote and existing GUI/recording low-level paths retain their previous behavior.
New single-note history/publication/modified behavior requires GUI, existing-project,
plugin and DSP compatibility execution. Enabled native history requires closed full
clip serializer coverage and can refuse external linked groups. Full inputs/rosters/
scalars/lifetimes/allocations and sorting remain resident; repeated validation can be
quadratic. Native construction/serialization/callbacks have no hard deadline or
measured CPU/RSS/latency. Existing arbitrary self/callee/parent/plugin deletion,
reentry, concurrent mutation, throwing cleanup and external-effect limits remain.
Native/structured restoration can recreate note/helper identities. C++/Qt/MOC/ABI,
actual PCM/hardware, existing-project/plugin compatibility and real-provider complete
musical production remain unverified.

Source diagnostics pass across 463 changed/new C++ files and 162 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute batch creation or prove complete DAW acceptance.

### Capability schema-preflight continuation (2026-10-14)

The previous turn made source progress on checked batch note creation and corrected
two generated selection-array schema defects. Current catalog inspection found a
broader boundary gap: execute checked top-level primitive types/required/unknown
keys but did not enforce published nested schemas, enums, numeric/string bounds,
patterns or uniqueness. Song.set also applied tempo before parsing later master
fields. This continuation adds recursive published-schema preflight before native
dispatch and pre-parses all supplied song settings. The full DAW commission remains
active and unproven. No native build, fixture, GUI/plugin/DSP, actual audio/hardware
or real-provider execution occurred.

StudioCapabilities.validateArguments validates operation arguments against the same
inputSchema emitted by studio.capabilities, without accessing native models. It
checks supported primitive types, finite numbers/integral integers, enums, inclusive
numeric bounds, Unicode-code-point string lengths/patterns, required/additional
object keys including typed plugin attributes, array sizes and item schemas, and
declared uniqueness. Index-array uniqueness uses a numeric set, including equivalent
zero representations; the generic noninteger fallback uses pairwise JSON equality.
Unsupported schema types refuse. Errors carry args property/index paths. Arguments
are read-only and schema defaults are not inserted into caller objects.

FileResourceIO cancellation is checked before catalog construction, every 256 visited
values/comparisons and on completion. EventSuspension prevents event pumping through
outer polling scopes. No new global graph/note size limit replaces existing published
or native bounds. Catalog construction, QJson copies, long strings and regular
expressions are not preempted. Native state/owner/plugin/transport/cross-field
validation remains in native operations and passing schema validation proves none
of those runtime conditions.

Execute invokes this preflight before all operation dispatch paths. Generated
schemas now state actual song tempo/master volume/master pitch bounds, native owner
enums, native track creation types, nonempty names for the existing native-required
name paths and string-valued plugin attribute dictionaries. Song.set parses every
supplied field before any setter even apart from schema enforcement. Direct valid
tempo followed by invalid master settings can no longer publish the earlier tempo.
Invalid empty track/clip creation names and late invalid clip-edit names also refuse
before native creation/movement; controller/mixer names retain their prior native
admission with earlier diagnostics. No new native operation was added. The protocol's
early catalog overview was corrected from stale 122/eight/119 counts to the current
162 native operations, ten agent tools and 117 native declaration slots plus catalog
routing within the 128-tool budget; 22 native declarations remain protected.

StudioCapabilitySchemaTest is registered with a 120-second timeout and authors ten
cases / 46 scenarios. One case builds representative arguments for every current
published schema, including all optional fields, and calls preflight without an
initialized Engine. It checks catalog shape/required consistency, not native model
reachability. Other coverage includes nine required/unknown/primitive/nonfinite
failures, ten enum/bound failures, eight nested objects/arrays/attribute failures,
five index-array failures, seven specialized valid shapes, input/default preservation,
three digest failures, cancellable 4096-index traversal without event pumping, and
explicit cases where state/cross-field admission still belongs to native execution.
All tests remain uncompiled/unrun.

StudioCapabilityPreflightTest is registered with a 300-second timeout and authors
seven cases / 15 scenarios: three late song-field refusals with unchanged native
state and successful valid settings, four creation-name refusals with unchanged
object counts, late invalid clip name without movement/resize, nested attributes
before plugin lookup, durable cached failure with preserved song state, four scripted
Chat/Responses direct/catalog correction routes and cancelled note creation. These
are authored, uncompiled/unrun. Scripted inference proves no actual musical reasoning,
listening/evaluation/revision or complete production.

Broader published constraint enforcement changes rejection timing, cancellation
timing and diagnostic strings, requiring full catalog, native/GUI/plugin/legacy
regression execution. It does not turn native callbacks/setters/external effects
into transactions or change existing recovery boundaries. Full schema construction,
argument QJson storage, uniqueness sets and recursive validation remain resident;
future noninteger uniqueness can be quadratic. There is no measured CPU/RSS/latency,
native C++/Qt/MOC/ABI, actual PCM/hardware or real-provider complete musical production
evidence. Full DAW acceptance remains unverified.

Source diagnostics pass across 465 changed/new C++ files and 162 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute schema preflight or prove complete DAW acceptance.

### Checked native clip-property continuation (2026-10-14)

The previous turn made source progress on recursive capability preflight. Current
inspection found that clip.edit still applied multiple direct native setters without
a compound native checkpoint or owner checks across their notifications. This
continuation adds Clip.EditOptions and checked Clip.edit, implemented in
NativeClipEdit.cpp, and routes the existing structured operation through it. The
full DAW commission remains active and unproven. No native build, fixture, GUI/plugin/
DSP, actual PCM/hardware or real-provider execution occurred.

The native core parses/copies optional position/length/signed offset/automatic
sizing/name/stored-mute values, admits registered clip/track ownership on the model
thread and idle MIDI/sample/automation recording through the shared native recording
guard, and checks requested manual bounds before writing. PatternStore cell positions
remain owned by PatternStore, including an explicit same-position request. Manual
length/offset disables automatic sizing. Existing zero-length clips can retain their
length when not explicitly resizing, preserving metadata editing of empty patterns.
Explicit manual lengths remain positive.

An explicit automatic-sizing enable is separate from manual length/offset. It invokes
the native subtype's updateLength behavior, including native offset resets, and is an
applied refresh even if visible fields match. Empty/equal edits without refresh avoid
history and notifications. Automatic sizing is delegated rather than reconstructed;
the core verifies final geometry afterward, and does not prevalidate every nested
note/sample/automation state or replace native content validation.

Relevant original clip properties are snapshotted and revalidated after cooperative
work and complete enabled clip checkpoint serialization. Offset/automatic sizing
require a valid audio clock and automatic sizing requires a positive meter; clock/
meter dependencies are rechecked across admission and setter callbacks. Native virtual
move/length/offset/update/name/mute functions remain authoritative. Owner/recording
checks follow each setter. Stored mute readback, rather than controller-driven live
value, is checked, retaining native controller precedence. Omitted name/mute and
manual geometry remain checked against their expected final state.

One enabled complete clip checkpoint is published before setter application, requiring
closed whole-clip control serializer coverage. Nested global native journalling is
temporarily disabled during compound setters and restored with a scope guard, avoiding
separate partial BoolModel checkpoints in normal execution. Native intermediate
setter/global notifications remain observable. This is not an atomic multi-signal
transaction: callbacks can observe partial states and callback-generated side effects
or history retain existing limits. Native post-commit failure can retain fields and
checkpoint; StudioRuntime supplies the broader whole-project recovery boundary and
may recreate handles. Final owner/properties/bounds are checked before and after
final data/modified notifications.

Structured clip.edit parses all supplied fields then delegates to this API and adds
changed to its existing geometry/automatic receipts. Its schema now publishes actual
position/positive manual length/signed offset bounds; prompt/protocol guidance records
manual freeze, separate automatic refresh, recording and PatternStore ownership.
There are still 162 native operations, ten supplemental agent operations and 22
protected declarations within the existing 128-tool budget. Raw native setters and
existing GUI drag paths retain their previous behavior, avoiding per-mouse-move
compound history changes.

StudioClipEditTest is registered with a 300-second timeout. Sixteen cases / 58
scenarios author native fields/identity/content across MIDI/sample/automation/pattern
clips, sample frame offset and later tempo change, two native no-op paths, existing
zero-length pattern metadata, explicit automatic refresh, four complete native undo/
redo paths including linked mute/solo controls and journal scope, ten native bounds/
PatternStore/coverage refusals, nine recording/owner/cancel/retirement/property/meter/
clock changes, nine malformed structured requests, song/PatternStore receipts, durable
history/replay/reopening, four returned notification/retirement/length/mute failures,
direct failure with restored journal flag and one undo, eight scripted Chat/Responses
direct/catalog future/stale-owner routes and schema/declaration bounds. All remain
uncompiled/unrun. Scripted inference proves no actual-provider musical reasoning,
listening, evaluation, revision or complete production.

Native setter/notification/history behavior needs GUI/legacy/project/plugin/DSP
compatibility execution. Complete clip checkpoints retain native XML precision and
can recreate nested identities; external linked groups can refuse enabled native
history. Arbitrary self/callee/parent/global/plugin deletion, reentry, concurrent
mutation, throwing cleanup and external effects retain unsupported limits. Global
journal suppression can also suppress callback-generated entries during the compound
window. Serialization/virtual sizing/setters/callbacks are not preempted and no
CPU/RSS/latency or C++/Qt/MOC/ABI execution is established. Actual PCM/hardware and
real-provider complete musical production remain unverified.

Source diagnostics pass across 467 changed/new C++ files and 162 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Build tools and an LMMS executable remain absent from
PATH; all 17 pinned submodules remain uninitialized. These source checks do not
compile or execute clip property editing or prove complete DAW acceptance.


Native track solo ownership continuation, 2026-10-14

Source review found TrackView owned the only connection between the track solo
model and Track::toggleSolo. A headless track.edit could change the flag without
applying the native mute policy. Track now owns that DirectConnection, and the
view no longer installs it. Native setters, automated writes and structured tools
therefore reach the same core policy without creating widgets. Track cloning now
normalizes its restored solo flag in core after all track/clip restoration; the
GUI clone handler no longer supplies a second transition.

The transition acquires the native engine change guard without imposing a new
application-thread restriction on native automated writes. It snapshots weak peer
identities, model mute values and solo flags before applying any peer setters.
The first selected solo saves every peer's pre-solo cache before notifications;
subsequent solo selection preserves those caches. A container flag suppresses
recursive transitions from internal peer solo clears and a weak scope guard
releases it on ordinary return/exception. Membership is checked around writes;
retired peers are skipped and a retired source stops the loop. Modern versus
legacy automation behavior and separate song/pattern containers are retained.
This uses allocations and repeated membership searches, with no measured real-time
or CPU/RSS/latency guarantee. Added tracks retain the existing native policy.

Track::loadTrack scopes a local restoration flag. Whole project loading and an
active thread-local NativeLinkedControls restoration scope also suppress command
effects, including model notifications issued after an individual track load has
finished. Serialized solo/mute/cache combinations stay authoritative, even when
they do not describe an isolated track. Portable preset commands normalize an
actual solo transition after their owned restore scope ends; ending solo temporarily
uses the old pre-solo cache so a previously muted target is restored correctly.
Clone selection is applied once after its own restoration has finished.

Structured track.edit snapshots weak owner/target handles, parses inputs before
writes and checks registration after move/name/mute/solo callbacks. Final row
receipts for moves are rediscovered from the current container. The protected
model work scope now includes track.edit. Native solo can unmute a target even
when the request also supplied muted=true; receipts report the native effective
value. Schemas and agent guidance describe these semantics without adding an
operation or changing the declaration budget.

StudioTrackSoloTest is registered with a 300-second timeout and authors 17 cases /
24 scenarios: two modern/legacy policy rows, two switching/cache rows, native
automated writes, pattern isolation, track and linked restoration, portable preset,
solo clone, peer/source retirement, exception guard cleanup, structured ordering
and effective receipt, durable undo/redo/replay/reopening, three returned failures,
cancellation, declaration budget and four scripted Chat/Responses direct/catalog
routes. All are uncompiled/unrun. Native per-control journal behavior is retained;
this continuation does not introduce or verify a single compound native solo undo.
Session-level history/recovery assertions also remain authored evidence only.

Native controller/link precedence can conflict with isolation, intermediate setter
signals remain observable, and the guard suppresses internal recursion rather than
providing an atomic transaction. Arbitrary external reentry, callee/parent/global/
plugin deletion, concurrent mutation, throwing cleanup and external effects retain
shared unsupported limits. GUI/legacy/project/plugin/DSP compatibility, Qt/MOC/ABI,
actual rendered PCM/audio hardware and a real provider's complete composition,
listening, evaluation, revision and preservation remain unverified.

Source diagnostics pass across 469 changed/new C++ files and 162 native operations;
Python syntax, JSON, fixture inventories/whitespace, final newlines and git diff
--check pass. Native build tools and executable remain unavailable on PATH and all
17 pinned submodules remain uninitialized. Source diagnostics do not establish
complete DAW acceptance. The full commission remains active and incomplete.


Native track removal and clone completion continuation, 2026-10-14

The prior turn made source progress by moving native solo effects into Track.
Follow-up inspection found TrackContainer::removeTrack held its nonrecursive
roster write lock while clearing solo. That setter now consistently invokes native
mute notifications in headless operation as well as GUI operation. A notification
that called countTracks could deadlock on the lock, while removing/moving peers
could invalidate the saved erase iterator. This was a concrete source defect;
no native deadlock reproduction or execution was obtained.

Native detachment now takes the engine model guard, captures weak owner/target
handles and checks membership under a scoped read lock. It releases that lock
before solo restoration, then acquires a scoped write lock and re-finds the exact
target before erasing. Native modification/removal notifications occur afterward.
If a nested detach already removed the target, the outer call returns without a
second erase or removal signal. Returned peer retirement and reorder no longer
reuse a stale iterator. Ordinary exceptions release scoped locks and the existing
solo transition guard, but can leave partial masks before the erase; this change
is not an atomic native transaction.

Structured track.remove previously entered delete immediately. Clearing solo
from inside a native destructor could then execute a throwing notification inside
a noexcept destruction boundary. It now runs the recoverable solo transition while
the complete track is alive, before delete, and revalidates weak registered owner/
target and recording admission after the transition. An uncleared effective solo
is refused. This reduces an identified pre-teardown failure path; it does not make
arbitrary track/plugin/destructor/cleanup callbacks exception-safe.

Track::requireIdleForRemoval is a native validation entry point using the engine
change guard. It refuses a track's active note recording or any owned sample/
automation recording state already covered by the native clip admission checker.
Pattern owners additionally require their native mapping, complete row counts and
idle note/clip recording across all affected pattern rows, including surviving
cells that native deletion repositions. It inspects existing rosters without lazy
getClip creation. Structured admission is checked before cooperative work and again
after work and solo notifications. Raw native teardown/loaders and existing GUI
deletion admission retain their prior contract.

Core clone completion now checks its weak owner and duplicate registration before
and after final solo selection and Song modification. An ordinary detached copy
is deleted by the existing weak cleanup guard before returning failure. Structured
track.clone rediscovers the actual duplicate row after native callbacks, rather
than returning the container size captured before cloning. Schemas and agent
instructions describe recording admission and receipt semantics. Track removal and
clone calls join the protected model-work scope without adding a native operation
or changing provider declaration limits.

StudioTrackLifecycleTest is registered with a 300-second timeout. Twelve cases /
25 authored scenarios cover container reads during solo restoration, peer removal,
nested detachment with one signal, callback-driven roster moves, ordinary exception
lock release, structured removal masks, seven note/sample/automation/pattern/sparse/
surviving-cell recording refusals, clone reorder receipt, native pattern survivor
identity/note content/current selection, whole-session undo/redo/replay/reopening,
four pre-teardown throw/source retirement/clone retirement/detachment recoveries,
four scripted Chat/Responses direct/catalog clone-and-remove routes and cancellation
of both structural operations. All are uncompiled/unrun; scripted routes are not
actual-provider musical production evidence.

Native per-control history behavior remains unchanged and no compound native
track-removal/clone undo is established. Session history is authored only. Conflicting
nested actions can interrupt solo restoration, and arbitrary external reentry,
callee/parent/global/plugin deletion, concurrent mutation, throwing cleanup and
external effects retain shared unsupported limits. Full native build/Qt/MOC/ABI,
GUI/legacy/project/plugin/DSP compatibility, actual PCM/audio hardware, CPU/RSS/
latency and real-provider interpretation/planning/composition/rendering/listening/
evaluation/revision/preservation remain unverified.

Source diagnostics pass across 470 changed/new C++ files and 162 native operations;
Python syntax, report JSON, fixture inventories/whitespace, final newlines and git
diff --check pass. Native tools and executable remain unavailable on PATH and all
17 pinned submodules remain uninitialized. Complete DAW acceptance is unproven;
the commission remains active.


Native pattern query and loop dispatch continuation, 2026-10-14

The previous turn made source progress on track removal and clone completion.
Tracing pattern-column operations exposed a separate concrete read-path defect.
PatternStore::lengthOfPattern accepted negative selectors in its less-than-count
check, then passed them to Track::getClip, a lazy accessor that creates an absent
cell. Explicit-column automation similarly asserted cell presence in debug builds
and called the lazy accessor in release builds. These queries could therefore
change sparse/native loaded state while inspecting or evaluating it. No native
failure reproduction or execution was obtained.

Length queries now check selectors, read existing MIDI cells and retain the native
one-bar fallback for absent/empty patterns. Nonpositive meter returns zero. Rounded
bar calculation uses qint64, so the maximum native stored tick length no longer
adds a bar-rounding term in signed 32-bit arithmetic. Metadata bar counts may still
describe a rounded tick extent that cannot fit a native tick. Checked playback and
automation consumers validate widened products and refuse that extent rather than
performing overflowing arithmetic or modulo by zero. This does not comprehensively
harden every legacy GUI/auto-sizing consumer or arbitrary malformed project state.

PatternStore native dispatch rejects invalid columns and skips missing cells before
calling native track playback. Signed pre-roll remains accepted: it is a native
playback coordinate, distinct from an invalid negative automation query time.
PatternStore automation rejects invalid selectors/negative time/invalid loop spans
and retains its valid end clamp. TrackContainer selected-column collection reads
existing cells without debug-only presence assertions or lazy creation. Its global
position-sorted assertion was removed for explicit column reads, where native row
precedence must apply even when editor positions are nonmonotonic. Song-range
collection still uses getClipsInRange's chronological insertion; its precedence
was not replaced with a new global sort or row-order policy.

PatternTrack and song pattern automation compute loop length in qint64 and reject
nonpositive or unrepresentable native tick spans before wrapping. PatternTrack's
offset and source-time addition are widened, retaining the native offset/remainder
rules and narrowing only after signed wrapping. Source review explicitly retained
negative pre-roll instead of normalizing every negative coordinate into a positive
loop position. Neither this dispatch change nor its tests establish PCM correctness.

PatternQueryTest is registered with a 300-second timeout and authors thirteen cases /
29 scenarios. Four invalid selector rows include both integer endpoints; further
coverage includes sparse MIDI/automation rows through PatternStore and the base
collector, negative automation versus signed playback, local time/row precedence,
read-only target state, end clamp, six stored length/rounding boundaries, three loop
positions, five signed offset rows, song pre-roll, zero/negative meter and rounded
loop overflow refusal, empty project fallback and a native project save/load query
round trip. The observed InstrumentTrack subclass records native routing coordinates
without loading an instrument or synthesizing audio. All cases are uncompiled/unrun.

The work did not make raw pattern-column destruction transactional. Removal still
uses existing native callbacks and arbitrary reentry/retirement while destructors
run remains an unsupported boundary. Existing engine/owner/thread assumptions,
concurrent mutation, callback/plugin/global/parent deletion and throwing cleanup
retain shared limits. Actual C++/Qt/MOC/ABI, GUI/legacy/project/plugin/DSP compatibility,
rendered PCM/audio hardware, CPU/RSS/latency and real-provider complete musical
interpretation/planning/composition/rendering/listening/evaluation/revision/
preservation remain unverified.

Source diagnostics pass across 472 changed/new C++ files and 162 native operations.
Python syntax, report JSON, fixture inventories/whitespace, touched final newlines
and git diff --check pass. Native tools/executable remain unavailable on PATH and all
17 pinned submodules remain uninitialized. Complete DAW acceptance remains unproven;
the full commission remains active.


Native automation transport interruption continuation, 2026-10-14

The previous turn made source progress on pattern query immutability and loop
arithmetic. Inspection of automation playback found a further concrete source gap:
weak target snapshots protected returned model retirement, but a callback could
stop/restart/pause/seek/clear the project while the old pass continued applying its
saved values. Native processNextBuffer would then proceed through a raw track list
captured before the callback. Project clearing or later-peer retirement could leave
that list stale. No native reproduction or execution was obtained.

The private processAutomations method now returns continuation status and accepts
the buffer's weak playback track snapshot. It captures existing native playback
generation, play mode/state, actual timeline position and weak selected pattern
identity/registration. It checks that context after native recording ownership,
curve recording, previous controller release, target ownership and value callbacks.
A changed context returns before further saved targets/recording work. Weak model
snapshots and publication of current ownership before applying values are retained,
so ordinary failure/stop handling can still restore surviving controller targets.
No new transport epoch, structured operation or provider declaration is introduced.
The earlier ownership test seam was adapted to weak tracks without changing its
11 cases / 16 authored scenarios.

Native processNextBuffer captures weak track handles before timeline/automation
callbacks, consumes a failed automation continuation before metronome/track work,
skips retired or detached tracks and rechecks transport/position after native
track callbacks. Pattern playback checks the selected owner's live identity and
registration. Timeline increment uses a widened expected-tick bound and checks
returned transport/position before writing frame offset, preserving a callback's
stop/restart/seek reset rather than overwriting it. Completed-buffer frame counting
excludes an abandoned buffer; in-flight native calls can already have changed state
or produced effects. Native timing/playback state remains authoritative.

SongAutomationInterruptionTest is registered with a 300-second timeout and authors
12 cases / 42 scenarios. Thirty scenarios cover stop/restart/pause/seek/project clear
at ownership, actual value publication, prior-controller release, recording, buffer
and timeline stages. Additional rows cover later-peer deletion/detachment during
automation and track playback, four returned track-callback transport changes, fresh
post-restart automation, retained native tempo automation and selected-pattern change/
retirement. The observed instrument records dispatch counts without synthesizing
PCM or loading a plugin. Tests intentionally avoid deleting the executing track's
callee during its own play callback. All are uncompiled/unrun.

This check does not roll back earlier writes, preempt setters/plugins or make native
callbacks atomic. Paused/seeked/selection-changed controls can retain their native
ownership until the next pass or stop. Arbitrary nested DSP/automation reentry,
effective context changes reverted within a callback, same-context curve binding/
geometry edits, self/callee/parent/global/plugin destruction, concurrent mutation,
throwing cleanup and external side effects retain unsupported limits. Weak playback
snapshots add a buffer allocation and repeated registration searches can be quadratic;
there is no CPU/RSS/latency or real-time measurement. Full C++/Qt/MOC/ABI, GUI/legacy/
project/plugin/DSP compatibility, actual PCM/audio hardware and real-provider complete
musical interpretation/planning/composition/rendering/listening/evaluation/revision/
preservation remain unverified.

Source diagnostics pass across 473 changed/new C++ files and 162 native operations.
Python syntax, JSON, fixture inventories/whitespace, touched final newlines and git
diff --check pass. Native build tools and executable remain unavailable on PATH and
all 17 pinned submodules remain uninitialized. Complete DAW acceptance remains
unproven and the commission remains active.


Native staged automation commit continuation, 2026-10-14

The previous turn made source progress on transport interruption during automation.
Review of native recording publication found another concrete defect: commit held
its recursive clip mutex across cooperative checkpoints, but used m_recordingTake
after callbacks without checking whether the take had been discarded/replaced or
received more samples. A callback could invalidate that dereference, publish stale
prepared data or overwrite a direct edit. A checkpoint that waited for a reader
needing the same mutex could also deadlock. These are source findings; no native
failure reproduction or execution was obtained.

Native commit now requires disarmed recording, as the existing StudioRecording
release path already provides. Under the initial clip lock it snapshots weak
receiver/take/owner handles, original and captured time maps, recording sample/
budget/grid/error/last-value state, binding order plus original raw identities,
geometry, interpolation/tension and meter. Raw identities are compared only as
identity values alongside weak handles, so a legacy disconnected target's retirement
cannot become an apparently unchanged null binding. Clip membership is rechecked.
The snapshot is taken at commit entry rather than at reservation and does not pin
every external parameter/controller property or earlier same-context visible edit.

Prepared nodes are rebound to the receiving curve. Cooperative checkpoints execute
without holding its mutex, then revalidate lifetime/configuration/ownership. Full
map comparisons are deferred to final admission and its locked recheck, avoiding
a full map scan at every 256-node checkpoint. Native AutomationNode equality covers
its owner and position as well as both values, both tangents and locking, so raw
node changes without dataChanged also invalidate publication. Under the final lock
the map swaps and the pending take retires. Native sizing and final dataChanged run
after releasing the mutex, with weak receiver checks after returned calls. Caller
engine/control-thread ownership remains required; releasing the clip mutex is not
release of every outer native lock.

AutomationTakeCommitTest is registered with a 300-second timeout and authors 16 cases /
28 scenarios. Coverage includes successful native node/tangent/content preservation
and rebinding, discarded/replaced takes and fresh retry, duplicate/new additional
samples, eight geometry/interpolation/binding/meter changes, four raw node changes,
checkpoint-raised recording error, disconnected legacy target retirement, detached
receiver, required disarming, first/final checkpoint exceptions, receiver retirement
at a checkpoint, cross-thread valueAt readers during checkpoints/final notification,
retained independent name/mute edits and observable final notification failure or
receiver retirement. All remain uncompiled/unrun. The prior staged recording and
application session recovery suites remain registered but also unexecuted.

Pre-publication refusal preserves visible data except for changes made by the
callback itself; a surviving take may be retried from a fresh snapshot. Native
post-publication exceptions can leave the take retired and data published. No new
compound native journal entry or rollback across notifications is claimed. Native
sizing/setters are not preempted, and deletion inside an executing sizing/callee,
arbitrary reentry/global/parent/plugin retirement, concurrent mutation, throwing
cleanup, malformed data and external side effects retain shared limits. Copying,
comparison and callback work have no hard deadline or measured CPU/RSS/latency bound.
Complete C++/Qt/MOC/ABI, actual capture/GUI/legacy/project/plugin/DSP compatibility,
rendered PCM/audio hardware and real-provider complete musical interpretation/
planning/composition/rendering/listening/evaluation/revision/preservation remain
unverified.

Source diagnostics pass across 475 changed/new C++ files and 162 native operations.
Python syntax, JSON, fixture inventories/whitespace, touched final newlines and git
diff --check pass. Native build tools/executable remain unavailable on PATH and all
17 pinned submodules remain uninitialized. Complete DAW acceptance remains unproven;
the commission remains active.


Native automation reservation source continuation, 2026-10-14

The previous turn made source progress on cooperative commit publication. Its
commit-entry baseline still allowed a visible edit made earlier during a reserved
capture to be overwritten by the older take. Native target range/step/scale changes
could also alter the meaning of sampled values without changing the binding pointer.
This turn moves the source baseline to reservation rather than redefining the
commission around the earlier commit boundary. These are source findings; no native
reproduction or execution was obtained.

AutomationClip now owns an immutable process-local RecordingSource alongside its
reserved take. The source stores an independent native point map, pending binding
IDs, weak owner/membership and ordered weak/raw binding identities, geometry,
interpolation/tension/meter and direct target minimum/maximum/step/scale/automation
support. Explicit point-map detachment prevents a mutable AutomationNode reference
retained before reservation from modifying a shared baseline in place. Weak/raw
identity comparison rejects retirement and same-address replacement. Ordinary
control values and independent name/mute changes are not frozen.

Capture validates context and target metadata before accepting a sample, using the
existing latched recording error on a mismatch. It does not scan the point map each
sample. Complete map comparison occurs before commit, so an existing visible point
edit is retained while the stale take is refused. Commit preparation additionally
checks the reservation baseline at its existing checkpoint/final-admission boundaries.
Discard starts a new baseline from the current visible curve. Effective changes
reverted before an invalid sampled context may be accepted; an error already latched
by capture remains until discard. This uses effective native state comparisons,
not a new universal mutation epoch.

Reservation construction exposed another callback boundary: creating the detached
AutomationClip emits native Song length notifications. Construction now runs outside
the receiver mutex, then checks weak receiver/source state and whether another active
reservation was published. A nested published take is preserved instead of overwritten.
The detached take's unused explicit length copy was removed; its sizing remains
disabled. Baseline and take publish together under the receiver mutex and retire
together at commit/discard. Native caller engine/control-thread ownership remains
required, and the previous post-publication observable boundaries are unchanged.

AutomationTakeReservationTest is registered with a 300-second timeout. Fourteen cases /
40 authored scenarios cover fourteen point/node/tangent/geometry/interpolation/
binding/meter/target-metadata conflicts before commit; twelve capture context changes
including retirement; deferred point conflict; retained mutable-node alias; effective
restoration, latched error and fresh discard/retry; ordinary control value/name/mute
changes; constructor callback editing/receiver retirement/mutex reader/nested active
reservation; three checkpoint target metadata conflicts; and forced target address
reuse. All are uncompiled/unrun. Existing staged commit and StudioRecording session/
capture fixtures remain registered and also unexecuted.

The baseline is process-local; unfinished takes are not serialized for restart.
Direct target dimensions are pinned, while complete linked-control/controller/
external-driver state remains the native caller's responsibility. StudioRecording
keeps its existing broader driver checks. No compound native journal checkpoint,
post-publication rollback, arbitrary callback atomicity or universal malformed-state
hardening is claimed. Native sizing/callee/global/parent/plugin destruction, other
reentry, concurrent mutation, throwing cleanup and external effects retain shared
unsupported limits. The independent original map adds storage/copy work proportional
to point count, and capture/checkpoint work has no measured CPU/RSS/latency or hard
deadline guarantee. Complete C++/Qt/MOC/ABI, actual capture/GUI/legacy/project/plugin/
DSP compatibility, rendered PCM/audio hardware and real-provider complete musical
interpretation/planning/composition/rendering/listening/evaluation/revision/
preservation remain unverified.

Source diagnostics pass across 476 changed/new C++ files and 162 native operations.
Python syntax, JSON, fixture inventories/whitespace, touched final newlines and git
diff --check pass. Native build tools/executable remain unavailable on PATH and all
17 pinned submodules remain uninitialized. Complete DAW acceptance remains unproven;
the full commission remains active.


Native prepared automation point and settings continuation, 2026-10-14

The previous turn made source progress on recording reservation stability. Ordinary
point editing still used a raw curve pointer through separate progression, tension,
point and tangent setters. Each could emit notifications before the next write;
a callback could retire/change the curve while the shared helper continued. Native
putValues/setNodeTangents also retain their own mutex across signals. These are
source findings, not a native failure reproduction or execution result.

AutomationClip::editCurve now prepares point/settings edits using the native curve
implementation. It validates the application/model thread, engine/project state,
idle recording/reservation and curve dragging, input fields and target ranges.
The independent source snapshot helper is shared with recording reservation. It
preserves original points, geometry, bindings and native target dimensions while
cooperative checks validate context. A private detached native curve runs the
existing putValues/setNodeTangents arithmetic, preserving exact source ticks,
in/out discontinuities, default tangent locking/unlocking and neighbouring tangent
behavior without invoking GUI quantization. Existing source point positions for a
point edit must fit the supported tick domain; prepared values/tangents must be
finite. Settings-only edits preserve existing geometry and node data.

Prepared nodes are rebound to the receiver, then map/progression/tension publish
under one receiver mutex. Native null binding cleanup remains part of point edits.
Automatic sizing is admitted before publication and applied through the existing
native updateLength: song/unparented curves refresh length and offset, PatternStore
curves follow native pattern extent and retain their offset. Final complete state
is checked after native sizing and an unlocked dataChanged notification. Point calls
remain applied edits, including a repeated write; empty/equal settings are no-ops.
This does not introduce a compound native journal entry or replace native GUI setters.
Whole-session history remains the application revision boundary.

Shared structured song/global/note-detuning point/settings helpers now delegate to
this native method. The six operations share protected work/cancellation/model
ownership handling without adding an operation or altering provider declaration
limits. Point receipts include changed and final node/settings values. Detuning
point/settings calls keep the helper alive while native curve construction can
notify, then check weak MIDI clip ownership, note lifetime and helper identity before
and after parent notification. Native note recording must be idle for these edits.
Clear/remove/connect/disconnect and raw GUI mutation paths retain their earlier
contracts and are not claimed to use the new prepared path.

StudioCurveEditTest is registered with a 300-second timeout and authors seventeen cases /
38 scenarios: full point/settings observation in one final curve signal; eight invalid
inputs; two settings no-ops; three recording/reservation/drag refusals; five preparation
callback failures; five returned notification failures; song/pattern sizing/offsets;
mutex reader; tangent unlock/grid independence; three structured curve locations;
durable failure/replay/undo/redo/reopening; generated tangent overflow refusal;
invalid initial meter; two unsupported source positions; cancellation/interference; and detuning owner
retirement with session recovery. All remain uncompiled/unrun. Source review also
caught a Qt keyword used as a local variable in the earlier AutomationTakeCommitTest;
it was renamed and a source guard added. This correction reinforces that the previous
source-only diagnostics never established native C++/Qt/MOC correctness.

Temporary native curve construction can still emit global length notifications before
publication. Point map/settings publication is complete under its mutex, while native
sizing and later signals remain observable boundaries. A direct post-publication
failure may retain changed data; application recovery remains broader. Global/inline
object creation and outer inspection are not universally transactional. Arbitrary
self/callee/parent/global/plugin destruction within native sizing or other executing
calls, reentry, concurrent mutation, throwing cleanup, malformed legacy state and
external side effects retain shared limits. Independent snapshots/prepared maps add
copy/allocation work, and native callbacks have no hard deadline or measured CPU/RSS/
latency bound. Full C++/Qt/MOC/ABI, GUI/legacy/project/plugin/DSP compatibility,
rendered PCM/audio hardware and real-provider complete musical interpretation/
planning/composition/rendering/listening/evaluation/revision/preservation remain
unverified.

Source diagnostics pass across 477 changed/new C++ files and 162 native operations.
Python syntax, JSON, fixture inventories/whitespace, touched final newlines and git
diff --check pass. Native build tools/executable remain unavailable on PATH and all
17 pinned submodules remain uninitialized. Complete DAW acceptance remains unproven;
the commission remains active.

Prepared native curve removal/clear continuation (2026-10-14)

The six structured song/global/note-detuning removePoint and clear routes now
share AutomationClip::editCurve with point/settings writes. A request selects
at most one point write, point removal or clear action. Native removeNode/clear
prepare the map before publication; final notification runs outside its mutex.
Removal regenerates native neighbouring tangents, retains locked tangents,
prunes dead bindings and refreshes automatic length/offset according to the
native song/PatternStore rules. Clear preserves geometry, progression, tension,
automatic sizing flag and bindings. Absent removal and empty clear remain
applied native actions. Missing global curves remain absent.

These routes require idle recording/reservation/dragging and join the protected
cooperative work scope. Detuning routes retain the helper through lazy curve
construction and check MIDI owner, note identity and helper membership through
mutation, default reset and parent notification. Clear checks that the returned
helper value equals its default and its curve remains empty after resetting.
Callbacks can still produce external effects before a refusal.

StudioCurveEraseTest authors seventeen cases / 46 scenarios for native tangents
and ownership, clear preservation, absent/empty actions, conflicting actions,
six busy states, four song/pattern sizing combinations, selected unsupported ticks,
retained malformed ticks, NaN refusal, preparation interference, six final callback
faults, mutex readers, missing global curves, six structured routes, helper reset
retirement/value tampering, session undo/redo/reopen/replay and failure recovery.
All are uncompiled/unrun. Earlier StudioCurveEditTest replay assertions now remove
and verify the native replayed flag before comparing the persisted receipt.

Raw GUI setters and connect/disconnect retain their existing contracts. Native
remove selectors can repair selected unsupported integer ticks when the retained
map is valid; structured selectors retain their published bounded tick domain.
Clear can remove unsupported ticks without tangent arithmetic. NaN source data
cannot pass the independent baseline equality check, so checked edits refuse it;
this is not a universal legacy repair API. No compound native journal was added.
Native temporary construction can emit global length notifications, and sizing,
helper reset and parent signals remain observable boundaries. Direct failures
after publication can retain changed data; StudioRuntime supplies broader project
recovery. Arbitrary executing-callee/parent/global/plugin deletion, concurrency,
reentry, throwing cleanup and external effects retain the preceding limitations.
Copies and native callbacks have no measured CPU/RSS/latency or hard deadline.

Source checks pass across 478 changed/new C++ files and 162 native operations.
Native build/Qt/MOC/ABI, GUI/legacy/project/plugin/DSP compatibility, rendered PCM,
audio hardware and a real-provider complete production remain unverified. Native
build tools/executable are absent from PATH and all 17 submodules are uninitialized.
The complete DAW commission remains active and full acceptance remains unproven.

Checked native automation binding continuation (2026-10-14)

AutomationClip::editBinding now supplies the checked control-thread native path
for automation.connect and automation.disconnect. It snapshots independent curve,
binding, target-dimension and lifetime state before preparing the requested change.
A first connection to an empty unbound curve reads the native controller-backed
value, applies the native inverse scaling and putValue node math, and publishes
that initial point and ordered weak binding list together under the curve mutex.
A final observer no longer sees the first point without its binding. Nonfinite
initial values refuse before publication. Native destruction-signal wiring,
automatic sizing and final dataChanged run after releasing the curve mutex.
Complete returned wiring state is checked before sizing; complete expected state
is checked after sizing and final notification. Both structured routes join the
protected cooperative work scope and retain bounded summary acknowledgements.

The checked API requires a ready engine on the application/model/target owner
thread, positive meter and idle recording/reservation/dragging. It preserves
native duplicate-connection and absent-disconnection false results without final
notification, removes all duplicate occurrences of a disconnected target, and
prunes the corresponding raw/saved pending IDs even for a pending-only target.
A pending-only cleanup still returns false and has no final dataChanged, matching
the existing native contract. Disconnect retains curve data/settings/geometry;
additional connections do likewise. Only an initial seeded connection refreshes
native automatic sizing: song offsets reset and PatternStore offsets remain.
Existing unsupported targets can be disconnected; new unsupported targets refuse.
Raw GUI addObject/removeObject/clearObjects and inline restore binding remain
available with their preceding contracts. No operation or compound native journal
was added; StudioRuntime retains responsibility for broader session recovery.

StudioAutomationBindingTest authors 21 cases / 57 scenarios: linear/logarithmic
initial-point observation; preserved existing curves/settings/geometry; no-op and
duplicate bindings; pending-ID cleanup followed by native resolution; six busy
states; null/unsupported/zero-meter admission; unsupported existing disconnection;
four song/pattern automatic/manual sizing combinations; five constructor faults;
three controller-read faults; native controller value versus stored value; two
nonfinite controller seeds; eight final callback faults; connection-hook recording
interference before sizing; mutex readers; target destruction after native wiring;
cancellation/point/dimension interference; four structured song/pattern routes;
and session undo/redo/reopen/replay and failure recovery. All remain uncompiled/
unrun. Source review corrected a typed model scale-setter name while authoring;
source diagnostics cannot establish C++/Qt/MOC or runtime correctness.

This publishes native data together, not a transaction across signal wiring,
controller reads, global constructor signals, native sizing or external effects.
Qt connection hooks and final observers can change state after publication;
returned failures may retain data/wiring changes until broader session recovery.
A target's sampled value can change later without invalidating the sample; the
checked context pins dimensions/lifetimes rather than every linked/controller
relation. NaN source baseline equality, invalid dimensions or unsupported geometry
can refuse checked edits. Arbitrary destruction of an executing native callee,
controller, target, parent, global or plugin within its own method; concurrency,
reentry and throwing cleanup retain the shared limits. Snapshot copies/native
callbacks have no measured CPU/RSS/latency bound or hard execution deadline.

Source diagnostics pass across 479 changed/new C++ files and 162 native operations.
Native build tools/executable remain absent from PATH; all 17 submodules are
uninitialized. C++/Qt/MOC/ABI, native fixtures, GUI/legacy/project/plugin/DSP
compatibility, rendered PCM/audio hardware, actual-provider full musical production
and requirement-by-requirement full DAW acceptance remain unverified. The complete
commission remains active; these changes do not establish completion.

Checked native controller disconnection continuation (2026-10-14)

controller.disconnect now delegates to AutomatableModel::disconnectController.
The checked native operation captures the weak ordered linked group, binding and
controller lifetimes, stored/init/interpolation values, ranges/step/scale and
controller-use flags. It requires a ready owner-thread project and owner-thread
linked models/connections/controllers, checks cancellation/context before mutation,
and preserves an unbound parameter as changed=false without notification.

A present connection is detached from the model before its destruction. Weak
cleanup retires a displaced connection once, including an owned MIDI controller,
without deleting a connection that callbacks have reinstalled on that parameter.
A scoped outgoing signal block suppresses the native setter's intermediate
notification and restores the caller's prior block state through failures or
parameter retirement. The final parameter notification follows resource retirement;
complete captured group state is checked after detach, retirement and that final
notification. The structured receipt returns changed and stored manualValue.
Linked peers retain their bindings and can still provide effective control; a
stored manual value is not a claim that all linked input has stopped. The route
joins the protected cooperative work scope. No operation or compound native
journal was added; StudioRuntime supplies broader project recovery/history.

AutomatableModel now holds its ControllerConnection through QPointer, with an
out-of-line accessor so forward declarations remain sufficient in the header.
The native setter guards receiver/replacement lifetime and binding generation
across Qt signal wiring, installs retirement handling before value forwarding,
and prevents a deferred old destruction callback from clearing a newer binding.
Native unlink clears the binding and invalidates linked caches before its final
Qt disconnect hook. The setter still returns void and leaves displaced-connection
ownership to its caller. Existing in-place rack rewiring, MIDI assignment ownership,
restore and GUI callers retain their separate lifecycle/transaction limitations.

Source review also found existing authored linked-control fixtures calling the
private isLinkedToModel query. Its existing read-only declaration was moved public
alongside the other native linked-group inspection methods; behavior is unchanged.
This fixes a source-level access problem and does not establish native compilation.

StudioControllerDisconnectTest authors seventeen cases / 31 scenarios for final
observation of retired rack/MIDI/dummy resources; absent bindings and two signal
block states; linked manual/binding/control-flag preservation; destruction-time
observation and receiver retirement; six final callback faults; destruction-time
rebinding; peer value/topology interference; cancellation/value/dimension admission;
raw setter receiver retirement and nested replacement; deferred old destruction
with a later binding; foreign-thread refusal; present/absent structured receipts;
rack/MIDI session undo/redo/reopen/replay; and failure/retirement recovery. Every
case remains uncompiled/unrun. Existing controller/MIDI, linked-value/cache,
persistence, GUI and plugin fixtures must also be executed before accepting this
native model layout/accessor and notification change.

Captured group checks do not cover every automation/plugin/control relation.
Suppression affects outgoing parameter notifications while resource callbacks and
other external effects remain observable. A returned failure can retain detached,
rebound or callback-modified state until broader session recovery. Arbitrary
executing-callee/controller/parent/global/plugin deletion, unsupported Qt-hook
reentry, concurrent mutation and throwing cleanup retain shared limits. Group
copies and native callback work have no measured CPU/RSS/latency or hard deadline.

Source diagnostics pass across 480 changed/new C++ files and 162 native operations.
Native build tools/executable remain absent from PATH and all 17 submodules remain
uninitialized. C++/Qt/MOC/ABI, native fixtures, optional GUI, legacy/project/plugin/
DSP compatibility, rendered PCM/audio hardware and actual-provider complete musical
production remain unverified. Full DAW acceptance remains unproven and the complete
commission remains active.

Checked native MIDI assignment continuation (2026-10-14)

controller.midi.connect now delegates to AutomatableModel::connectMidiController.
The native operation validates owner-thread readiness, backend identity/state,
channel/controller bounds, parameter support/scale and captured linked state before
constructing anything. It prepares the native MIDI controller/port and connection,
checks source context after each native call, and validates effective native input
settings, name, ancestry, owner threads and feedback before publication. Native
Song ancestry remains intact for MIDI-port child parameter discovery; the controller
has no shared rack index. Preparation can expose temporary Song children/backend
ports and is not a transaction over those external observations.

Weak cleanup guards replace caller unique_ptr ownership across publication. The
parameter can adopt the prepared connection and be retired by callbacks without
a second owner deleting that same connection. The checked operation suppresses
intermediate parameter notifications, activates controller input, retires displaced
connection-owned resources, restores the original signal block and emits one final
native notification. Complete source/result, candidate settings and backend/lifetime
checks surround these boundaries. Repeating an identical assignment remains an
applied replacement and returns changed=true. The structured receipt retains its
parameter MIDI selector and ownership metadata, adds changed, and the route joins
the protected cooperative work scope. No operation or compound native journal was
added; StudioRuntime remains responsible for broader project recovery/history.

The linked ControllerState snapshot/matcher is shared with checked disconnection,
including manual/init/interpolation values, ranges/step/scale, ring order, control
flags, binding/controller identities and owner threads. ControllerConnection now
installs its controller-retirement hook for owned MIDI controllers as well as shared
controllers. Independent owned-controller destruction retires its connection; normal
connection destruction/rewiring unhooks before deleting its owned controller to
avoid recursive retirement. This does not make arbitrary callee self-destruction
inside an executing native constructor/method safe.

StudioMidiAssignmentTest authors nineteen cases / 42 scenarios: unbound/shared-rack/
owned-MIDI predecessors; channel/controller/readable boundaries; repeated ownership
replacement; six invalid/support/scale admissions; five preparation faults including
source value/dimension/death, constructor exception and changed backend; prepared
port interference; resource-retirement observation and parameter retirement; six
final callback faults including model/owned-MIDI destruction and binding replacement;
signal blocks; linked peer preservation/interference; cancellation before/after
preparation; native controller channel filtering; linear/logarithmic dimensions;
structured receipts; MIDI child parameter discovery; and session undo/redo/reopen/
replay/failure recovery. The fixture injects a test MIDI client and exercises native
controller/model/port objects; it is not hardware, backend compatibility or rendered
PCM evidence. All cases remain uncompiled/unrun.

Native temporary construction/configuration/backend registration and resource
callbacks remain observable. Input may arrive during preparation; captured linked
fields do not cover every automation/plugin/controller relation. Returned failure
can retain callback-modified or published state until session recovery. Unsupported
executing-callee/constructor/backend/global/parent/plugin deletion, connection
stealing/sharing, Qt-hook reentry, concurrent mutation and throwing cleanup retain
the shared limitations. Rack controller.connect in-place rewiring and other raw
assignment/restore/GUI/learn callers retain separate lifecycle and transaction
limits. Copies, feedback traversal and callback work have no measured CPU/RSS/
latency or hard deadline guarantee.

Source diagnostics pass across 481 changed/new C++ files and 162 native operations.
Native build tools/executable remain absent from PATH and all 17 submodules remain
uninitialized. C++/Qt/MOC/ABI, native fixtures and regression suites, optional GUI,
legacy/project/plugin/DSP compatibility, rendered PCM/audio hardware and actual-
provider complete musical production remain unverified. Full DAW acceptance is
unproven and the entire commission remains active.

Checked shared-controller assignment and retirement continuation (2026-10-14)

controller.connect now delegates to AutomatableModel::connectController. The
native operation validates owner-thread readiness, registered shared rack identity,
parameter support/scale, feedback and the captured linked model state. Existing
ControllerConnection identity is preserved for in-place rewiring, including a
previous owned MIDI binding. An initially unbound model prepares a fully constructed
native dummy connection before wiring the shared target and adoption. Weak cleanup
retires abandoned connections without overlapping model ownership. The current
controller-use flag remains intact; this route does not reactivate input suppressed
by native automation ownership. An identical finalized binding returns changed=false
without a parameter notification. A changed binding restores the prior signal block
and emits one final parameter notification with complete returned context checks.

The structured route joins the protected cooperative work scope and returns the
actual current rack index, native connection count and changed. Callbacks can remove
an earlier controller and shift indices; the acknowledgement follows the surviving
controller rather than repeating the input index. No operation or compound native
journal was added. Session recovery/history remains broader than a direct native
operation, whose returned failure can retain callback-modified or published state.

Native ControllerConnection now holds a weak controller reference, explicit count
membership and a bounded generation. In-place rewiring decrements the previous
count, publishes the target/count and installs retirement/value forwarding before
retiring old owned MIDI resources. Capacity checks prevent overflowing a native
connection count or generation. Weak returned-state checks stop old continuation
after receiver/target retirement or a nested rewire. Cleanup retires displaced owned
resources once. Destruction marks retirement, clears fields, unregisters and removes
its count before resource callbacks; already retiring connections cannot be rebound
or deleted again through deleteConnection.

An early retiring signal is emitted before native resource hooks, even if connection
signals were blocked. For ordinary owner-thread bindings its model handler clears
the binding and invalidates linked caches before observers can read a retiring
controller. Qt AutoConnection preserves deferred delivery for foreign-thread legacy
bindings; checked model operations require owner-thread linked state. Accessors,
model destruction/read/cleanup paths ignore retiring connections, and generation
checks keep later bindings intact when old destruction callbacks arrive. This adds
native layout and MOC changes that require full rebuild/regression verification.
Raw setController remains void and connection-level valueChanged/resource callbacks
remain observable while final parameter notification is suppressed.

StudioControllerAssignmentTest authors 22 cases / 45 scenarios: unbound/shared/
owned-MIDI/dummy predecessors; identity/count observation; no-op and input-use flag;
six target/capability refusals; direct/linked feedback; count readiness during owned
MIDI retirement; retirement deleting model or replacement controller; raw observer
retirement and nested rewiring; count exhaustion; early retirement/cache clearing
with blocked/unblocked connection signals; destructor resource callbacks retiring
the model; a later replacement binding surviving old destruction; seven final
parameter callback faults; signal blocks; linked peer preservation/interference;
four cancellation/source-interference paths; shifted rack receipts; session undo/
redo/reopen/replay and failure recovery. These are authored source fixtures and all
remain uncompiled/unrun. Existing MIDI assignment/disconnection, controller restore,
linked-control/cache, GUI, plugin and project suites must also be executed.

This does not establish a universal transaction over Qt wiring, temporary unadopted
connection counts, connection-level observers, resource cleanup, controller child
parameters or plugin/automation state. Raw constructor/GUI/restore/learn paths retain
separate lifecycle and transaction limits. Arbitrary destruction of an executing
constructor/callee/parent/global/plugin, unsupported Qt-hook reentry, stolen/shared
connection ownership, concurrent mutation and throwing cleanup remain limitations.
Copies, feedback traversal and native callbacks have no measured CPU/RSS/latency or
hard deadline guarantee. Source diagnostics across 483 changed/new C++ files and
162 native operations pass; no C++/Qt/MOC/ABI or native runtime success is claimed.

Native build tools/executable remain absent from PATH and all 17 submodules remain
uninitialized. Optional GUI, legacy/project/plugin/DSP compatibility, rendered PCM,
audio hardware, actual-provider complete musical production and requirement-level
full DAW acceptance remain unverified. The entire commission remains active.

### Controller rack lifecycle continuation (2026-10-14, source only)

Song rack changes now use native audio ownership, weak owner/target references and
an in-flight removal list. Removal detaches the raw roster entry before beginning
controller retirement and before notifying removal observers. Re-adding that
retiring target is refused. A weak cleanup guard completes targeted deletion when
a removal observer throws or deletes the target itself. An independently running
destructor retains responsibility for its own completion. Removing the process
shared dummy preserves that singleton; an ordinary custom dummy remains owned.
Direct controller destruction prunes matching raw roster entries and emits a
roster notification. These hooks use a separate QObject context that is destroyed
at the start of Song destruction, before Song members or base-owned tracks die.

Controller has an early retirement phase, entered before built-in LFO, peak and
MIDI resources are dismantled. It unregisters clock participation, forces the
early notification through blocked signals and refuses new checked bindings or
structured selections. ControllerConnection listens to this phase before its
QObject destruction fallback; model bindings and linked caches retire before
roster destruction observers run. Recursive deletion of an already retiring owned
controller is skipped. Custom dummy bindings also retire; their uncounted status
does not exempt them from target lifetime tracking. Custom subclasses must enter retirement before destroying
their own resources. Retirement/destruction listeners must not throw.

Structured controller.add/edit/remove suspend incidental event delivery and use
cancellable native work under audio ownership. Creation checks cancellation both
before construction and after preparation. Receipts identify the surviving
controller at its current index, including callback-driven earlier-row removal;
rename returns the actual resulting name. Creation summary=true avoids expanding
the rack. Legacy controller.list snapshots weak controller/child identities,
checks the selected row and roster size around cooperative work and returned
getters, then checks all roster identities before returning. Use project.inspect
controller pages for bounded inventories. Changed identities refuse inspection;
this is not a snapshot transaction over every native parameter value.

The GUI rack now checks raw signal payload membership before dereferencing it,
purges views whose models died, uses weak references in queued deletion requests,
and rechecks ownership after its existing modal confirmation. Empty/stale move
requests are ignored. This source review does not establish GUI compatibility.
The structured exception boundary also returns an error receipt for nonstandard
native exceptions; it does not make arbitrary partial native changes atomic.
Published creation or callback changes can remain after direct failures; Runtime
checkpoint recovery is the separate broader boundary. No operation or compound
native journal was added: the capability count remains 162.

StudioControllerLifecycleTest authors 20 cases / 36 scenarios for raw and
structured removal, early/observer deletion, exceptions and admission cleanup,
blocked direct destruction/cache retirement, destructor-driven row removal,
creation callbacks/current indices/nesting, inspection interference, rename
retirement/faults, cancellation before mutation and after construction, shared and
custom dummy ownership, owner retirement and teardown hook ordering, busy reads,
peak type admission, and session undo/redo/reopen/replay/recovery. All are
uncompiled/unrun. The peak admission case uses only a native base-controller type
tag and supplies no evidence of real peak-effect plugin execution.

The retirement signal and context add native layout/MOC and callback-order
changes requiring a complete rebuild and native/GUI/plugin/project/DSP regression
checks. Temporary constructor exposure, arbitrary destruction inside an executing
constructor/getter/callee/parent/global/plugin, unsupported Qt-hook reentry,
foreign-thread or concurrent raw mutation, stolen connection ownership and
unbounded observer-created work retain limits. Weak checks protect continuations
after returned calls, not arbitrary invalid execution inside those calls. Copies,
metadata, native callbacks and feedback traversal have no measured CPU/RSS,
latency or hard-deadline guarantee. Source diagnostics cover 486 changed/new C++
files and 162 operations; they provide no C++/Qt/MOC/ABI or native runtime proof.

Full native fixtures, optional GUI, legacy/project/plugin compatibility, rendered
PCM, actual hardware, real-provider complete musical production and requirement-
level full DAW acceptance remain unverified. The full commission remains active.

### Checked native MIDI learn continuation (2026-10-14, source only)

The headless learn operation now shares AutomatableModel's native MIDI assignment
implementation with controller.midi.connect. It does not publish through a second
raw setter/unique_ptr adoption path. The private assignment helper accepts exact
learned input subscriptions and a retained CC value; ordinary MIDI connect keeps
its existing public signature and default configuration. Channel/controller,
input mode, native Song ancestry, connection ownership, subscription requests,
queue registration/failure/drop versions and retained normalized native buffer
values are checked through publication and notification. The controller remains
connection-owned and outside the shared rack.

Learn captures an independent ControllerState for the selected linked group before
creating its detector. Stored/initial/old values, dimensions, scale/step/strictness,
input-use flags, linked order, binding identities, controller lifetimes and owner
threads must survive the wait and preparation. Song, parameter and detector use
weak references. Canonical project/engine/backend identities, loading/transition
state and recording/device readiness are checked after returned work; waiting
continuations check ownership before acquiring another native audio guard.
Backend and port checks retain the observed-active queued-input requirement. The
native first valid CC and source identity select the final input; unexpected
detector channel/controller/subscription changes refuse adoption.

The native detector is retired after candidate preparation and before publishing
the new binding. Its destruction callbacks are followed by cancellation, baseline
and candidate validation. The existing assignment boundary blocks intermediate
parameter notifications, publishes and activates the replacement, retires the
previous owned resources, restores the original signal block, then emits the final
parameter notification and checks the resulting state. Detector/candidate cleanup
uses weak guards without overlapping unique_ptr/model ownership. Existing shared
rack controllers survive replacement; their former connections retire. The final
Song modified notification is also followed by owner and committed-state checks.
Success returns changed=true; connectionVerified=false still does not prove
physical delivery. The learned value is transient and is not restored as live
input by checkpoint replay.

StudioMidiLearnTest now authors 17 cases / 73 scenarios, including its existing
native detection/filter/source/refusal/history coverage and new linked-baseline
interference, waiting detector death, candidate construction/cancellation faults,
detector cleanup interference, final shared/owned resource ordering, blocked
signals, target/controller/value/topology/input-use/port faults, project modified
callbacks, standard/nonstandard exceptions and Runtime failure recovery/replay.
These fixtures and all affected assignment, input, backend, restore, persistence,
GUI and DSP regressions remain uncompiled/unrun. The fake queued MIDI client is
not physical hardware or a real provider production trace.

This boundary validates returned native work; it is not a universal transaction
over native/plugin/controller internals. Direct failures after publication can
retain callback state until separate Runtime checkpoint recovery. No compound
journal or capability was added (162 operations). Deliberate application-thread
nested event waits remain necessary for queued input; incidental preparation event
pumping is suppressed. Cancellation/timeout remain cooperative before publication,
with no measured CPU/RSS/latency or hard deadline. Arbitrary destruction inside an
executing constructor/callee/parent/global/plugin, unsupported Qt-hook reentry,
foreign/concurrent raw mutation, throwing retirement listeners and same-address
backend replacement retain limits. The GUI detector's useDetected/copy methods
are unchanged and need separate verification.

Source diagnostics cover 486 changed/new C++ files and 162 operations. No native
compilation, Qt/MOC/ABI, optional GUI, project/plugin compatibility, PCM, hardware,
real-provider musical workflow or full DAW acceptance is proved. The full
commission remains active.

### Native effect-chain lifecycle continuation (2026-10-14, source only)

EffectChain transition guards now retain a weak owner rather than a raw reference
to a member that callbacks can destroy. Retirement disconnects a dedicated QObject
hook context before chain members or base-owned children disappear. Published
effects have direct destruction hooks that prune their raw roster entries and
disable an empty chain. Nested chain notifications are suppressed during an
existing transition. This tracking begins at QObject destruction, after derived
plugin resources; it is not an early-retirement protocol for every plugin.

Append publishes ownership before callbacks, then checks surviving chain/effect
membership. Owned take and legacy detach use weak cleanup across reparenting,
disabling and chain notifications. A target deleted by an observer is not deleted
again; one adopted into another native chain is neither returned as a second
unique owner nor cleaned up by its former chain. A refused return can therefore
still accompany native callback changes. Owned take releases the QObject parent;
legacy remove preserves the existing parent contract. Clear checks its weak owner
after notifications and each destructor, tolerates another pending effect being
deleted, and avoids repeating aboutToClear when an observer starts owner teardown
inside that same notification. Actual destruction/retirement listeners must not
throw. Ordinary explicit transition exceptions retain their separate error path.

Native save snapshots weak ordered effect identities and refuses changes after
returned serializers. Load retains native teardown-before-restore ordering, uses
weak cleanup for unpublished candidates, installs lifetime tracking at publication
and validates the restored roster through final notification. Missing-plugin XML
remains preserved through the native DummyEffect path. Serialization failures can
leave partial XML or native state; no universal plugin serialization transaction
is implied.

Structured effect.add/remove/move/clear now perform cancellable work with incidental
event pumping suspended under native audio ownership. Weak project/chain/effect
checks retain the original owner selection. Creation returns the surviving current
index; movement verifies the requested index; removal verifies retirement; clear
verifies an empty disabled result. The ordered weak roster and bypass state are
checked after Song's modified notification. Published or callback changes can
remain after a direct failure; Runtime checkpoint recovery is the separate broader
boundary. No capability or compound native journal was added (162 operations).

Effect inspection checks weak roster identities around returned native work and
parameter expansion. Shared parameter discovery now snapshots weak roots/children
after native refresh, and parameter pages check weak identities around cooperative
work. These checks do not establish that every caller safely collected its initial
raw roots, or freeze every parameter value/dimension. The optional GUI retains its
existing weak queued deletion request; add-effect dialog, candidate and rack owner
continuations now also use weak cleanup and checks. GUI execution remains unrun.

EffectChainLifecycleTest authors 16 cases / 32 scenarios for independent deletion,
append/detach/move owner or target death, cross-chain adoption, ordinary/nonstandard
exceptions, nested clear and pending-peer deletion, serializer/load interference,
track-chain retirement, inspection/discovery changes, cancellation, project
notification invalidation and Runtime recovery/replay. All are uncompiled/unrun.
The fixture uses native test effects and missing-plugin records, not actual peak
or third-party plugin execution. Existing EffectChainTest, controls/paging/history,
GUI, controller, project, plugin and DSP suites also require execution.

The native chain layout and notification/ownership changes require a complete
C++/Qt/MOC/ABI rebuild. Arbitrary destruction inside executing constructors,
serializers/getters/callees/parents/globals/plugins, unsupported Qt reentry,
foreign-thread/concurrent raw mutation, stolen ownership and same-address aliasing
retain limits. Authoritative native DSP loops are retained; no universal real-time
or destructor safety is established. Plugin work and cancellation are cooperative,
with no measured CPU/RSS/latency or hard deadline. Source diagnostics cover 487
changed/new C++ files and 162 operations; full native, optional GUI, project/plugin
compatibility, PCM, hardware, real-provider musical production and complete DAW
acceptance remain unproved. The full commission remains active.

### Native instrument loading continuation (2026-10-14, source only)

InstrumentTrack now stores its selected instrument as a QPointer with out-of-line
accessors, so an independently destroyed instrument does not leave a dangling
selection or get deleted again during track teardown. Native loadInstrument uses
audio ownership for its complete silence/teardown/construction/naming/notification
sequence. The old selection is cleared before old-plugin destruction. The track's
nonrecursive processing mutex is not held across destructors, factories or signal
observers, avoiding an old teardown callback deadlocking when it deletes the
track. Native processing uses the engine's existing audio ownership boundary.

A weak owner and replacement admission flag prevent stale continuation or nested
native load during old retirement, construction, rename and instrumentChanged.
Track retirement starts by refusing further replacement. Candidate cleanup is weak
and only deletes an unadopted instrument that belongs to the requesting track;
a foreign factory result is refused without taking ownership. Requested name/key
data is copied before callbacks. Cooperative cancellation is checked before
mutation and after construction. The published instrument is checked after its
native display-name lookup, track rename and instrumentChanged notification.
Exceptions release admission and preserve already-published native ownership.
Direct failure after old teardown can leave a null or callback-modified selection;
Runtime checkpoint recovery is the separate broader boundary.

Track.setName also retains a weak receiver across Song.modified before emitting
nameChanged. Returned note-handle cleanup checks the track before releasing MIDI
output. These checks protect continuations after returned calls, not arbitrary
invalid execution inside plugin/native callbacks. Full native DSP/MIDI/hardware
behavior remains unverified; software cleanup is not proof of physical silence.

Structured instrument.load now runs cancellable work with incidental event
pumping suspended under native audio ownership. It validates current weak project,
container, registered track and selected-track recording/replacement readiness,
uses the existing native factory, validates the selected plugin descriptor and
returns the surviving current track index/container. Song.modified is followed
by track and instrument identity checks. Use the returned index and rediscover
after callback-driven row changes. No operation or compound native journal was
added: 162 capabilities remain.

The optional GUI loader keeps weak track/instrument references and checks refused
loads before moving the instrument to its existing destination thread. Browser,
drop and instrument-window paths now check nullable loads and returned owners.
Legacy GUI worker loading is retained; arbitrary foreign/concurrent Qt mutation is
not established as safe. GUI/preview/import behavior requires native regression.

Preset replacement/settings entry points now refuse overlap with direct native
replacement. The complete preset loadTrackSpecificSettings/replaceInstrument/
loadPreset stack still has separate raw callbacks and holds a track mutex across
settings restoration. Its lifetime/ownership risks are not resolved by this
continuation and require further implementation and verification. This is an
explicit remaining native path, not a claim of complete preset safety.

InstrumentLoadLifecycleTest authors 18 cases / 31 scenarios for composition/port/
control identity, nested loads at four phases, owner death during old teardown or
a returned factory, standard/nonstandard/null/foreign factory results, independent
plugin deletion, rename/instrument notifications and faults, cancellation before
mutation and after construction, copied source names, settings/preset overlap
refusal, selected-note-recording admission, current-row receipts, Song.modified
retirement, and Runtime recovery and
cached failures. All remain uncompiled/unrun. Most use a native test instrument;
some structured cases borrow the actual builtin descriptor without executing that
plugin's DSP. Separate Runtime cases request the actual tripleoscillator plugin,
but their authored source is no execution or sound evidence.

Weak selection and GUI loader changes require a full native layout/accessor and
C++/Qt/MOC/ABI rebuild. Executing constructor/callee/getter/parent/global/plugin
self-destruction, unsupported Qt reentry, stolen ownership, throwing retirement,
same-address replacement and raw concurrent mutation retain limits. Factories
must clean up their own incomplete objects when they throw. There is no complete
new GUI retirement protocol for independently deleted plugins. Plugin work and
cancellation are cooperative, with no measured CPU/RSS/latency or hard deadline.
Source checks cover 490 changed/new C++ files and 162 operations; native fixtures,
optional GUI, project/plugin compatibility, actual PCM/hardware, real-provider
musical production and full DAW acceptance remain unproved. The full commission
remains active.

### Native preset restoration continuation (2026-10-15, source only)

Native preset restoration now has checked continuation boundaries through Track,
InstrumentTrack and the shared serializer. Track.loadTrack/loadPreset use native
audio ownership, suppress incidental event pumping, retain weak receivers and
owners, refuse overlapping track/preset state loads, and check after returned
setters, derived settings, clip construction/restoration and linked restoration.
The derived-settings dispatch flag is cleared before another cooperative work
check. Native full-track restoration and preset-only clip retention remain
separate existing modes; weak clip deletion stops after receiver retirement.

InstrumentTrack settings restore no longer holds the track processing mutex over
native model, resource, plugin or observer callbacks. Its MIDI-CC loading flag uses
weak cleanup and refuses nested settings. An expected weak plugin plus raw identity
checks independent retirement without confusing it with an intentional null
selection. Ordinary restore detaches the old selection before teardown, prepares a
native factory result with weak cleanup, publishes it, restores native plugin
state, then validates the selected actor after instrumentChanged. The matching
preview reuse path, legacy plugin-node fallback, MIDI CC ordering/active-list
validation, shaping, effects and tuning continue through their native models.

Outer replaceInstrument retains a separate preset admission scope and grants only
its intended Track load access. It preserves native editable clip identities,
excludes the top-level preset MIDI port node, restores destination mute/solo and
mixer settings, finishes the existing linked-control scope and validates final
preservation and selected plugin identity after Song.modified. Native track and
port labels still follow the loaded track name. Existing internal/legacy linked
control behavior requires execution of the linked-track and full project suites;
source similarity is not compatibility proof.

SerializingObject save/restore now observes QObject-backed receivers across
returned virtual settings and hook calls, and stops before touching a hook after
receiver retirement. Non-QObject serializers retain their native contract without
QObject lifetime observation. Additional weak continuations cover the instrument
rename override after Track.setName, returned MIDI CC/cache and pitch events,
MIDI-CC initialization, microtuner, note stacking/arpeggio, sound shaping and
envelope model loads. User-wave decoding prepares a local result before assigning
the member. Envelope parameter notifications now occur after releasing the
parameter mutex, so notification callbacks can return or retire the owner without
a later locker accessing its destroyed mutex. DSP data computation is retained;
actual audio correctness remains unverified.

Structured preset.load runs cancellable work with event pumping suspended under
native audio ownership, validates weak project/container/track selection before
and after file loading/restoration, refuses selected-track note recording, returns
the surviving current track index/container and checks the selected actor after
final project notification. Existing native warning handling remains the partial-
load refusal boundary. No operation or compound native journal was added: 162
capabilities remain. Cancellation or native/callback failure after a partial
settings write or old-plugin teardown can retain direct native state; the enclosing
Runtime checkpoint recovery is the separate broader boundary.

InstrumentPresetLifecycleTest authors 13 cases / 37 scenarios covering modern and
legacy restore, preview reuse, native values/model IDs, clip/routing/flag
preservation, nine owner-retirement phases (including tuning, filter and envelope),
generic track/preset rename death, four reentry phases, preservation invalidation,
factory faults, explicit partial cancellation, four serializer settings/hook
lifetime paths, ordinary non-QObject persistence, shifted track receipts and
Runtime standard/nonstandard failure recovery/replay. All are uncompiled/unrun.
Most use native test instruments; separate Runtime rows request actual
tripleoscillator load/save/restore but provide no execution or sound evidence.

Track admission virtual/flags and instrument changes require full layout/vtable,
C++/Qt/MOC/ABI rebuilds. The shared serializer and unlocked envelope notification
change require broad native, optional GUI, legacy project, preset, plugin, MIDI,
DSP and persistence regressions. These checks protect continuations after returned
calls, not arbitrary invalid execution inside constructors/callees/getters/
serializers/hooks/parents/globals/plugins, raw foreign/concurrent mutation, stolen
ownership, unsupported Qt reentry or throwing cleanup/destruction listeners.
Unobserved non-QObject lifetime and raw saveTrack/savePreset or other paths outside
these boundaries retain separate risks. Opaque plugin bytes and all native value
or dimension changes are not universally verified or frozen. Original XML
document identity is retained for native linked references, rather than claiming
an immutable deep snapshot. Native cleanup attempts do not prove physical silence.

Source diagnostics cover 494 changed/new C++ files and 162 operations. Plugin work
and cancellation remain cooperative without measured CPU/RSS/latency or hard
deadline evidence. Native fixtures, optional GUI, project/plugin compatibility,
actual PCM/hardware, real-provider musical production and whole DAW acceptance
remain unproved. The full commission remains active.

### Checked native meter arithmetic and notification snapshots (2026-10-15, source only)

Inspection of live meter reads found that TimePos multiplied signed integers and divided by a controller-derived denominator without domain checks. Explicit-TimeSig bar calculation now validates positive operands, widens the product to int64, rejects sub-tick or tick_t-overflow results, and narrows afterward. Beat calculation refuses zero-length beats before quotient/remainder operations. Previously defined positive truncation and signed position remainders remain; a large representable quotient with an overflowing old intermediate is now safe. TimeSig construction remains a value container, while calculation validates use. Moving these inline calculations into core definitions requires a complete rebuild.

Song reads each live meter control once under native model ownership, checks its actual model range and calculated resolution, then publishes accepted clock/history/VST sync data from one snapshot. Its local guard is released before Qt notifications. A private nonwrapping atomic generation advanced under native ownership prevents superseded outer frames from publishing more state after a nested update. Weak Song/engine/project checks surround live reads and dispatch boundaries. Known receivers report through NativeValueNotification and rethrow only after Qt returns; direct failures can retain already published state, while StudioRuntime owns durable recovery. The counter does not make other state or VST shared-memory writes atomic, or pin executing getters/roots.

TimeSignatureSafetyTest authors nine groups / forty-one scenarios: ten positive/boundary arithmetic rows, ten invalid bars, two zero beats, ten malformed live Song controls with unchanged accepted clock/VST metadata, a changing getter read once, joined-thread guard acquisition inside notification, four receiver errors, nested update ownership, and two persistent runtime recovery/revision/reopen cases. The persistent cases author four independent native SampleTrack/LFO/mixer/ProjectRenderer stereo WAV decodes with sample-rate/finite/RMS checks. The shared-memory checks inspect native LMMS data, not a running VST host. Every native C++/Qt/thread/ABI/shared-memory/DSP/PCM assertion remains uncompiled/unrun; lock checks depend on the CTest process timeout if a lock leaks.

Executed Python arithmetic passed ten valid and ten invalid meter vectors, two zero-beat cases and three binary32 controller mappings, without running C++. Source diagnostics pass 567 C++ delimiter files / 165 operations; the lexical slot scan covers 206 test C++/header files and thirty-nine candidates in twenty-eight files. CMake/compiler/Qt/LMMS prerequisites remain unavailable on PATH and all seventeen submodules remain uninitialized. Other native TimePos/cache/frame/bar/quantize/position arithmetic, malformed-meter inspection, unframed/foreign Qt receivers, arbitrary getter retirement and partial direct side effects retain limits. Full core/application/plugin/MOC/UIC/link/ABI/sanitizer/GUI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/audio/MIDI/backend/hardware validation, CPU/RSS/realtime/atomic/latency/deadline evidence and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain required. The full commission stays active and unproven.

### Ordinary capability work and cancellation publication (2026-10-15, source only)

Review of the new readback cancellation cases exposed a direct-adapter gap: argument validation temporarily installed the supplied token, but the ordinary execute fallback discarded it before native dispatch. Existing runtime outer scopes could mask this. The ordinary fallback now retains the token through dispatch, checks before/after the native action and after dirty publication, and suppresses cooperative event polling at those boundaries. Weak Song identity/project-generation checks surround each callback to refuse an old result after project replacement. Existing specialized operation branches and nested native work keep their own publication/event policies.

This preserves the distinction between native adapters and the application's durable runtime. Direct cancellation can reject success after native effects were applied; StudioRuntime owns checkpoint restoration and suspends cancellation for mandatory rollback. Original native errors retain their origin. Cooperative checks do not interrupt an executing plugin, codec, system call or blocked native getter, and do not make arbitrary callback mutation or foreign-thread access safe. Scope/probe overhead, repeated budget sampling, CPU/RSS/realtime/latency and deadlines remain unmeasured.

StudioCapabilityCancellationTest authors eight groups / fifteen scenarios: four post-getter direct cancellations with no outer scope, two callback failures, two late mutation cancellations, two simultaneous native-error/cancellation cases, nested source restoration, final-probe project replacement, outer queued-event suppression and two durable rollback/replay/reopen cases. Native sample payload assertions check restored editable content alongside tempo/gain, retired clip identity, history/cursor/revision and durable failure receipts. Tests use the existing private dirty-state setup helper; Qt observers remain nonthrowing and fault probes run after Qt returns. Every native fixture remains uncompiled/unrun; no DSP/PCM or provider execution is established.

Source diagnostics pass 565 C++ delimiter files / 165 operations; the lexical slot audit scans 205 test C++/header files with thirty-nine candidates in twenty-eight files. CMake/compiler/Qt/LMMS prerequisites remain unavailable on PATH and all seventeen submodules remain uninitialized. Full core/application/plugin/MOC/UIC/link/ABI/sanitizer/GUI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/audio/MIDI/backend/hardware validation, compatibility, performance, and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain outstanding. The whole DAW commission remains active and unproven.

### Agent-visible invalid parameter readback and native tempo repair (2026-10-15, source only)

Live parameter inspection now preserves repair discovery when a controller returns an invalid number or throws. Shared readback reports finite raw observations, actual IntModel/BoolModel effective conversion, and valid/nonfinite/unrepresentable/outside-range/unavailable states. Hosted FloatModels advertising integer metadata retain float semantics. Getter errors have a 1024-byte UTF-8 input bound; null values have explicit states. Selected-model/Song weak checks and project generations are verified around getter/work boundaries, with cooperative cancellation still rejecting the inspection and event dispatch suspended. Strict native Song/model getter contracts remain in force.

Project summary and full inspection expose tempoReadback with the parameter handle, stored manual value and direct-controller flag. Invalid tempo is null; healthy tempo remains the effective native integer. StudioRuntime's existing summary observation therefore has an authored recovery path into the reasoning/tool loop. Agent instructions require inspecting the parameter and linked peers before explicitly disconnecting a bad binding. A peer can own the source even when the tempo model has no direct binding; local disconnect then remains a no-op.

The existing native disconnect avoids a bad live read, publishes the detached state, and notifies using retained manual state. Runtime uses its last committed checkpoint before mutation, checkpoints only after success, and restores committed native content on a failed notification. No new capability or automatic destructive repair was added. Getter adapters in fixtures are attached after a healthy durable baseline, then removed before successful checkpoint; the fixtures do not demonstrate portable serialization of malformed controller sources.

StudioParameterReadbackTest authors eight groups / twenty-one scenarios spanning healthy conversion/host metadata, seven malformed tempo cases, mixed parameter pages, inherited binding ownership, two cancellation checks, two scripted chat/responses inference loops, three durable repair successes and two one-shot notification failures. Authored history/cursor/revision/replay/reopen assertions cover the durable cases; five cases author eight independent native SampleTrack/LFO/mixer/ProjectRenderer stereo WAV decodes with sample rate, finite content and RMS checks. Every native fixture and scripted agent transition remains uncompiled/unrun; no real provider was contacted or autonomous musical result verified.

Source checks pass 564 C++ delimiter files / 165 operations. The lexical slot audit covers 204 test C++/header files with thirty-nine candidates in twenty-eight files, not a proof of transitive exception containment. CMake/compiler/Qt/LMMS prerequisites remain unavailable and all seventeen submodules remain uninitialized. Other native metadata, meter, master volume/pitch, track, mixer and plugin reads retain their existing failure contracts; nonfinite stored state can still refuse disconnect. Weak checks do not pin executing getters/callees/parents/roots, and faults can leave native side effects. Full native C++/Qt/MOC/UIC/link/ABI/sanitizer/GUI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/audio/MIDI/backend/hardware verification, CPU/RSS/realtime/latency/deadlines and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain required. The full DAW commission stays active and unproven.

### Integral parameter admission and tempo narrowing (2026-10-15, source only)

Native castValue now validates standard integral representations before the native cast. Nonfinite integer/Boolean inputs refuse with domain_error; non-Boolean targets truncate in double and compare with exact lower and constexpr binary exclusive upper bounds, refusing out-of-range results. This avoids undefined unrepresentable [floating-to-integer conversion](https://eel.is/c++draft/conv.fpint) while retaining defined fractional truncation, including unsigned zero from negative fractions. Finite typed Boolean zero/nonzero behavior and the legacy rounded Boolean form remain. Nonfinite Boolean refusal is an added model policy rather than a claim that C++ Boolean conversion was undefined; [its ordinary zero/nonzero rule is defined](https://eel.is/c++draft/conv.bool). Floating targets retain previous propagation.

Song's const/nonconst tempo reads and tempo update now share checkedTempo before bpm_t narrowing. This prevents an int-representable controller result such as 65536 plus a normal tempo from wrapping into a valid-looking 16-bit value. Valid property setter fitting and API signatures remain; no fields, virtual interfaces or studio operations were added. Core/application/plugin rebuild is necessary for template/accessor changes.

NativeParameterConversionTest authors twelve groups / eighty-seven scenarios covering standard signed/unsigned boundaries through 64 bits, fractional and Boolean compatibility, nonfinite/range refusal, float-rounded INT_MAX metadata, stored-value preservation/repair, linked reads, Song tempo/meter Qt return/clock/ownership and the tempo wrapping case. Three actual LFO/mixer/AudioEngine rows author fault latching, silent repeated periods, explicit reset and finite recovery. Six native direct-renderer/studio rows author preserved destinations and six independent finite stereo rate/RMS WAV decode checks after SampleTrack/LFO/mixer export repair. Every native fixture remains uncompiled/unrun. Python arithmetic passed twelve fixed binary32 boundary vectors, eight fractional vectors and rounded INT_MAX evidence; this is vector verification, not C++/Qt/DSP/PCM execution.

Source checks pass 563 C++ delimiter files / 165 operations; the lexical scan covers 203 test C++/header sources with thirty-nine candidates across twenty-eight files. Existing float storage remains, so exact full-width integer metadata/value persistence is unfinished: INT_MAX rounds to 2^31 and now refuses rather than invoking undefined conversion. This checks final representation, not arbitrary model/enum/packet/musical bounds; prior fitting/controller clamps and floating nonfinite propagation remain. Uninstrumented GUI/event/paint/foreign Qt readers and plugin casts need local contracts, and weak scopes do not pin native callees or reverse partial state. Malformed tempo repair can require binding detachment. Getter adapters are unpersisted and test PCM is not autonomous composition/listening. CPU/RSS/realtime/latency/deadlines, native build/Qt/MOC/UIC/link/ABI/sanitizer/GUI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/audio/MIDI/backend/hardware and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain outstanding. The full commission remains active and unproven.

### Nested tempo notification and ownership recovery (2026-10-15, source only)

The nested tempo path now contains its own returned failures. TempoSyncKnobModel's tempo/custom-meter Qt receivers catch/report into the current native value frame; custom delivery remains a typed member with UniqueConnection. Ordinary calculation/setSyncMode/setScale/updateCustom contracts still propagate errors. Active synchronization validates positive tempo/scale/custom meter and finite positive conversion before fitting the parameter. Existing valid factors remain, while invalid zero/nonfinite/overflow conversion is refused. Mode/scale emissions have reporting scopes and the last-mode marker commits after successful delivery.

Song's model receivers for changed/unchanged tempo, meter and master gain catch/report; its engine-rate frame-clock receiver reports device notification failures. Tempo processing replaces manual model-lock pairing with scoped ownership and per-note unlock, checks returned Song/engine context and range, releases ownership before frame-clock/VST/Qt publication and frames the tempo emission. Both SampleClip constructors contain tempo/meter receivers. VstPlugin's normal tempo/rate receivers report through native value/device frames; four tempo-sync GUI callback paths catch/report or diagnose using weak model bindings. This closes concrete nested-return and lock-leak paths without adding studio operations, virtual APIs or per-model callback/exception fields.

TempoNotificationRecoveryTest authors seven groups / thirty-three scenarios across standard/nonstandard nested tempo/custom-meter failures, changed/unchanged delivery and journal restoration, UniqueConnection retry, first/second getter failures with separate-thread native lock acquisition, numerical refusal/repair, raw unframed diagnostics and mode/scale failure return. Two persistent rows author actual native LFO checkpoint rollback/model replacement/history/cursor/revision/receipt replay, subsequent revision, restored mixer binding and close/reopen/replay/render. Four finite stereo SampleTrack/LFO/native mixer/ProjectRenderer/WAV artifacts are independently decoded across those two scenarios. All native assertions remain uncompiled/unrun. The CTest timeout bounds lock-regression tests only, not production preemption or latency.

Source validation passes 562 C++ delimiter files / 165 operations. It now inventories committed/staged/local C++ additions/modifications from the documented a2f57e70c upstream baseline, plus untracked files, and rejects empty coverage. This repairs the working-tree-only check that briefly returned zero after a checkpoint. Full baseline diff checking found seven extra EOF blank lines in existing fixtures; removing them changed no test logic. The lexical audit covers 202 test C++/header sources and retains thirty-nine candidates in twenty-eight files, including locally caught throws. Counts and structural checks are not compilation or transitive semantic safety proof.

Core/application/plugin/MOC rebuild and native Qt/UIC/link/ABI/thread/sanitizer/GUI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/PCM/MIDI/backend/hardware and real-provider musical-production evidence remain outstanding. ALSA's existing errno clock path stays separate and no real VST/remote host was exercised. Scoped locks and weak returned-context checks do not pin executing notes/getters/parents/roots, fix all integer conversion/note-resize arithmetic, validate unknown sync enums or restore arbitrary partial callback/plugin/clip/clock/journal effects. Raw/meta-object/private-slot/unrelated outer Qt paths need their own containment; reports lack origin tokens for queued/overlapping work. Snapshot/Qt/weak/TLS/refcount/getter/plugin/GUI/codec/cleanup costs remain cooperative and unmeasured for CPU/RSS/realtime/latency/deadlines. The full autonomous DAW commission remains active and unproven.

### Native value failure frames and LFO timing (2026-10-15, source only)

NativeValueNotification is a shared internal stack scope for synchronous value-notification failure transport. Receivers catch locally and report the first exception_ptr; emission rethrows only after Qt delivery completes. A thread-local scope chain restores on normal return/unwind without dereferencing QObject roots. Reports apply to the current emitting thread, including real render threads, without asserting QObject owner affinity; empty/outside reports reject and another thread cannot reach that frame. No per-model/controller callback/exception storage, virtual API or DAW tool was added. The earlier private model probe remains separate and runs only after production receiver reports have been accepted without error.

The scope now surrounds explicit AutomatableModel changed/unchanged delivery and its native value/topology journal callbacks, Controller periodic updates, ControllerConnection rewires and ordinary/autodetected MIDI CC notifications. Signal-to-signal forwarding from these native origins preserves that scope. LFO waveform/speed/multiplier receivers catch/report locally, and its engine-rate receiver uses the existing AudioEngine reporting frame. The normal MIDI autodetection GUI receiver also catches/reports or diagnoses unframed work. This repairs real getter-error paths, while foreign receivers and outer Qt callers still need their own local containment.

LFO duration calculation now validates returned weak LFO/engine/rate context after each native control getter, rejects nonfinite/nonpositive duration before commit and retains existing float multiplier arithmetic. A successful nested native device change cannot be overwritten by a stale outer duration. Direct ordinary calls can still throw; unframed rate delivery diagnoses locally. These checks do not pin an executing getter/callee/parent, sanitize all native parameter conversions or turn callbacks into graph transactions. Stored model, binding, packet, phase, journal and external effects can remain after direct failure.

NativeValueNotificationTest authors four groups / twenty-three scenarios across six actual emitter families, first-error identity after later observers, nested setters, reporting scope/thread behavior and precedence over private probes. LfoRateNotificationTest authors nine groups / thirty-seven scenarios spanning getter/numerical errors, model/controller/rate notification return, export setup, retained restoration and phase retry, unframed/direct-call contracts, a nested actual WAV device switch and valid native rate/multiplier arithmetic. Three rows author actual AudioEngine MixException latching, silent repeated periods, preserved clock, explicit reset and finite repaired playback. Nine scenarios author twelve independently decoded real SampleTrack/LFO/mixer/ProjectRenderer/WAV artifacts; a controlled unpersisted getter adapter supplies faults without replacing LFO DSP. The existing clock fixture now catches its fault inside Qt and verifies later delivery before returned failure/retry. Every native C++/Qt/thread/model/DSP/PCM/GUI assertion remains uncompiled/unrun.

Source validation passes 559 C++ delimiter files / 165 operations. The lexical audit scans 201 test C++/header files and retains thirty-nine candidates in twenty-eight files, including locally caught clock/lifecycle slots. Complete core/application/plugin rebuild for new exported internal symbols, Qt/MOC/UIC/link/ABI/thread/sanitizer, GUI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/PCM/MIDI/backend/hardware and real-provider musical-production evidence remain necessary. Raw/manual/meta-object/subclass/unrelated signals can bypass the scope, and reports carry no origin token for late/queued or unrelated overlapping work. Stack TLS/exception-handle work, native weak snapshots, visited lists, Qt/plugin/getter/allocation and cleanup costs remain unmeasured and cooperative without hard deadlines. Whole DAW acceptance remains unproven; the full commission stays active.

### Controller lifecycle receiver containment (2026-10-15, source only)

Native Song controller added/removed/roster delivery now establishes a thread-local, stack-owned reporting frame with weak Song identity and first-error exception_ptr retention. Synchronous owner-thread receivers catch their own exceptions and report into the nearest live same-Song frame. Empty/outside/foreign-thread reports reject; nested frames restore their predecessor without accessing a retired Song. Native propagation happens after Qt delivery finishes. No per-Song callback/exception storage or studio operation was added.

Three ControllerRackView lifecycle receivers now catch/report. Add and queued Remove callbacks catch/report or diagnose locally, keeping ordinary native errors inside their outer Qt callback as well. Pending view construction uses weak cleanup and publishes vector/index state after layout insertion. Song's destruction roster hook also catches locally. Explicit removal retains late destruction-time reports in an outer native frame and rethrows after the destructor returns; independent deletion diagnoses unframed failure locally. This is a narrow lifecycle contract, not universal Qt/destructor safety.

The two existing lifecycle fault methods now use local catches/reporting, preserving six authored scenarios. Four persistent standard/nonstandard add/remove rows additionally reopen the studio, replay cached failures and verify restored rack/binding/value/tempo. A new QCoreApplication SongControllerNotificationTest authors six groups / twenty-three scenarios: first-error identity and later observer delivery for all three notification kinds, nine nested outcomes, rejected empty/outside/foreign reports, explicit removal/direct deletion, payload retirement and blocked notification/frame cleanup. Every native fixture and actual GUI path remains uncompiled/unrun; no DSP/PCM or production timing proof is claimed.

Source checks pass 555 C++ delimiter files / 165 native operations. The lexical audit scans 199 test C++/header sources and retains thirty-nine candidates in twenty-eight files, including a controller lifecycle slot whose explicit throws are now locally caught. Counts cannot replace semantic connected-callee review. Complete core/application/plugin rebuild, actual GUI partial-allocation/refresh, Qt/MOC/UIC/link/ABI/thread/sanitizer, existing project/preset/clone/controller/automation/instrument/PatternStore/plugin/DSP/PCM/MIDI/backend/hardware execution and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain outstanding.

Weak frames do not pin native callees/parents/roots, prevent same-address raw payload reuse/concurrent mutation or restore arbitrary callback/view/plugin/external side effects. Direct native edits can remain partial; persistent checkpoint recovery remains separately unproven. Foreign receivers, retirement listeners, modified notifications and arbitrary direct/meta-object entry points still need local containment. A queued receiver cannot report into a completed transition; unrelated overlapping same-Song reporting frames have no origin token. Native/Qt/layout/plugin/allocation/cleanup work remains cooperative without measured CPU/RSS/realtime/latency or hard deadlines. Whole DAW acceptance remains unproven and the commission stays active.

### Native model notification fault isolation (2026-10-15, source only)

Explicit AutomatableModel changed/unchanged notifications and NativeControlLinkJournal replay now share a protected dispatcher with a private thread-local test-peer installation. No callback field was added to each model. The uninstalled branch emits/returns before probe copying or checkpoint weak-reference construction. Installed dispatch owns a callable copy before Qt delivery, records the initial signal block and weak receiver, then invokes the checkpoint after delivery returns only for a surviving, initially unblocked receiver. The weak-target adapter supports optional unchanged delivery and same-thread lexical/LIFO installation/reset. Removing an installation during Qt delivery does not cancel the already retained invocation.

Twenty-four methods across ten existing suites now use this ordinary-call fault boundary. They cover native setter initialization/attachment/tempo, linked writes and value/topology replay, persistent startup/rollback/commit/undo/replay/session ownership and agent replanning, rack/MIDI assignment/disconnection and learned publication. Eighteen explicit and six indirect helper-based throwing connections were removed. Nonthrowing controller/MIDI retirement and mutation rows still use actual Qt observers. Prior claims about these fault paths are superseded by the repaired fixture contracts; all remain uncompiled/unrun.

ModelValueNotificationTest authors eight groups / twenty-three scenarios: all direct observers finish before standard/nonstandard faults return to manual/automated setters, unchanged/blocked semantics remain, peer faults stop later propagation and allow retry, observer/self-removal retains callable lifetime, retired receivers skip pending invocation, nested scopes restore their predecessor and worker installations stay separate. Its detached model is allocated/registered and retired on the journal owner thread; automated-only updates run temporarily on the worker, followed by join and restored affinity. This is not concurrent journal coverage. No native execution, DSP or PCM evidence has been obtained.

Source validation passes 554 C++ delimiter files / 165 native operations. The lexical scan covers 198 test C++/header sources and retains thirty-nine candidates in twenty-eight files; indirect calls and local containment require semantic review. The dispatcher is a test seam, not production containment for escaping foreign-slot exceptions. If another Qt receiver called the model setter, that outer receiver still needs a local catch. Controller signal-to-signal forwarding, subclass/manual/meta-object emits and unrelated Model notifications bypass this helper. Same-thread lexical/LIFO scope ownership is required; pending copied invocation intentionally survives installation removal.

Complete core/application/plugin rebuild and C++/Qt/MOC/UIC/link/ABI/thread/sanitizer validation remain necessary. No per-model storage is added, but installed callable copies and native setters/journals can allocate; thread-local overhead, CPU/RSS/realtime/latency/deadlines are unmeasured. Weak receivers do not pin executing callees/parents/roots or restore arbitrary callback/plugin/external effects. Full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/PCM/MIDI/backend/hardware and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain open. Whole DAW acceptance remains unproven; the commission stays active.

### Song export transition ownership and pending cleanup (2026-10-15, source only)

Native Song startExport/stopExport now publish/release transition activity under the existing model-change mutex through a weak-Song/engine guard. Reentry rejects before the active transition's cleanup catch, and an already exporting song cannot restart. Initialization failure, including returned failure of the initial stop, clears exporting and records pending cleanup with MIDI admission updated. Existing audio.device.restore must recover that work before another direct export or fresh external MIDI admission; normal ProjectRenderer failure cleanup still attempts native stop/recovery. This closes the source gap where partially started playback could be exposed without pending recovery.

Four private native post-call points cover start timeline/playback and stop timeline/playback after Qt delivery returns. Probes retain their executing callable and install through a weak scoped test adapter; ordinary stop uses the timeline point without acquiring export recovery semantics. Three explicit direct-slot injections in TrackRenderScopeLifecycleTest/Sf2RenderAuditionTest and eight indirect helper-based injections in SongExportRecoveryTest now use these boundaries. Cleanup/model-lock/MIDI/audition/reservation assertions remain authored. The failed-start MIDI row now requires blocked input and two dropped events until explicit recovery, then accepts fresh input.

SongExportTransitionTest authors six groups / thirty scenarios across four standard/nonstandard fault phases and recovery, native checkpoint and playback-observer reentry refusal, active restart refusal, eight worker startup faults and eight transient/persistent worker cleanup faults through native manager/studio entry, preserving destinations and permitting retry. Sixteen retries author independent decoded finite stereo sample-rate/RMS PCM through native SampleTrack/ProjectRenderer/WAV. Every native fixture remains uncompiled/unrun. Source checks pass 552 C++ delimiter files / 165 operations. A lexical scan of 196 test C++/header sources now finds fifty-seven candidates across thirty-eight files; the additional indirect helper findings show why complete semantic connected-callee review is still required.

Native C++/Qt/MOC/link/ABI/thread/sanitizer/transport/callback/MIDI/DSP/PCM proof and full core/application/plugin rebuild for Song layout remain necessary. This protects export transitions, not arbitrary raw/direct transport/model/plugin mutation during DSP, immutable graph rollback, executing callee/global-root retirement or universal memory failure. Probes install/reset only with control ownership and workers stopped; invocation can occur on the actual render thread. Faults after returned native calls do not establish foreign-slot exception recovery. Fixture PCM/software MIDI does not prove physical delivery/silence or autonomous musical production/listening. Measured CPU/RSS/realtime/latency/deadlines, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain open; whole DAW acceptance remains unproven.

### Native notification failure frames and SoundFont delivery (2026-10-15, source only)

AudioEngine rate/quality notifications during initialization/setup/restoration now run inside a scoped owner-thread exception-reporting frame. Receiver-caught nonnull exception_ptr reports retain the first failure; later reports cannot replace it, and out-of-frame/foreign-thread reports are refused. A nested frame restores its predecessor on return/unwind. The native caller rethrows after Qt dispatch returns, preserving setup failure and noexcept restoration's NotificationException/reservation/retry handling without unwinding Qt signal delivery. Notification-completion flags remain after acceptance.

The normal native-engine SF2 sample-rate connection is now a typed receiver with a weak engine, local catch and report into that frame. Unframed delivery is diagnosed. Existing direct reloadSynth/resource behavior, committed-preview restoration, early retained-rate conversion and native candidate/synth/DSP remain; no extra DSP failure latch or substitute audio implementation was introduced. ProjectRenderer continues to refuse failed export preparation before worker startup. This repairs the normal production receiver gap found previously, while arbitrary direct/meta-object calls outside that receiver retain their original throwing contract and broader receiver auditing remains required.

Eight new AudioDeviceTransition scenarios cover first exception/type retention after later observers, nested inner/outer outcomes, frame release and outside/foreign-thread rejection; its suite now has twelve groups / thirty-six scenarios. Two new Sf2RenderAudition groups/five scenarios cover standard/nonstandard setup callbacks and snapshot/standard/nonstandard restoration faults, complete later-observer delivery, NotificationException ownership retention, explicit per-phase retry and decoded actual SoundFont retry PCM. Three retained-clock cases add observer-delivery assertions. Sf2RenderAudition now has twelve groups / twenty-four scenarios / ten decoded PCM scenarios, including required real module/runtime replay/checkpoint coverage. All C++/MOC/UIC/link/Qt/thread/FluidSynth/DSP/PCM fixtures remain uncompiled/unrun.

Source checks pass 550 C++ delimiter files / 165 operations. Native API/ABI/MOC/UIC/link/plugin-import/sanitizer/callback/audio execution still requires proof and full core/application/plugin rebuild for engine layout/API changes; Instrument virtual API and tool count remain unchanged. Frames assume live native owner-thread roots and do not establish queued/foreign/manual signal recovery, arbitrary foreign-slot/callee/global-root retirement safety, immutable graph rollback or universal memory-failure handling. Synchronous/nonpreemptible calls and unmeasured CPU/RSS/realtime/latency/deadlines remain. Sixty lexical test candidates across forty files plus indirect connected-callee review remain unresolved. Full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain outstanding; whole DAW acceptance stays unproven.

### Native post-notification test checkpoints (2026-10-15, source only)

Four private AudioEngine test-peer checkpoints run after native setup/restoration quality/rate Qt notification dispatch returns. Empty installation returns without callable copying; an installed probe is copied before invocation so self-reset/returned export-owner retirement does not destroy the executing functor. Restoration completion flags advance only after checkpoint acceptance. A scoped QPointer test adapter resets idempotently and restores a prior probe under its owner-thread, lexically nested contract. There is no new public studio operation, replacement codec/plugin/device or DSP. AudioEngine layout requires full application/plugin rebuild.

Seventeen methods in eight suites now raise faults at ordinary native checkpoints: RenderManagerPublication/Lifecycle, StudioRenderOwnership, AudioDeviceTransition, StudioExportSettings/Stem, MidiInputRender and StudioBackendSelection. Existing native PCM/output preservation/ownership/recovery assertions remain authored. Twelve new AudioDeviceTransition scenarios cover all four points with normal/standard/nonstandard outcomes, self-removal, retained callable lifetime/release, native failure/return identity, reservation retry and candidate destruction; that suite now has nine groups / twenty-eight scenarios. This supersedes the earlier named direct-slot fault limitation for those migrated rows, while foreign-slot exception recovery remains unsupported. Every native fixture remains uncompiled/unrun.

A source-only lexical scan covers 194 tests/src C++/header files and reports sixty connect-call arguments with explicit throw tokens across forty other files, none in the migrated eight files. These are semantic-review candidates, not proof of escaping exceptions or complete safety; indirect callees/getters/macros/native internals need separate audit. Production source inspection additionally found Sf2Instrument::reloadSynth connected directly to sampleRateChanged with throwing resource/preview/export-rebuild paths and no slot-local catch. Its signal delivery and existing export-failure fixture expectation still need repair. Private test checkpoints do not fix that production path, timeline cleanup or other model-notification candidates.

Source validation passes 550 C++ delimiter files / 165 operations, without native C++/Qt/MOC/link/ABI/thread/sanitizer/callback/codec/OS/PCM proof. Checkpoints occur after completed notification dispatch; they do not simulate exceptions inside Qt. Non-LIFO/foreign-thread probe mutation, arbitrary executing callee/global Engine retirement, universal memory recovery and measured CPU/RSS/realtime/latency are not established. Full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain open. Whole DAW acceptance remains unproven.

### Studio cancellation and reply exceptions outside Qt dispatch (2026-10-15, source only)

The studio render cancellation timer catches callback exceptions in its slot, retains std::exception_ptr, stops timers, aborts/joins the real renderer and rethrows after the event loop returns/worker joins. Inference's production HTTP cancellation and reply collector also catch inside connected slots, retain the first failure, stop timers, abort the real QNetworkReply and quit the loop before rethrowing in ordinary control flow. Failed/oversized/timed-out/aborted collection does no further reading. This preserves callback exception identity for native/direct and structured studio recovery without replacing DSP or changing provider protocol schemas.

[Qt's exception-safety contract](https://doc.qt.io/qt-6.8/exceptionsafety.html#exceptions-in-client-code) requires handling application exceptions inside signal-connected slots. Earlier inspection found direct-slot fault injection in RenderManagerPublicationTest and StudioRenderOwnershipTest; the native checkpoint continuation above replaces those injections in eight suites. Broader fixture and connected-callee audits remain necessary. The cancellation tests exercise exceptions caught inside the production slot and rethrown after dispatch returns.

StudioRenderCancellationTest authors twelve project/stem/direct/capability scenarios across boolean/standard/nonstandard callback outcomes, observing actual running native execution, one callback, failure identity, prior-output preservation, staging/preferences/context/device/scope recovery and independent decoded finite stereo sample-rate/RMS PCM on twelve native WAV retries. Test-only progress instrumentation pauses the real renderer between periods with a finite gate; it changes no audio samples or codecs/plugins/DSP. StudioInferenceTest adds four real loopback production-HTTP generation/audio-request cases with standard/nonstandard callback failure, connection release and retry; its suite now has nine case groups / twenty-one expanded scenarios. The loopback fixture disconnects member-capturing slots before retiring fields. All native tests remain uncompiled/unrun; loopback payloads/results do not establish real-provider listening or reasoning.

Source validation passes 549 C++ delimiter files / 165 native operations. Native C++/Qt/MOC/link/ABI/thread/sanitizer/OS/HTTP/codec/PCM execution remains required. No allocator-fault proof, universal memory-exhaustion recovery, foreign-slot/native abort/wait/destructor failure containment, arbitrary callee retirement/global teardown, blocked-call preemption or measured CPU/RSS/realtime/latency/deadline is established. The three-second test gate is synchronization only. Full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware, optional-codec regressions and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain outstanding; whole DAW acceptance remains unproven.

### Studio render recovery and publication identity (2026-10-15, source only)

StudioAudio now owns a weak engine/Song identity, project replacement generation/filename and accepted audio reservation generation across native audio.render. Returned admission/asset/cancellation/finalization/restoration/publication continuations check that identity, and the timer aborts when it changes. Destructor cleanup follows stack renderer/worker retirement and stem-scope release, restores only the same reservation/preferences, rechecks after native restore callbacks and removes the temporary pathname only when its engine/reservation generation remains current. A newer scope reusing that pathname cannot lose its output to old cleanup; uncertain leftovers deliberately remain. QSaveFile direct-write fallback is explicitly disabled. Execution continues through the full native ProjectRenderer and selected codec/DSP.

StudioRenderOwnershipTest authors eight case groups / up to twenty-nine scenarios with all codecs enabled (twenty-five with WAV/FLAC): enabled-format project/stem success with digest/preferences/PCM, project clear/filename change during restoration/publication, separate/reused stage paths under newer reservations through own and outer file-work callbacks, renderer destruction acquiring new ownership after setup failure, three interruption classes for project/stem, transient/persistent restoration failure, post-commit accepted/new output preservation and two admission mutations. Up to fifteen scenarios author independent decoded finite stereo sample-rate/RMS PCM (eleven with WAV/FLAC), including two decodes in the post-commit case. Enabled codec/decoder failures fail without QSKIP. The earlier renderer-failure fixture now omits unsupported bitDepth in lossy studio requests. Every native fixture remains uncompiled/unrun.

Source validation passes 548 C++ delimiter files / 165 native operations. Weak/generation ordering checks are not native C++/Qt/MOC/link/ABI/thread/sanitizer/codec/OS/callback/PCM proof. This protects returned control-thread continuations, not deep graph immutability/rollback, arbitrary foreign/raw ownership mutation or retirement inside executing callees/constructors/destructors/global Engine teardown; the DSP worker still requires live authoritative roots/device. Supersession skips old preference restoration and retains uncertain staging for owner recovery. Per-file commit and UUID naming do not establish crash/batch/durability/capacity atomicity, hostile-link isolation or staging authentication; later destruction callbacks do not undo accepted files. CPU/RSS/realtime/latency/deadlines and actual native IO failures remain unmeasured/unrun. Optional-codec preservation and full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware plus real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain outstanding. Whole DAW acceptance remains unproven.

### Native renderer failure observation and format bounds (2026-10-15, source only)

ProjectRenderer validates format indices before native codec-table access and factory invocation, refusing negative/sentinel/out-of-range values without opening requested output. Invalid extension queries return an empty QString. Supported formats continue through their full native factories, and build-disabled optional formats remain unavailable without fallback.

A sticky atomic encoding-failure observation is populated before device installation, after returned setup, at rendering period boundaries and after native finalization. Known pre-installation failures refuse device installation; setup failures refuse worker startup; worker failures stop subsequent periods. The new post-write check runs before progress emission. Cached status remains readable after native device restoration/deletion without accessing the old file device. RenderManager and StudioAudio now distinguish encoding/output-writing failure from processing exceptions, DSP/plugin failure and cancellation. Existing per-file staging preserves prior destinations and cleanup permits explicit fresh attempts.

ProjectRendererFailureTest authors nine case groups / up to forty-two scenarios with all codecs enabled (twenty-four with WAV/FLAC alone): five invalid enum values through renderer/manager, enabled native construction, disabled optional-codec refusal, failed input before installation and during setup with no worker startup, worker failure with no later progress, pre-finalization refusal retained after recovery, three manager input-fault phases with prior-output preservation/staging cleanup/retry, and studio failure without an artifact followed by retry. Up to sixteen retries author independent decoded finite stereo sample-rate/RMS observations (eight with WAV/FLAC). Faults invoke production native encoders with invalid input, never replace DSP or codecs. Enabled construction/decoder failures fail instead of QSKIP. All native fixtures remain uncompiled/unrun.

Source validation passes 547 C++ delimiter files / 165 native operations, including bounds and check ordering. Controlled input faults do not establish real disk-full/short-write/permission-loss/close-failure, library crash or out-of-memory recovery. Cooperative checks cannot interrupt native calls or establish arbitrary foreign mutation/reentry/callee retirement/global teardown safety; CPU/RSS/realtime/latency/deadlines remain unmeasured. ProjectRenderer layout requires full application/plugin rebuild. Optional codec preservation still needs enabled builds and MP3 strict independent decoding support. Native C++/Qt/MOC/link/ABI/thread/sanitizer/OS/PCM, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve evidence remain outstanding. Test fixture samples do not establish autonomous musical production or listening; whole DAW acceptance remains unproven.

### Native file encoder admission and packing ownership (2026-10-15, source only)

AudioFileDevice now admits nonempty buffers before native packing allocation or int-narrowed calls: output must be initialized/open, without recorded failure or finalization, input must be nonnull with finite available stereo channels, and frame/channel/float-byte multiplication must fit native int. Zero writes do nothing; OGG privately sends end-of-stream only during finalization. Invalid zero-channel/zero-rate/above-int-rate construction refuses before output opening/truncation. This is a per-write representability boundary, not a maximum export length or proof of memory capacity behind a caller pointer.

Native WAV/FLAC float packing and shared S16 conversion in both endian modes zero-pad extra channels beyond stereo SampleFrame storage. Shared conversion affects hardware output too and needs backend regression execution. WAV/FLAC own retained float/integer packing vectors; MP3 owns retained interleaved/encoded vectors with integer LAME capacity arithmetic and a fixed bounded flush array. Existing mono/stereo scaling, codec settings and FLAC negative-clip handling remain. OGG uses a QUuid-derived stream serial, removing explicit srand/rand mutation; opaque dependency RNG behavior and stream metadata remain unverified.

NativeAudioEncoderTest authors ten case groups, up to sixty-two scenarios across all four enabled codecs (thirty-five with WAV/FLAC only), fourteen independently decoded PCM scenarios and five packing-storage observations. Coverage includes zero writes, closure/rename/idempotence, five invalid-input classes, late-write byte preservation, six multichannel format/depth cases, both endian conversions, failed initialization, C RNG sequence, full native RenderManager publication and three invalid initialization classes preserving prior bytes. Enabled-codec initialization/independent-decoder failures fail instead of skipping PCM proof; disabled optional formats have no rows and do not prove full default feature preservation. Enabled MP3 needs independent libsndfile decoding support. All C++ fixtures remain uncompiled/unrun.

Source validation passes 546 C++ delimiter files / 165 native operations and Python boundary arithmetic for one/two/four/65535-channel packing plus stereo LAME capacity. Retained vectors do not establish allocation-free native libraries or measured CPU/RSS/realtime/latency; finite checks add synchronous work. Buffer growth/native allocation, trusted raw pointer capacity, direct nonfile conversion callers, arbitrary reentry/callee retirement/global teardown and process-wide memory exhaustion are not universally covered. Full native C++/Qt/MOC/link/ABI/sanitizer, library/OS closure, decoder/channel ordering/delay/resampling/audio execution and application/plugin/codec rebuild remain necessary. Complete GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware, real-provider listening/revision and autonomous musical-production acceptance remain unproven.

### Native per-file export staging and commit (2026-10-15, source only)

Native RenderManager CLI/GUI output now passes through a separately owned adjacent QTemporaryFile before QSaveFile publication with direct-write fallback disabled. ProjectRenderer and the chosen native codec remain the execution path. AudioFileDevice finalization flushes/closes output idempotently after codec completion, allowing a closed staged file to be read before bulk rendering installs its next file device. Destination bytes remain untouched until commit, including when a native creation boundary throws after opening its requested staged output.

The publication copy uses existing bounded FileResourceIO streaming and validates weak manager/renderer, DSP state, reservation generation and Song identity/generation/filename before commit. Final-file publication follows successful device restoration; intermediate bulk stems can commit before the next stem. This preserves per-file partial-batch behavior, with earlier accepted stems retained on later failure and no undo of an already committed file after late cancellation. Returned owner retirement cancels an uncommitted destination. Staging ownership remains independent of the manager's lifetime, and temporary cleanup checks engine/reservation identity so a newer reservation reusing the old staging filename cannot lose its output to old cleanup.

Ten QCoreApplication cases / sixteen scenarios are authored for project/stem replacement, unchanged destination during setup, setup exceptions, DSP refusal, three publication interruption classes, owner retirement during copy, restore failure and retry, partial bulk success, WAV16/24/32 close/rename/idempotence, destination-directory preservation and base-file late-write refusal. Seven scenarios author independently decoded PCM. A lifecycle row additionally reuses a staging pathname under a newer reservation; older retirement rows decode staging before retirement and refuse uncommitted destination publication. Native tests remain uncompiled/unrun.

Source checks report 545 C++ delimiter files / 165 native operations. This is per-file atomic publication rather than a batch/project transaction or crash journal. Uncertain temporary cleanup may leave artifacts after persistent device failure or superseded reservation. Additional staging/copy disk space and O(encoded bytes) IO remain, with no measured CPU/RSS/latency or hard deadline. Native Qt/codec/filesystem/device/destructor calls remain synchronous/nonpreemptible; hostile-link isolation, same-size staging tamper authentication, durability and OS sharing are unproven. The encoder continuation above additionally authors closure/PCM checks for all enabled codecs; every native WAV/OGG/MP3/FLAC execution remains outstanding. Core/plugin/codec rebuild is required for layout and finalization changes. Full native C++/Qt/MOC/link/ABI/sanitizer, callback/atomic-replace/audio/hardware execution, default GUI/CLI/plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend compatibility and real-provider autonomous composition/listening/revision acceptance remain open.

### Native renderer creation containment and queued identity (2026-10-15, source only)

The manager now catches exceptions and null results at the native renderer creation/setup boundary, joins any returned renderer before clearing a failed bulk queue and routes failure through native completion/device/scope cleanup. A translated message cached at manager construction avoids building exception text in the allocation-failure catch path. Explicit cancellation and setup failure remain distinct; retry resets both. Factory-time cancellation prevents worker startup, and reservation identity is rechecked before a returned renderer is adopted for execution. The protected factory defaults to the complete production ProjectRenderer and changes the manager's virtual interface; application/plugin rebuild is required.

Progress and finished callbacks now use queued weak renderer identities and compare them with current member ownership before forwarding progress or advancing a job. A retired renderer's posted callback cannot affect a replacement job on the same manager. These are weak queue identities rather than extra owning references, preserving timely renderer/file destruction after executing frames have returned.

Eight QCoreApplication cases / twenty scenarios are authored at the factory boundary: precreation allocation/standard/nonstandard/null project/stem failures preserving existing bytes, two post-native-creation exceptions with actual renderer destruction and retry, four same-manager precreation repair/render rows, returned/throwing factory owner retirement, pre-return cancellation, nested start refusal, newer reservation protection and a failed bulk queue stopping until explicit retry. Eight scenarios author independently decoded actual SampleTrack/ProjectRenderer PCM; successful paths delegate to the native factory. One lifecycle case additionally queues old progress/completion signals before a failed setup then runs a replacement, checks exactly two completions and excludes the old progress sentinel. All native fixtures remain uncompiled/unrun.

Source checks report 544 C++ delimiter files / 165 native operations. Typed boundary faults do not prove process-wide memory exhaustion recovery or failures within every Qt/codec/library constructor; cached messages reduce diagnostic allocation while signals, native recovery and destruction may still fail. Factories must return newly owned unstarted native renderers. The publication continuation above now owns staging independently of returned renderer creation and commits per file; native filesystem and complete regression proof remain outstanding. Arbitrary executing callee self-retirement, foreign QObject mutation, global engine/root teardown and deep immutable project state remain outside universal guarantees. Full C++/Qt/MOC/link/export/vtable/ABI/sanitizer, actual shared/queued lifetime/native DSP/PCM execution, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware compatibility, CPU/RSS/latency/deadline and real-provider autonomous composition/listening/revision evidence remain required for the complete DAW commission.

### Native renderer pins and audio-device reservation generations (2026-10-15, source only)

RenderManager now shares ownership of its native ProjectRenderer with local executing frames rather than allowing manager retirement to delete an executing setup/finalizer. Retirement marks cancellation before detaching owned renderer/selection state, aborts even pre-worker setup, joins before releasing processing flags and drops renderer context before native device recovery. Returned setup/finalization/abort/progress continuations check weak manager survival, and starting/finishing cleanup no longer holds raw references to retired owner fields. Cleanup of a retired frame retries the existing native restoration path under suspended asset cancellation; persistent failures remain explicitly recoverable through audio.device.restore. It preserves finalized native files without claiming manager completion and removes owned unfinalized output after closure when possible.

Finalization, restoration and renderer QObject destruction now share a finishing phase that rejects nested starts and rechecks owner/cancellation before completion. Old guards are dismissed before finished may synchronously reuse the same manager. Setup hands a returned renderer back to member ownership before normal failure cleanup, allowing unattached file devices to close before Windows removal. AudioEngine accepted stores allocate monotonic uint64 reservation identities with overflow refusal; manager and orphan cleanup compare engine identity and generation before restoring a device or removing partial output. A destruction callback that releases an old scope and creates a later reservation/native output cannot give that newer scope to the older cleanup frame.

Fifteen QCoreApplication cases / twenty-three scenarios are authored for quality/rate project/stem setup retirement, standard/nonstandard setup errors, owner retirement during restoration, late cancellation, renderer destruction retiring/cancelling the manager, transient/persistent restoration faults and explicit recovery, synchronous same-manager completion reuse, nested restoration refusal, replacement reservation/output ownership, accepted/refused generation allocation, progress retirement, unattached DSP-refused file closure and exclusion of old queued progress/completion during replacement. Nine scenarios author independently decoded native SampleTrack/ProjectRenderer PCM, plus two decodes of native AudioFileWave replacement output. One additional real RemotePlugin fault-host case retires the manager from inside a hung native rate-setup wait; RemotePluginTest now contains twenty-three cases. Native shared renderer survival/destruction, device status, context exclusion and independent thread model-lock acquisition are asserted. Every native fixture remains uncompiled and unrun.

Source checks report 543 C++ delimiter files / 165 native operations. AudioEngine/RenderManager layout changes require a complete application/plugin rebuild. This is finite owner/reservation handling, not immutable composition or rollback. Reservation IDs cover accepted storeAudioDevice scopes; arbitrary direct unreserved device changes, native constructor/callee/getter/destructor internals, foreign QObject mutation and global engine/root teardown remain outside universal guarantees. The creation continuation above contains returned creation/setup faults in source; actual allocator/library constructor/finalizer behavior and process-wide memory exhaustion remain unverified. Recovery and partial-file removal are best effort, finalized files can outlive a missing completion receipt, and the publication continuation above adds staged per-file commit in source, without batch atomicity. Native DSP/host/library/codec/destructor waits remain nonpreemptible, with no hard deadline or CPU/RSS/latency/realtime/hardware evidence. Full native C++/Qt/MOC/link/export/ABI/sanitizer, signal/worker/host-platform/shared lifetime execution, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend compatibility and real-provider autonomous musical/listening/revision acceptance remain open.

### Native bulk export preparation and weak manager ownership (2026-10-15, source only)

RenderManager's CLI/GUI bulk preparation now owns weak Song/PatternStore track/model snapshots across effective mute getters and asset polling. It validates weak manager/engine/roots/thread, Song generation/project filename/output clock, export/remote/pending cleanup and stored mute/solo values, then checks complete final roster membership/order before publishing eligibility. It retains native instrument/sample eligibility, reverse queue order, mute/solo values and previously-admitted behavior across earlier stems' automation. No borrowed container iterator crosses a preparation callback.

Filename preparation captures directory and format values before its virtual getter, validates returned native ownership before copying the borrowed QString, and polls cancellation around the owned result. Per-track scope construction remains local until manager survival/cancellation checks pass. Weak starting cleanup replaces raw owner references during eligibility/selection/naming; abort now records cancellation before any renderer exists, and nested starts cannot publish a second queue. Standard/nonstandard preparation failures use existing completion/error/restoration paths. Manager retirement during the supported returned preparation callbacks leaves only local weak continuations/scope ownership to unwind.

Twelve QCoreApplication cases / thirty-one scenarios are authored against the native manager and project model: sample stem numbering/filtering, getter-driven previous/future row retirement/add/reorder/project/model changes, getter exceptions and manager retirement, cancellation/nested starts, first/final asset callback retirement, file cancellation/exception classes, name getter selected/peer/manager/project failures, naming cancellation, manager retirement during local selection construction, same-manager explicit repair/retry and Song/PatternStore eligibility. Three scenarios author independent libsndfile decoding of actual native SampleTrack/ProjectRenderer audio. Queue and raw atomic checks avoid export-gated public flag getters, and independent thread acquisition checks returned model ownership. None of the native fixtures has been compiled or run.

Source checks report 542 C++ delimiter files / 165 native operations. This changes RenderManager layout and needs a complete rebuild. The source guard is finite preparation validation, not an immutable deep composition/plugin/controller/automation transaction or general rollback; an unchanged final state after transient edits can be admitted. Snapshot/collection and per-stem validation/name work add synchronous O(n) costs with no CPU/RSS/deadline/latency evidence. The publication continuation above adds staged per-file commit in source, without batch atomicity. The renderer-lifetime continuation above replaces member-only setup ownership and the reference rollback in source; native execution and broader lifetime proof remain outstanding. Asynchronous finalize/restoration/progress/destruction, arbitrary executing native getter/callee internals, foreign QObject mutation and global root/engine teardown remain outside this guarantee. PatternStore rows inspect membership rather than decoded playback; sample output does not prove instrument-plugin audio. Full native C++/Qt/link/ABI/sanitizer, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware and real-provider autonomous musical/listening/revision acceptance remains open.

### Native stem admission and owned roster snapshots (2026-10-15, source only)

Native per-track export admission previously polled FileResourceIO while traversing raw container iterators and could later dereference retired weak exclusions when publishing flags. TrackRenderScope now snapshots Song and PatternStore rosters as owned QPointer identities before cancellable work. The existing application-thread/model guard and event suspension remain; weak engine/root/selected actor and mute/solo model checks also retain Song generation, project filename, output clock and stored selected eligibility. Returned asset callbacks and effective mute getters must preserve that context, and pending cleanup now refuses admission even if Song's export flag has cleared. Periodic polling traverses only the owned snapshots. A final O(n) membership/order/owner comparison precedes all atomic selection/exclusion publication, avoiding a full roster rescan at every checkpoint. Failure unwinds active admission/model ownership without changing flags; callback side effects remain native state. Existing mute/solo and previously-admitted bulk semantics are retained.

Thirteen QCoreApplication cases / thirty-six scenarios are authored for Song/PatternStore sample/instrument selection, first/final callback selected/excluded/project retirement, reorder/add/detach/project-name/render-context changes, second-checkpoint past/future row deletion, selected stored-model changes, cancellation and standard/nonstandard exceptions, independent thread model-lock acquisition after failure, nested ownership, installed-scope weak cleanup, foreign container/thread refusal, native export/pending cleanup with export flag false, effective-controller getter context/errors and structured stem failure with preserved destination then explicit repair. The last two rows author independent libsndfile decoding of actual native SampleTrack/ProjectRenderer output. Raw atomic flag inspection avoids the native public getters' export-state gate. These are native unit/integration fixtures, not GUI automation, and none has been compiled or run.

Source checks report 541 C++ delimiter files / 165 native operations. Admission snapshots add synchronous O(n) work/identities with no measured CPU/RSS/latency or hard deadline. They validate finite final ownership and selected eligibility rather than deep project, clip, controller, plugin or opaque state; transient changes ending with the same roster can be accepted, and external callback edits are not rolled back. Arbitrary executing native/Qt getter/constructor/callee/destructor behavior, foreign QObject mutation and global root/engine teardown remain outside universal guarantees. Callers still release installed scopes only after native processing stops; retirement tests do not exercise concurrent DSP. Instrument rows establish authored flag assertions, while decoded recovery rows cover sample stems only, leaving instrument-plugin/PatternStore audio unproven. Native C++/Qt/link/ABI/sanitizer execution, full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware regression, actual audio and real-provider autonomous music/listening/revision evidence remain required for acceptance.

### Native SoundFont retained-synth clock preparation (2026-10-15, source only)

The output clock is already authoritative when AudioEngine emits sampleRateChanged. SF2 now prepares the retained synth's conversion immediately after application-thread resource admission, before fallible preview restoration/effective getters, file cancellation polling or accepted-bank verification. Earlier ordering left missing/changed snapshots and early callback failures ahead of the resampler update, so a retained synth could use a stale output/input ratio. The update and buffered-input reset hold the existing engine/synth guards; file work still holds neither instance mutex. Successful independent candidate publication resets conversion from its actual accepted native rate as before. No alternate synthesizer or DSP generator is introduced, and ProjectRenderer continues to refuse failed export preparation.

Six QCoreApplication cases / twelve native scenarios are authored with production SF2 C++/MOC/UIC, real FluidSynth/libsamplerate and actual NotePlayHandle/instrument buffers. A never-started production AudioDummy subclass exposes controlled output rates without generating samples. Five bank-verification-failure rows exercise 32000/48000/88200/96000/192000 Hz; additional rows cover absent private snapshot, cancellation and standard/nonstandard callbacks after audition, repeated failed rate change with old input discarded and native voice pointer/id preserved, explicit retry with generation advance and safe old note-data retirement, and one required built-module route across four rejected transitions. All scenarios author finite stereo PCM/energy/pitch assertions. Metadata-only alteration preserves sample bytes and RIFF structure while invalidating accepted identity; a byte-level Python check confirmed unchanged sample/preset tables and a changed hash, without native execution. Two older Qt dialog/render fixtures now explicitly include FluidSynth function declarations.

Source checks report 540 C++ delimiter files / 165 native operations; all native fixtures remain uncompiled/unrun. This is controlled native instrument-buffer coverage, not proven AudioEngine/mixer/backend/offline-file/PatternStore playback or hardware execution. Metadata/private-snapshot failpoints do not establish user repair behavior, and allocator/resampler/program-selection failure injection is absent. Conversion reset can discard buffered input, so acoustic continuity/quality and device behavior remain unverified. Arbitrary native constructor/getter/callback/destructor/foreign mutation/global teardown and nonpreemptible work remain outside universal safety/rollback guarantees. Full C++/Qt/MOC/UIC/link/ABI/module and sanitizer execution, default-plugin GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI compatibility, actual audio/hardware, CPU/RSS/latency/deadline and real-provider autonomous composition/listening/revision acceptance remain open.

### Native SoundFont preview exclusion and renderer preparation (2026-10-15, source only)

The native SF2 preview API now refuses auditions throughout export admission and execution. A retained RemotePlugin context is checked before taking the engine model-change mutex, including finished or cancelled contexts awaiting release. Application-thread admission then checks native file-device ownership, Song export/pending cleanup, stored audio-device reservation and stem selection. Public owned-token restoration is permitted during reservation/stem-only admission and refused during consuming export windows. This distinction preserves dialog Cancel when an untouched admission scope can finish without any device-rate notification.

ProjectRenderer already begins a RemotePlugin context before owner-thread setAudioDevice and starts its worker only after setup succeeds. AudioEngine stops processing and emits qualitySettingsChanged then sampleRateChanged on device installation even for an unchanged numerical rate. SF2 uses that existing signal as its preparation boundary: restore a current preview to checked effective project controls first, then verify the accepted bank and rebuild. The private rebuild returns success; a failure while render admission is active raises into the renderer's setup-failure handling, preventing worker start and successful finalization. Ordinary rate rebuild failure retains the prior native synth and diagnostic behavior, after restoring its preview. The public Qt slot and native Instrument virtual interface are unchanged; no generic plugin preparation ABI is added.

Ten QCoreApplication cases / nineteen scenarios are authored against production SF2 C++/MOC/UIC, FluidSynth and real calibration-bank bytes. They cover seven admission states, standalone file-device ownership, Song and idle PatternStore synth preparation/finalization callbacks, damaged accepted-snapshot failure inside/outside export, two sample rates, callback refusal, a foreign held engine model-change guard, pre-setup cancellation, preserved output with explicit failpoint recovery, and one required built-module Runtime render/receipt replay/checkpoint reopen after source removal. Five scenarios author independent libsndfile decoded-PCM/pitch checks. PatternStore coverage here is preparation of its synth, not playback. Two additional QApplication dialog rows exercise restoration on Cancel during reservation or stem selection. Required module imports do not skip; none of these native fixtures has been compiled or run.

Source checks report 539 C++ delimiter files / 165 native operations. The preparation boundary is the existing ProjectRenderer device path used by native export and Studio render/stems; bare internal Song::startExport/manual renderNextPeriod without device setup has no new preflight. Concurrent arbitrary native mutations are not made into an immutable project transaction. Live playback/voice tails, actual hardware-rate retained-synth/resampler behavior (clock preparation is addressed above in source), arbitrary Qt/library/callback/destructor lifetimes and nonpreemptible work remain outside this source guarantee. Private accepted-snapshot corruption/repair is a fixture failpoint. No C++/Qt/MOC/UIC/link/ABI/module-import, signal/worker/sanitizer, PCM/acoustic/hardware, CPU/RSS/latency/deadline or real-provider musical/listening execution evidence is established. Full default-plugin GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI and autonomous end-to-end acceptance remain open.

### Native SoundFont modal and preview ownership (2026-10-15, source only)

SoundFont presentation now uses the same weak native/root/model/widget ownership and setup epoch discipline as GIG while retaining live native audition, its source/proxy table, regex search, numeric sorting and keyboard routes. Source/proxy/selection/filter objects are heap QObject children tracked weakly. Returned resource getters, model/widget actions and native commits check accepted bank/path identity, root generations, project filename/rate, stored bank/program and current setup. Initial selection uses a checked effective-model getter; manual values remain the context baseline. Old setup connections and queued notifications cannot resume a newer context, and caption/catalog/old accepted identity stay owned through scoped continuations. The view owns its modal child through a weak heap pointer with checked selected-track/project context.

The native core retains its three-argument preview API and adds opaque typed token ownership. Native selection creates a token without committing stored models; only the current token can restore current checked effective controls, with a second token check after getter returns and before channel selection. New audition, model/committed selection, bank publication/clear/pending state and successful synth rebuild invalidate older tokens. Failed audition preserves the last token, replay/null restores refuse, and expected restore during fileLoading uses observation admission. Dialog Cancel/invalidation/reuse/destruction and empty search results restore only their own current token. Unavailable effective programs unset the native channel, preventing new notes from inheriting an old audition. The [FluidSynth API](https://www.fluidsynth.org/api/group__midi__messages.html) documents that unsetting preserves SoundFont/bank IDs and changes the preset to an unassigned state; native fixtures normalize that state without assuming cleared IDs. Existing voices/tails are not instantly removed.

Thirty-one cases / sixty-seven scenarios are authored using QApplication, production SF2 C++/MOC/PatchesDialog UIC, actual FluidSynth and real SF2 bytes. Coverage includes owned context/modal lifetime, cancellation/commit with native channel inspection, stale getters/models/widgets, nested/reused/queued context, bank/root/rate changes, standard/nonstandard failures, token supersession/restore/native invalidations/admission, filtering/keyboard/destruction/multiple-dialog audition, invalid previews, unchanged serialization, projection/filter/selection retirement, source-row native ownership and two cancellation rows during device/stem admission. One required built-module case saves portable project resources, removes the original bank and reloads the selected program. An existing QCoreApplication lifecycle case also covers token restore/replay refusal without widgets. Required discovery fails rather than skips; CTest requests the offscreen Qt platform and all native fixtures remain uncompiled/unrun. Rate notifications are synthetic native signals, not hardware/rate-change verification.

Source checks report 538 C++ delimiter files / 165 native operations with existing supplemental tools and declaration budget unchanged. Full C++/Qt/MOC/UIC/export/link/ABI/offscreen/test-host module imports, actual modal/proxy/selection/signal/self-destruction and sanitizers, and default-plugin GUI/legacy/project/preset/clone/bank/controller/automation/DSP/MIDI/export regression remain open. Arbitrary executing constructors/callees/destructors, foreign Qt mutation/global teardown, native parser/callback/voice/tuning/effect and effective/controller/automation/opaque state remain outside universal guarantees. Finite checks provide no deep immutable snapshot or rollback; direct faults can retain effects until Runtime recovery, failed cleanup can leave audition state, and authorized native publication can survive dialog retirement. Live audition still affects playback and existing voices/tails. The renderer preparation continuation above addresses the ProjectRenderer device path in source; arbitrary native mutations and manual DSP entry without device setup remain outside it. No hard deadline, latency, CPU/RSS, realtime, actual PCM/hardware or real-provider musical/listening proof is established. Whole DAW acceptance remains unproven.

### Native GIG program-dialog ownership (2026-10-15, source only)

GIG presentation now uses an owned native program catalog and native selectProgram commit, replacing raw GigInstance/libgig cursor/model/label access and separate bank/patch writes. Candidate changes remain local until explicit acceptance, preserving the former GIG no-op audition behavior. Cancel leaves native controls and instrument selection unchanged; unchanged acceptance does not republish a program. Stored current selection is reflected when available, an unavailable choice requires explicit acceptance of a visible valid fallback, numeric sorting remains available, and duplicate bank/program rows are suppressed. Stable native catalog sorting retains enumeration order for equal keys.

A dialog context pins weak instrument/selected track/container/Song/Mixer/AudioEngine/models/widgets, accepted bank identity/path, project/routing generations, project filename/rate and stored bank/program. Setup epochs, copied caption/catalog values, weak cleanup and returned-context checks cover resource getters, widget callbacks and native commits. Old setup connections are disconnected, queued older notifications check their epoch, and file/project/rate/external control changes invalidate stale selection. Unnotified generation/resource metadata changes are checked at the next interaction. The old accepted identity remains pinned until scoped continuations finish during setup replacement. A weak heap modal caller checks surviving view/model/track/project ownership and copies the track name only after the returned owner check; parent retirement no longer deletes a stack-owned child dialog.

Twenty authored cases / forty-five scenarios use QApplication, production GIG C++/MOC/PatchesDialog UIC and actual libgig split banks, with an optional second program that leaves default fixtures unchanged. They cover current selection, three Cancel routes, two native commit routes, empty/pending refusal, six actual modal context retirements/replacements, external controls, returned getters, native commit owner/dialog death, nested setup/widget retirement, reused setup/disconnection/queued old notifications, root/routing/rate notifications, standard/nonstandard errors, copied caption, bank metadata mismatch, missing widgets, unchanged acceptance, unavailable programs and retired snapshot cleanup deleting the dialog on reused setup. One required module case saves portable native resources, removes original root+companion files and reloads the selected program. CTest requests the Qt offscreen platform; these are native widget unit fixtures, with no external GUI automation. Native discovery fails rather than skips, but the suite registers only with gigplayer enabled. All native build/widget/runtime execution remains unverified; a synthetic native rate notification is not hardware/rate-change evidence.

Source validation reports 537 C++ delimiter files / 165 native operations. Full C++/Qt/MOC/UIC/export/link/ABI/offscreen/test-host module imports, actual modal event ordering and default-plugin GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI regression remain required. Arbitrary executing constructors/callees/destructors, foreign Qt mutation, global teardown and native parser/streaming/voice/dimension/effective/automation/controller/opaque state remain outside universal guarantees. Context checks do not roll back getter side effects or partial native commits; deletion after successful program publication may leave the authorized choice committed. The SoundFont dialog continuation is recorded above; native verification remains open. No new actual PCM, hardware, latency, CPU/RSS, hard-deadline or real-provider autonomous musical/listening proof is established; complete DAW acceptance remains open.

### Native GIG pending root and companion recovery (2026-10-15, source only)

Native GIG settings now preserve a pending absolute root filename and three stored bank/patch/gain controls when the root is absent, when structurally checked unbound declarations have missing companions, or when an explicit unresolved marker is loaded. A marked source remains pending after files appear. The resource guard compares/adopts the flag with instance/group/path identity. Native instance/note retirement removes old samples; pending inventories have no accepted group or programs, and native play returns zero buffers while clearing queued pending notes. This leaves other instruments and downstream effects intact. Empty settings/explicit clear remove pending state, and native rate reset retains it.

FileResourceSnapshot adds an overload with typed GigCaptureFailure while retaining its existing two-argument entry. It starts as Other, reports MissingCompanion only for unbound sources after parsing every declaration, checking safe/noncolliding names, validating every present companion's native RIFF/identifier layout and verifying the captured root/present member set, and reports None on a complete successful capture. Missing members do not short-circuit later corrupt members or invalid declarations. Accepted snapshot damage, root/read/copy failures, cancellation and malformed present members stay errors. No partial group is returned. This only proves structural consistency of the available source; native libgig program/sample validation requires the complete group and occurs at repair. Filesystem absence is best effort, without an atomic filesystem/content snapshot or sandbox guarantee.

Pending repair uses native complete-group candidate loading through existing file/resource operations. It requires the stored bank/program, rejects incomplete/corrupt/missing candidates or unavailable programs without replacing pending state, and clears the flag only after successful native adoption. Bank/patch parameters remain editable while pending; clear remains available for intentional new-bank selection. Settings loading has a separate mode so unrelated presets/projects are not constrained by the old pending choice. Native save emits gigplayer/src/unresolved without a fake snapshot. DataFile permits only exact marked bank references as unbound absolute checkpoint XML and keeps portable bundles strict. Inspection and GUI distinguish an unresolved declaration from a readable root/loaded program inventory. Pending persistence retains root/control choices rather than immutable absent companion bytes or names; core capture errors can name missing companions. Same-root repair works after companions are restored, and historical warnings may remain.

Nineteen native GIG cases / thirty-four scenarios are authored from production GIG C++/MOC/PatchesDialog UIC and actual libgig single/split writers. They cover missing/marked/appeared roots and legacy missing companions, three repair routes, unavailable programs, incomplete/corrupt replacement rejection versus clear, empty settings, pending checkpoints, malformed/accepted binding conflicts, independent settings replacement, root/present companion corruption, rate reset, notification retirement, admission failures, inspection/mixed assets and one native NotePlayHandle zero-then-nonzero buffer path. Four required module scenarios cover Runtime load/undo after root appearance/redo/replay/rejected edit/owned split reopen after original root+companion removal, pending reopen after a companion appears and same-root repair, and scripted Chat/Responses discover/search/repair/verify. Core snapshot tests add three cases / eleven scenarios using structural nonplayable RIFF fixtures to distinguish missing extensions/convolution from corruption/unsafe/colliding references, accepted damage and cancellation/exception paths. None has been compiled or run. Plugin discovery fails rather than skips, but native suites register only with gigplayer enabled.

Source checks report 534 C++ delimiter files / 165 native operations with the existing supplemental tools and declaration budget unchanged. Full native application/plugin C++/Qt/MOC/UIC/export/link/ABI/test-host module imports, full default plugin and GUI/legacy/project/preset/clone/bank/controller/automation/DSP/MIDI regressions remain required. The GIG patch-dialog continuation is recorded above; constructor/destructor/custom callback, derived selected-instrument, effective/controller/opaque/render/streaming/dimension/voice/rate internals are not universally guarded; direct failures may retain partial effects until Runtime rollback. Engine model-change admission spans synchronous file/native work, so no realtime, hard deadline, latency, CPU/RSS or actual PCM guarantee is established. Native tools are absent, seventeen submodules remain uninitialized, and hardware/real-provider Interpret/Plan/Compose/Render/Listen/Evaluate/Revise/Preserve evidence and whole DAW acceptance remain open.

### Native GIG file/settings lifetime foundation (2026-10-15, source only)

GigInstrument resource/settings/program transitions now pin weak native ownership and selected-instrument/container membership, root generations/project filename/output rate, GigInstance/accepted group/path identity and three stored controls. An application-thread/nonreentry guard suspends cooperative Qt event delivery, owns copied inputs and accepted snapshots, checks returned model/label/file/patch work and restores blocked bank/patch signals only on surviving QObjects. Expected nested patch observation remains permitted. Generic file resources retain accepted-group identity; owned program snapshots and label/name copies replace borrowed-vector continuations, and effective bank/patch getters run with postchecks before native instrument selection takes the synth mutex.

Cooperative dependency-group capture moved outside the libgig catch boundary, preventing callback exceptions from becoming translated bank errors. Returned capture and native failure translation validate context, and publication has a final cooperative check. File work runs outside both synth/notes mutexes. Native libgig construction, sample association/metadata/loop validation, candidate adoption, old note/instance retirement and lazy group streaming remain authoritative; bank/patch model publication follows native lock release. The engine model-change guard still spans the entire operation, so this is not a realtime or latency guarantee. GUI file-dialog return checks weak view/model/button/selected-track/project generation, with scoped surviving-button restoration.

Fifteen authored cases / thirty-six scenarios use production GIG C++/MOC/PatchesDialog UIC and actual libgig single/split bank writers. They cover accepted native groups and three controls, two file-work mutex probes, four owner/track retirement paths, six load/save field retirements, weak blocked-signal cleanup, two label and three notification retirements, reentry, six entry/capture cancellation and exception paths, copied inputs/control/root changes, corrupt/missing root/companion replacement versus clear, native program selection, two owned-source-removal native NotePlayHandle buffer/restore/copy paths and Qt event exclusion. One required built-module Runtime route covers publication failure/checkpoint rollback/replay/retry/rejected edit/reopen after root/companion removal. Plugin discovery fails rather than skips; the suite registers only with gigplayer enabled and cannot substitute for the full plugin build. No native fixture has compiled or run.

This lifecycle step preceded missing-group recovery; the pending root and companion continuation above records the subsequent retention, checkpoint admission and repair source changes. The GIG patch-dialog source continuation is now recorded above; native execution remains unverified. Constructor/destructor, arbitrary libgig callbacks, native parser/streaming/decompression/region/dimension/voice/rate internals and opaque/effective/controller state are not universally protected. The selected native instrument pointer is derived by updatePatch and is not frozen by the outer settings guard. Direct failures may retain partial effects until Runtime rollback. File/native work remains synchronous/nonpreemptible with no measured hard deadline, latency, CPU/RSS or realtime guarantee. Full C++/Qt/MOC/UIC/export/link/ABI, test-host module symbol binding/platform imports, GUI/legacy/project/preset/clone/automation/controller/DSP/MIDI regression and actual PCM/hardware/real-provider production evidence remain required. Source checks report 533 delimiter files / 165 native operations; existing tools/declaration budget and whole DAW acceptance gates are unchanged.

### Native SoundFont pending dependency and repair (2026-10-15, source only)

Sf2Instrument now represents a missing bank using its resolved absolute native filename and a distinct unresolved flag. Thirteen editable native controls, including stored bank/program and effect settings, remain available. Native unload/all-sounds-off, voice generation and queue reset remove the old bank; program inventory and accepted snapshot become empty, and pending instrument buffers return zero. This does not suppress other instruments or downstream effect tails. Legacy absent settings enter this state with a recoverable warning, while existing invalid/unreadable bank data remains fatal. An explicit saved marker does not probe/capture an appeared replacement file, and conflicting accepted bindings or malformed references are rejected. The resource guard now compares and adopts the unresolved flag with the bank/path/native identities.

Repair uses native FluidSynth candidate loading through existing file/resource APIs, including the structured instrument resource setter. Pending repair selects the stored bank/program only after confirming the candidate contains it; missing/corrupt files or unavailable programs leave the pending declaration and controls intact. Successful adoption clears the marker. Explicit resource clear remains available for intentional new-bank selection, and native bank/patch parameters can be edited while pending. A separate settings-load mode prevents stale pending controls from constraining an unrelated preset or project replacement. Empty settings clear pending state; rebuild retains it. Normal already-loaded replacement keeps its native selection behavior.

Persistence now recognizes only marked sf2player/src as an additional pending checkpoint dependency. It must resolve to an absolute path and have no accepted source binding, contributes no fake asset bytes, and still prevents a supposedly portable bundle from succeeding. Accepted native snapshots remain owned. Inspection reports declared unresolved state even when filesystem metadata says the path has appeared. The GUI patch picker requires an actual program inventory. Historical diagnostics can remain after repair.

Eighteen authored cases / thirty-one scenarios cover real native SF2 data, production Sf2 C++/MOC/PatchesDialog UIC and FluidSynth; retained settings, three repair routes, unavailable programs, pending/accepted rejection/clear, empty settings, SF2/SF3 pending-path checkpoint metadata, malformed/accepted binding refusal, independent settings replacement, corrupt-file fatal behavior, rebuild, notification retirement, admission failures, inspection/mixed assets and a native NotePlayHandle buffer silence/recovery path. Three required actual-module scenarios cover Runtime history/replay/rejected edit/reopen after source removal and scripted Chat/Responses inspection/search/repair/verification. Runtime undo is also authored after a file appears at the original pending path. These are uncompiled/unrun; SF3 compressed decode, native PCM and real-provider musical evaluation remain unproven. Required module discovery fails rather than skips, but the suite registers only when sf2player exists and cannot replace a full default-plugin build.

Source checks report 532 C++ delimiter files / 165 operations with the existing supplemental tools and declaration budget. Complete application/plugin C++/Qt/MOC/UIC/export/link/ABI and test-host module binding, GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI regression remain required. Existing constructor/global/native callback/destructor, raw effect/tuning/voice and opaque-state limitations persist; direct faults may retain partial effects until Runtime rollback. Synchronous file probes/copy/verification and native FluidSynth loading/iteration/rebuild/lazy retirement have no hard deadline or measured latency/CPU/RSS guarantee. Hardware and real-provider Interpret/Plan/Compose/Render/Listen/Evaluate/Revise/Preserve evidence and whole DAW acceptance remain outstanding.

### Native SoundFont lifetime foundation (2026-10-15, source only)

Sf2Instrument now guards file/settings/program/rebuild continuations using weak native ownership, selected instrument and container membership, application-thread and nonreentry admission, root generations/project filename/output rate, accepted bank and native synth identities, and thirteen manual controls. It owns copied filenames and program labels, suspends cooperative Qt event delivery, checks returned serialization/model/label/file/patch work, and restores blocked bank/patch signals only on surviving QObjects. Snapshot identity is exposed through native file resources. Manual values are a finite boundary check rather than a deep snapshot of automation, controllers or opaque native state.

Bank snapshot capture and retained source verification moved outside both instance mutexes. Native FluidSynth bank candidate loading, program enumeration, adoption and lazy retirement remain authoritative and serialized. Selected model publication follows mutex release. Synth replacement keeps its independent settings/voice generation behavior, verifies retained bytes before locking and unlocks before diagnostics; effect/tuning continuations are checked after return. The GUI modal file path validates surviving view/model/button/selected track and project generation, with weak scoped button restoration.

Fifteen authored native cases / fifty-one scenarios cover production Sf2 source and PatchesDialog UI, real bank bytes, file-work mutex probes from another thread, owner/track retirement, all thirteen load/save fields, weak signal cleanup, label and notification retirement, reentry, cancellation/exceptions, copied inputs/context changes, native program/clear/rejected replacement semantics, retained bytes after source removal and Qt event exclusion. One required built-module Runtime scenario covers publication failure/checkpoint rollback/replay/retry/reopen. It fails on absent module discovery; the overall suite registers only with sf2player enabled. Native test-host symbol binding, platform imports and the complete C++/Qt/MOC/UIC/link/ABI path remain unverified. No fixture has been compiled or executed.

This lifecycle step preceded missing-bank recovery; the pending dependency continuation above records the subsequent source implementation of SF2/SF3 reference retention, checkpoint admission and bank/patch-preserving repair. Constructor/global settings/allocation, arbitrary native callbacks/destructors, voice/render and raw tuning/effect slot internals remain outside universal lifetime guarantees. Direct faults can leave partial model/native/file/signal state until Runtime rollback. FluidSynth calls are nonpreemptible and no hard deadline, latency, CPU/RSS, actual PCM, hardware or real-provider autonomous production evidence is established. Source checks report 531 C++ delimiter files / 165 native operations, with ten supplemental tools and the existing 128-declaration budget unchanged. Full application/plugin/GUI/legacy/project/preset/clone/DSP/MIDI regression and requirement-by-requirement DAW acceptance remain open.

### Native dependency name-boundary continuation (2026-10-15, source only)

StudioProjectResources now separates callback-free native roster/base-name capture from virtual reported-name observation. Track and LFO names are read only after all weak native actors have been captured. The helper binds the returned QString reference, revalidates complete native context and weak owners, then copies the string. Structural validation uses qualified Track/Controller base names so it cannot recurse through virtual getters. Reported overrides remain owner metadata and are compared again after final plugin getters. This removes raw actor/vector continuations following name callbacks that could retire the current track/controller or a peer.

Track names propagate to instrument/track-effect/shaping rows and controller names to LFO rows; sample-clip and mixer-channel name scopes remain native. Initial reported-name capture polls work between actors. The final reported-name pass has no new cooperative poll and ends with complete structural validation. JSON shape, metadata tokens/pages/previews, capability count and read-only history/receipt semantics remain unchanged.

A no-display suite authors ten cases/forty-three scenarios: four virtual-name scopes, track/LFO initial/final self retirement, sixteen peer retirement cases covering clips/controllers/effects/Mixer, native backing and reported-only renames, plugin-getter name changes, eight exception cases, cancellation before the next getter, long-name previews/tokens and actual native Runtime rename/replay/history/reopen behavior. Independently owned returned text makes self-retirement fixture getters valid while the caller must refuse further native access. The test-only metadata instrument supplies no DSP. There are no module skips; every native scenario is uncompiled/unrun.

Source checks cover 530 changed/new C++ files and 165 operations, with ten supplemental tools and universal routing within 128 declarations. Observing returned values does not protect invalid getter/callee/destructor/global execution or unrelated dangling QString storage, concurrent raw Qt mutation or ownership stealing. Later virtual getters can alter earlier opaque declarations/dynamic names after those values were checked, so this is not a general immutable snapshot or proof that arbitrary stateful getters stabilize. Failed reads retain callback side effects without rollback. Pure roster capture, repeated full validation and linear name lookup can be quadratic and hold native model/DSP ownership; huge names/copies/native work have no hard deadline or measured latency/CPU/RSS. C++/Qt/MOC/link and full GUI/legacy/plugin/project/controller/sample/resource/DSP/MIDI regression, actual PCM/hardware, real-provider complete musical workflows and full headless DAW acceptance remain outstanding. The commission remains active.

### Native sound-shaping wave continuation (2026-10-15, source only)

Native volume/cutoff/resonance EnvelopeAndLfoParameters groups now preserve pending waveform paths independently from empty buffers, using userwaveunresolved in persisted elvol/elcut/elres settings. Legacy absent sources become recoverable references; marked state skips probe/decode after a file appears, while existing corrupt content remains fatal and malformed/accepted-binding conflicts are refused. Checked setLfoUserWave repair/clear preserves fourteen manual controls and selection unless explicitly selected, invalidates cached modulation shape data and checks dirty publication. Empty ordinary settings clear stale waves, and legacy reversed sustain remains native.

The DataFile resource table gains the three persisted tags. Parent sound-shaping serialization rebinds accepted source/sample data after renaming generic el nodes to their actual tags, validating resource identities/paths/pending state after hooks before binding. This closes an otherwise ignored binding window. Pending XML references do not invent asset bytes and bundles remain strict; accepted content supports restore/checkpoint copying after original removal with exact stored-asset manifest correspondence. Generic standalone el is not added to the asset table.

Project dependency inspection captures all three groups for Song/PatternStore instruments before plugin getters and validates weak shaping/parameter/native backing/path/pending/selector context. It emits only nonempty reference rows but observes empty groups against new references introduced by getters. New instrument.shaping.waveform.set routes current container/track/target to the actual native shaping object, checks root/track/group and resource publication, and returns current metadata. Target is volume/cutoff/resonance; the inventory resource key is not a repair argument. Existing bounded asset search supplies candidates and universal catalog routing supplies the new operation. GUI sample/clip drops use the same checked core loader instead of writing buffers directly.

The resource guard rejects foreign threads/reentry, tracks full weak QObject ancestry plus optional track/container and native engine/Song/Mixer generations/filename/backing/pending state, and owns path/buffer/manual-control snapshots across returned work. Parent save checks lifetimes between returned filter/group calls. Direct failures/hooks can retain partial native state until Runtime rollback; model notifications, XML aliases, tempo-sync/automation/controller/opaque state and native parameter mutex/registry/DSP internals retain limits. Zero user-wave content is not silence or musical evaluation. File work and stage recalculation can join DSP/allocate with no hard deadline or measured latency/CPU/RSS.

Twenty-one cases/seventy-eight scenarios are authored without module skips, covering all groups/missing markers/later files, direct/structured repairs and selection/clear/failure, tag/checkpoint/bundle/manifest and accepted binding boundaries, malformed native/protocol state, twenty-eight load/save model retirement rows, parent/hook/resource mutations, reentry/cancel/exceptions, cached native modulation, owned source removal/legacy restore, sparse inventory/getter mutation, corruption/events and Runtime history/rollback/replay/reopen. Two scripted Chat/Responses routes use actual dependency/search outputs for catalog-routed repair. A test-only observer instrument provokes getter mutation and does not synthesize music. Every native fixture remains uncompiled/unrun; modulation-array checks and scripted transport are not actual rendered PCM or real-provider production proof.

Source diagnostics cover 529 changed/new C++ files and 165 operations; ten supplemental tools plus universal routing retain 128 declarations. Full application/plugin C++/Qt/MOC/link/ABI, GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI/resource regression, older-version pending-marker compatibility, other dependency recovery, actual PCM/hardware and real-provider complete musical workflows remain unverified. No arbitrary callee/constructor/model-slot/serializer/destructor/global/concurrent Qt safety or deep immutable project snapshot is promised. Whole DAW acceptance remains unproven and the commission stays active.

### Native TripleOscillator resource continuation (2026-10-15, source only)

Each native OscillatorObject now retains an unresolved waveform path separately from its empty sample buffer and null antialias table. TripleOscillator saves independent userwaveunresolved0/1/2 markers with userwavefile0/1/2. Legacy absent files are recoverable references; explicit pending state skips probe/decode after files appear, while corrupt existing content stays fatal and invalid pending/accepted-binding conflicts are refused. Native ten-control state, selection/modulation/antialias behavior and other slots remain editable. Checked instrument.resource.set repair builds decoded content and a native FFT table before publication; failure retains prior resource, successful repair/clear clears only its marker and selection changes remain explicit.

Native FileResources now pin retained sample backing and expose pending paths/flags for dependency inventory and checked repair hints. DataFile checkpoint copying/matching admits only the exact three native attributes with corresponding markers as absolute unbound pending XML references; copied asset manifests remain exact and bundles refuse pending resources. There is no invented byte fingerprint or implicit equivalent-file substitution. Existing voices can retain old sample/table backing until they end; fresh voices using a pending user wave get zero content from it, while other oscillators/modulation can still sound.

One native instrument-wide resource guard rejects reentry/foreign threads and checks weak instrument/track/selection/container/engine/Song/Mixer actors, all oscillator identities, project/Mixer generations/filename and retained sample/table/pending state around returned work. Save/file phases check all thirty manual controls, while load permits native notifications between slots and freezes manual values during per-slot decode. Owned serialization and copy-before-release adoption avoid raw actor reads after retiring old backing; LFO context adoption receives the same ordering correction. GUI modal selection checks weak actors/current selection/project generation and initial tooltips use retained paths.

A production-plugin C++/MOC fixture authors seventeen cases/sixty-three scenarios: all slots missing/marked/later-present and native settings roundtrip, direct/structured active/inactive repair/failure/clear, mixed pending/copied checkpoint/bundle/manifest behavior, malformed settings and accepted-binding forms, twenty returned model serialization lifetime rows, actor/root changes, nested transitions, cancellation/exceptions/admission cleanup, copied inputs/control mutation, owned source-removal restore, inventory/corrupt-file/event behavior and two fresh native NotePlayHandle direct-wave/antialias buffer scenarios. Three rows require the actual built module: Runtime history/rollback/replay/reopen after source removal plus two scripted Chat/Responses dependency/search/catalog-routed repair traces. Module absence is a failure, with LMMS_PLUGIN_DIR set to the built target directory. The suite registers only when the plugin target is enabled; the full default plugin build remains required. Every native scenario is uncompiled/unrun, including those inspecting note buffers; scripted transport does not establish real-provider autonomy.

Source diagnostics cover 526 changed/new C++ files and 164 operations; ten supplemental tools and universal routing retain 128 declarations. Layout/helper changes require full C++/Qt/MOC/export/link/ABI/application/plugin and GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI/bundle/checkpoint regression. Old versions ignore pending markers; other resources and opaque plugins still need recovery semantics. Direct failures can retain partial state until Runtime rollback, no deep immutable XML/automation/opaque-state snapshot is promised, and arbitrary invalid callee/constructor/model-slot/serializer/destructor/global or concurrent Qt mutation remains outside guarantees. Existing shared FFT scratch/plans and generation are not newly concurrent or preemptible; no hard deadline/latency/CPU/RSS measurement exists. Actual rendered PCM/hardware, real-provider musical workflows and full headless agent-native DAW acceptance remain unproven; the commission stays active.

The fixture exports test-host symbols for native module loading. Platform module imports and test-host binding still require native link/execution checks, particularly Windows plugins built against the normal LMMS executable. Source registration and dependency paths do not establish module-host compatibility.

### Native pending LFO wave continuation (2026-10-15, source only)

The native LfoController now separates an unresolved waveform filename from its empty playback buffer. Legacy missing sources become recoverable retained-reference diagnostics; explicit userwaveunresolved settings persist intentional pending state without probing/decoding a file that later appears. Existing corrupt waves remain fatal. A selected pending user wave contributes zero modulation while the native base value remains active. Six manual controls, waveform selection and controller connections remain native editable state, and inactive pending waves retain their path independently from the selected waveform.

Checked waveform loading repairs or clears pending state, preserving controls/connections/selection unless explicit selection is requested. Failure retains prior reference/backing. Resource/controller inventory exposes retained paths/unresolved state and native repair coordinates independently from filesystem metadata. DataFile checkpoint copying/matching admits only explicit unbound pending LFO userwavefile references, retains ordinary manifest equality and accepted-source correspondence, and never invents stored asset bytes; bundles still require every dependency. Accepted source bindings cannot be bypassed by adding a pending marker. Repaired owned checkpoints retain content after original source removal.

LFO resource context checks application thread, weak controller/parent/engine/Song/Mixer actors, native replacement generations, project filename and retained backing/pending state across returned settings/decoding work. Save uses owned buffer/path values and checks manual controls; decoder input is copied and exposed control changes are refused around publication. Structured repair also checks resource/root/path after wave selection and dirty notification. GUI selection pins weak dialog/model/button/project generation and shows retained paths. Direct exceptions can leave partial state; structured Runtime rollback remains authoritative and invalidates handles. Callee internals, arbitrary lifecycle callbacks, global shutdown, concurrent raw Qt mutation and opaque state are not universally protected.

LfoWaveResourceTest authors nineteen cases/forty scenarios covering missing/marked/later-present state and native zero-wave contribution, direct/structured active/inactive repair/clear/failure, preserved connection, malformed state, three accepted-binding forms, pending checkpoint/bundle/manifest/inventory boundaries, dirty feedback, copied arguments/control mutation, project changes, serialization mutation/owned-source retention, corrupt-wave fatality/Runtime load rollback/replay, Qt event exclusion, history/undo/redo/reopen with binding after original removal, and two scripted Chat/Responses dependency/search/catalog-routed repair sequences. No module-conditional skip is used. All native cases are uncompiled/unrun; fixture buffer observations are not rendered audio, listening or real-provider autonomous musical production.

Source diagnostics cover 525 changed/new C++ files and 164 operations, with ten supplemental tools and universal routing within 128 declarations. LFO layout/helper changes require full application/plugin C++/Qt/MOC/link/ABI and controller/automation/legacy/GUI/DSP/MIDI/resource regression. Older LMMS ignores the pending marker, other instrument waves/banks/plugin binaries/parameter-owned LFO/opaque assets still need recovery work, and native DSP joins during file operations have no measured latency/CPU/RSS or hard deadline. Native execution, actual PCM/hardware, real-provider complete production and whole DAW acceptance remain unverified; the commission remains active.

### Native shared asset lookup continuation (2026-10-15, source only)

Native browser filename search is extracted into QtCore FileSearch, shared by GUI FileSearchJob and the studio. Owned task/callback values retain basename tokens/quoted phrases and add last-suffix matching alongside complete suffixes, fixing multi-dot audio lookup. Entry budgets, cancellation and recursive/hidden/symlink flags require neither Engine nor widgets. The GUI adapter preserves its worker/future lifecycle and unlimited recursive/follow defaults, pending native regression.

New read-only asset.roots exposes fourteen actual PathUtil bases without directory creation; local requires a project filename and may point to a session checkpoint directory. asset.search requires an explicit alias/absolute/prefixed directory, returning metadata candidates from bounded filename/suffix/kind search. Native weak project/engine identities, project generation/filename and captured base definitions are checked around traversal. Capture/final guards are brief; the filesystem walk stays outside native model edits with Qt events suspended. Callback changes refuse publication but arbitrary edits are not rolled back on failed reads.

Lookup bounds yielded entries to 1..1000000, pages to 1..1024 rows and item arrays to 512 KiB, with query/suffix limits and long-text previews. Metadata tokens cover process scope, initial root observation, settings and ordered entries independently of page/text mode/time. Each page rescans; changed observations require restarting. total/hasMore cover observed matches only: a capped unmatched scan can have hasMore false and scanComplete false. Narrow the directory or change budget and restart. Unchanged metadata does not establish unchanged bytes; no asset content is opened/hashed/decoded. Native order can drift, permissions reporting is best effort and symlinks can leave a root; no filesystem sandbox or atomic snapshot is promised. Capture, iterator/stat/tokenization/JSON/allocations have no measured CPU/RSS or hard deadline.

Two no-display suites author 26 cases/66 scenarios for shared matching/flags/budgets/cancellation/task ownership/exceptions and the actual GUI worker without Engine/window; native aliases, scoped lookup/page/text/metadata boundaries, project/base/cancel/event checks, no mkdir, historical receipt replay and project revision neutrality. Four scripted Chat/Responses routes use actual dependency/root/search results for checked sample repair, including a disappearing candidate, rediscovery/retry, undo/redo and owned checkpoint reopening after source removal. They exercise a mechanical repair sequence rather than real-provider autonomous musical production. No module conditional skip is used; every native fixture remains uncompiled/unrun.

Source diagnostics cover 524 changed/new C++ files and 164 operations. Ten supplemental tools plus universal catalog routing retain 128 declarations. Task alias/layout/export/MOC integration, GUI worker/future/cancellation, complete application/plugin C++/Qt/link/ABI build, legacy/plugin/DSP compatibility, actual PCM/hardware and real-provider complete musical workflows still need native evidence. Full DAW acceptance remains unproven and the commission remains active.

### Native project dependency inventory continuation (2026-10-15, source only)

`StudioProjectResources` provides `project.inspect section:"resources"` across declared instrument/effect resources in Song/PatternStore tracks and Mixer chains, native sample sources, rack LFO waves and unavailable effects. Rows retain native owner coordinates, keys, file metadata, required/unresolved state, stored selector state and supported repair hints. Whole-scan file-status and unresolved/required counts accompany bounded pages. Readable files and intentional silence remain separate; missing originals can still have retained native content. Backing pins, file metadata and operation hints do not prove decoding, sound, portability or successful repair.

The inventory captures weak ordered native rosters, selected actors and shared sample/wave/resource backing before invoking plugin getters. Returned getter, selector, owner/root/generation/filename and final roster/source checks precede publication; the final getter pass has no intervening cooperative checks. Pages default to 128 rows, allow up to 1024 and use a 512 KiB item-array bound. Preview mode keeps short text exact and bounds long text to 256 UTF-16 units, plus sixteen 64-unit file-type previews. Native strings hash through 8192-byte UTF-16 views. A process-local observation token covers declared metadata, native handles/generations, filename, full text/types/resolved paths and filesystem metadata independently of page/text mode. It is not an asset digest or persistent resource identifier, does not hash backing pointers or bytes, and cannot detect every same-size/opaque change between requests. Completion timestamps do not make filesystem probes atomic.

Project summaries advertise resource inspection without performing a full scan, and both provider routes inherit native schema and agent paging/repair guidance. The project schema now admits warning pages as well as resource pages; it previously excluded warnings despite the handler. Source assertions explicitly cover this route/enum alignment. The new fixture has 19 cases / 48 scenarios without conditional skips for native owner scopes, unresolved/retained state, read-only behavior, pagination/tokens/previews, observation changes and non-verification, actor/getter/selector lifetime, cancellation/failures/events, Runtime receipt/history/reopen and scripted Chat/Responses discovery. Every native fixture remains uncompiled/unrun; source checks cover 516 changed/new C++ files and 162 operations only.

Undeclared/opaque plugin binaries/assets, complete companion sets and parameter-owned/unregistered controller waves remain outside inventory coverage. Stored selectors differ from effective automation; required flags follow declarations. Complete roster/metadata/backing storage and repeated page scans have unmeasured CPU/RSS/latency, with no hard native getter/stat/allocation deadline or total input-memory bound. Filesystem results may change after probing. Arbitrary executing native constructor/callee/mutex/Qt-destructor destruction, dangling selector returns, foreign/raw mutation, mutating getters/global teardown and opaque side effects remain outside universal guarantees; a failed read is not a native rollback transaction. Application/plugin rebuild and native C++/Qt/MOC/link/ABI/platform/legacy/plugin/GUI/backend/DSP regression remain required. No rendered PCM, hardware, real-provider production or complete DAW acceptance is established.

### Native missing sample-clip repair continuation (2026-10-15, source only)

SampleClip now separates intentional missing-source metadata from its empty native playback buffer. It retains filename, optional source bounds and original rate plus arrangement/control/gain/reversal state. Native XML persists a silent marker; later filesystem changes do not silently alter preserved composition. Legacy missing sources become recoverable references, usable legacy embedded backups still supply sound state, and malformed metadata/embedded audio remain fatal. Inspection reports pending state, nullable unknown bounds and zero unknown rate. Copies retain pending value state, and pending length updates preserve placement and cannot create playback handles.

The checked native loader preserves pending clip settings by default through `sample.load`; callers can request explicit replacement with `preserveSettings:false`. Repair validates known bounds before adoption and preserves geometry, gain and reversal; unknown windows use decoded extent. Empty path clears the source/offset while retaining the clip. Native buffer publication clears pending state. Resource work uses owner-thread/reentry admission, native change/event ownership, weak clip/track/container/project anchors, retained buffer identity and checks around returned stages. Save owns native Sample/buffer and validates exposed controls/geometry/pending state. Structured loading additionally compares track/sample/buffer/geometry and project/Mixer/filename state around dirty notification and returns final inventory. The GUI file dialog checks weak returned context and uses native repair.

Checkpoint matching extends explicit pending-state admission from AFP to sample clips while preserving exact stored-file asset manifests and rejecting accepted-binding/embedded conflicts. Resource bundles remain strict about missing dependencies. A process-local fatal-diagnostic generation now distinguishes new native failures from typed retained-reference warnings during serialized selection and track settings restoration, including when diagnostic text repeats or is cleared; serialization retains its complete-generation check. Other native errors remain fatal for restoration.

The new core fixture has 19 cases / 37 scenarios, with no module-conditional skips, covering native/structured repair/replacement/clear, known/unknown source state, existing file/embedded compatibility, project/checkpoint/bundle/copy/clone/selection, accepted bindings and malformed state, callback retirement/mutation/reentry, borrowed paths, owned embedded encoding, native buffer publication, fatal generation and Runtime undo/redo/rollback/replay/reopen with original-source removal. Every native test remains uncompiled/unrun. Source checks pass for 513 changed/new C++ files and 162 operations only.

SampleClip/Song layout and admission/clear/repair/serialization changes require complete application/plugin rebuild and native C++/Qt/MOC/link/ABI/GUI/legacy/journal/clone/bundle/automation/recording/backend/DSP regression. Other instrument/controller and binary/plugin/effect missing dependencies remain incomplete. Older builds ignore the marker; no automatic search or missing-byte fingerprint is invented. Rate-changing repair retains raw frame coordinates without time-stretch equivalence, later resize/tempo policy can change geometry, and pending tempo offsets retain native frame-offset semantics. Source-window/reversal edits need decoded content; mutable Sample access can bypass policy. Arbitrary executing-callee/control-helper/model-mutex/constructor/Qt-destructor destruction, foreign mutation, global teardown and recording/waveform lifecycle remain outside universal guarantees. Partial native/dirty/error/journal/file effects can survive direct failure; Runtime rollback is still unproven. CPU/RSS/latency, repeated snapshot/traversal/storage costs and cooperative budgets are unmeasured. No rendered PCM, hardware, real-provider autonomous production or complete DAW acceptance is established.

### Native unresolved AFP checkpoint and relink continuation (2026-10-15, source only)

AFP preserves intentional silent references with native `unresolved="1"` settings and a filename. Legacy missing sources in empty AFP become retained pending state; explicit pending state restores an empty native buffer even if a file later appears at its old location. Checked resource loading/clearing repairs the state and removes the marker. Malformed empty/embedded-conflicting state is refused. Resource JSON exposes the native pending flag and publication checks it across project notification.

Checkpoint copying preserves unbound pending AFP/src references as absolute XML paths, covered by the native checkpoint digest. Asset manifests continue to fingerprint only real copied files and must exactly match all ordinary resource references. Other missing paths, unsupported markers and malformed pending references cannot bypass that check. Accepted copying or cloning bindings cannot be discarded through pending markers, including a declared path already equal to the retained file. AFP validates owner context after reference verification. Portable resource bundles still refuse incomplete dependencies.

Song keeps its existing diagnostic text/count/generation/paging and adds explicit recoverable unresolved-reference admission. Ordinary `collectError` always latches fatal state until clearing, independent of message text. Project/preset/checkpoint restore checks fatal state, permitting repairable AFP projects while retaining strict refusal for other native failures. GUI diagnostics remain visible, and repair does not erase cumulative warning history. These are typed native diagnostics, not translated-message matching.

Nine added cases / nineteen scenarios exercise diagnostic classification, absent/later-appearing files, pending settings and manifest validation, mixed stored/pending resources, bundle refusal, accepted copying/cloning bindings, dirty-time state changes and actual-module legacy load/checkpoint/relink/undo/redo/fatal embedded rollback/replay/reopen plus restored accepted media after source removal. The AFP fixture totals 27 cases / 44 scenarios, including three actual-module conditional scenarios, all uncompiled/unrun. Source checks cover 512 changed/new C++ files and 162 operations only.

Other instruments/controllers/sample clips and missing plugin/effect binaries remain outside this pending-state implementation. Older LMMS builds ignore the marker; cross-version behavior remains unverified. There is no invented fingerprint for missing bytes or automatic file search/substitution. Original filenames persist in project/inspection/history. Layout and behavior changes require complete application/plugin rebuild and native C++/Qt/MOC/link/ABI/legacy/preset/clone/GUI/automation/bundle/checkpoint/journal/DSP/backend regression. CPU/RSS/latency, actual PCM, hardware, real-provider autonomous production and full DAW acceptance remain outstanding.

### Native AudioFileProcessor lifecycle continuation (2026-10-15, source only)

AFP settings/file work now uses owner-thread, nonreentrant native change/event guards, copied filenames, weak actor/track/selection/container/project-generation anchors and retained expected buffer identity. Returned decoding, rename, model and notification stages revalidate context; weak cleanup releases admission only on a surviving instrument. Failed decoded replacements preserve the old resource and invalid embedded settings retain unresolved references. Save owns a native Sample/buffer plus unresolved path and linked control snapshots, validating sample scalars and all eight controls around native model and embedded serialization while preserving document context. Partial state can survive direct failure; Runtime rollback remains necessary and unproven.

Model slots retain direct/native automation dispatch with lighter context checks after returned getters/setters/signals, without settings owner-thread admission, change guards or polling. They refuse nonfinite amplification/points and native frame overflow and use double point multiplication before integer conversion. This continuation does not audit `playNote`, `beatLen`, DSP or concurrent automation; refcount/weak checks and repeated snapshot costs need measurement.

An optional shared `Plugin::FileResource` backing token pins AFP's native SampleBuffer during resource comparison. It is process-local and never serialized or exposed in JSON. Shared inspection checks plugin survival after getters and weak selector survival after values. Structured resource mutation pins actors, project/Mixer generations and filename before work, validates returned loaders/selectors, compares exposed track state and resource metadata/selector/backing identities around dirty notification, then rebuilds and checks final inventory. Generic plugins without tokens still have opaque-state gaps. The AFP GUI file-dialog continuation checks weak view/model/track/project/waveform identities around returned dialog/load/dirty work.

The production-source AFP fixture adds 18 cases / 25 scenarios for roundtrips, missing/embedded/legacy state, borrowed paths, retirement/reentry/exception/thread/event behavior, serialization changes and owned encoding buffers, getter/loader/result-inspection retirement, successful structured load/clear, selector retirement, same-path backing changes and actual-module Runtime standard/nonstandard rollback/replay/reopen. Two scenarios require built-module discovery. All native cases and GUI implementations are uncompiled/unrun; source checks pass for 512 changed/new C++ files and 162 operations only.

Full application/plugin rebuild and C++/Qt/MOC/link/ABI/GUI/automation/legacy/project/preset/embedded/bundle/checkpoint/journal/DSP/backend regressions remain required. Shared DOM/control/opaque state is not fully frozen. These checks do not establish safety for arbitrary destruction inside executing native constructors/callees/getters/model mutexes/plugins/Qt destructors, dangling selector returns, foreign mutation, global teardown or ownership bypass. Direct failure can retain native buffer/control/dirty/error/journal/file effects. Whole sample/DOM memory, repeated traversals, allocation, CPU/RSS/latency and cooperative-budget effectiveness are unmeasured, with no hard native work deadline. No native PCM, hardware, real-provider autonomous production or complete DAW acceptance is established.

### Native Hydrogen import continuation (2026-10-15, source only)

Hydrogen import now has per-instance source state, guarded direct/dispatched work, cooperative local XML input and a checked scalar/reference plan before native model creation. It accepts Song destinations explicitly and refuses unsupported destinations. The plan validates IDs/aliases, numeric/key/timeline bounds and note/sequence references. XML legacy escape handling now bounds accesses, advances on malformed input, preserves ASCII entities for XML parsing and handles declaration/BOM detection with a Qt input ceiling. DOM parsing remains blocking; unrelated formats remain available to other filters without spurious diagnostics.

The importer creates retained PatternTracks for all source patterns, including empty patterns, before native AudioFileProcessor rows. Source aliases map to those weak native identities, fixing dependencies on pre-existing Song track positions. Direct/legacy first-layer sample paths resolve relative to the input; exact cell/arrangement lengths, maximum-duration simultaneous groups and optional native tempo automation preserve the represented timing. Empty-group one-bar behavior is an explicit inference. Weak track/owner/clip/instrument/column checks and owned reverse cleanup retain caller additions and restore selection when the root context remains valid.

AudioFileProcessor now retains unresolved filenames only for silent empty instances, exposes/saves those references, omits false decoded-buffer registration and clears the marker after successful/embedded loading. Failed replacement of existing decoded content preserves its resource. Hydrogen keeps silent editable rows and exposes missing-resource and approximation diagnostics. Structured import publishes native warnings on success. Missing-asset bundling/checkpoint acceptance, relinking and actual audio remain unverified; AFP layout and missing/embedded/preset/resource behaviors require native rebuild and regressions.

`HydrogenImportTest` adds 22 authored cases / 51 scenarios using actual native AFP and production importer sources: invalid preflight, sample variants, original state/selection, empty/sub-bar/concurrent sequence timing, tempo, filename independence, missing references and plain-project reopen, replacement refusal, warnings, cancellation, actor/importer retirement, ownership cleanup, reentry/thread admission, legacy/BOM/Unicode XML, and native-module Runtime history/replay/reopen/publication-fault recovery. Three Runtime scenarios depend on module discovery. Every native case is uncompiled/unrun. Source checks cover 510 changed/new C++ files and 162 operations; no native C++/Qt/MOC/ABI or recovery acceptance is established.

Full Hydrogen compatibility is incomplete. The legacy first layer/component, forward playback, ordinary-note choke approximation, octave offset and volume clamp remain; velocity/random layers, component routing, reverse/loops/envelopes/effects/probability/lead-lag, presentation metadata and newer schemas need further implementation or golden verification. Drumkit path lookup is absent. Native PatternStore preview rounds to bars; arrangement clips retain source lengths, while shorter-pattern repetition, empty groups, fractional tempo and pitch/audio fidelity remain unverified. The engineering protocol links the upstream manual used for group timing; upstream schema/source files were not retrieved. Direct failures can retain caller/model/dirty/error/journal/MIDI/file effects, and Runtime rollback remains unproven. Arbitrary executing constructor/callee/getter/mutex/plugin/Qt destructor/global teardown, dangling returns, throwing cleanup, foreign mutation and ownership reuse remain outside universal guarantees. Whole input/DOM/plan/model memory, repeated scans/legacy buffer edits, allocations and CPU/RSS/latency are unmeasured with cooperative budgets only. Native full rebuild, GUI/legacy/project/preset/bundle/asset/checkpoint/journal/DSP/backend/MIDI regressions, PCM/hardware/provider production and full DAW acceptance remain outstanding.

### Native MIDI import continuation (2026-10-15, source only)

`ImportFilter::WorkScope` shares the native dispatch session with the exact active filter/destination/pattern or owns a direct-call session. Weak per-filter work admission rejects recursion and restores the work flag. Direct MIDI entry now retains native model/event guards and the prior journal policy and refuses overlapping project transitions. A local `QFile` and static file-open diagnostic decouple input lifetime from importer retirement; PortSMF remains the native decoder.

`MidiImportState` records weak created track/owner, clip, selected-instrument and model anchors and checks returned native operations. The initial empty PatternStore column is explicitly created and owned, and callback-created columns are refused as an unchanged empty destination. Cleanup retires only its surviving registered creations in reverse under cancellation suspension, checking project generation and parent/container membership. It preserves callback-created tracks/columns rather than deleting every track absent from a raw pre-import roster. Native note/control mappings, default instruments and pattern/automation geometry remain in use. Note splitting reads owned native Note copies, retains source pointer/lifetime identities and checks selected source values/roster before retirement. Weak progress ownership and a final state check precede commit.

Ten authored cases / nineteen scenarios were added to `MidiImportTest`, covering direct journal/reentry behavior, custom destination death, Song/PatternStore track/clip/instrument retirement, earlier metadata retirement, callback-owned track/column preservation, importer retirement during input read, source-note mutation during splitting, and standard/nonstandard partial-work cleanup/retry. The suite now contains 37 cases / 110 scenarios, all uncompiled/unrun. Source diagnostics cover 507 changed/new C++ files and 162 operations.

This does not establish safety for arbitrary destruction inside executing native constructors/callees/getters/mutex scopes/Qt destructors or plugin/DSP code. Dangling returns, throwing cleanup, global teardown, foreign Qt mutation, ownership bypass, same-address reuse and opaque/non-model edits remain limited. Native note copies retain detuning ownership rather than freezing all inline state. Direct failure may retain callback edits and native dirty/error/journal/assignment/file effects; Runtime checkpoint recovery is still required and unverified. Whole-file/parser/model/note memory and repeated validation/traversal costs are unmeasured, with cooperative checks rather than hard bounds on copying/sorting/resource/plugin work. Direct journal/admission, initial-column timing and base class layout/API changes require a full application/native plugin rebuild and Qt/MOC/ABI/GUI/legacy/MIDI-format/SoundFont/Patman/PatternStore/controller/checkpoint/journal/DSP/backend regressions. Hydrogen's global filename/instrument state, ignored target and unchecked XML/layer/sequence behavior remain separate work. Native PCM, hardware, real-provider autonomous production and complete DAW acceptance remain unproved.

### Native import dispatch continuation (2026-10-15, source only)

Native import dispatch now uses a private per-Song session with weak project/destination/PatternStore/Mixer/hidden anchors, original roster prefixes, a pinned pattern column and filename/replacement-generation checks. The session owns native model-change/event guards, restores the captured journal policy and refuses nested import or project clear/load/save/new. Copied descriptor names and fresh UTF-8 filename storage isolate candidate attempts. Weak candidate cleanup avoids repeated deletion after self-retirement and retains explicit parent adoption. Returned factory, filter and destructor stages are checked; declined/non-filter attempts must preserve exposed project state and destination membership before fallback. Final imported state is validated after candidate destruction.

Structured `project.import` retains destination/column identity before cooperative work and validates exposed state, context and dirty state around publication and inspection. Direct failure may retain partial native changes; Runtime checkpoint rollback remains necessary. GUI import dialogs hold weak owners, failed drops are ignored, and file-browser model-change ownership now uses RAII. Native/UI behavior has not executed here.

`ImportFilterLifecycleTest` adds 21 authored cases / 46 scenarios for journal policy and fallback, factory/filter decline mutations, standard/nonstandard cleanup, candidate self-retirement/adoption, null/non-filter results, destination death, original roster/column retirement or replacement, nested transitions, borrowed/UTF-8 filenames, cancellation/event/thread admission, mixer context replacement and destructor mutations. Four conditional scenarios run the actual MIDI importer through Runtime publication-fault recovery/replay/reopen, covering exceptions, visible edits and nested project replacement; the other factories are private deterministic dispatcher fault injection. All native tests are uncompiled/unrun. Source checks cover 507 changed/new C++ files and 162 operations.

Seventeen earlier fixture files (18 address references) plus the new import fixture now use `fixtureDescriptor`. In sixteen earlier files, inherited `Plugin::descriptor()` hides the unqualified namespace constant used in derived constructor initializers; one additional file was renamed consistently. A targeted source check catches this pattern, which earlier checks missed. This correction is not native compilation or general C++ lookup/type verification.

The outer import session does not protect direct plugin entry calls or repair MIDI importer raw model/note and rollback-container continuations. Hydrogen's global filename/instrument variables, ignored target and unchecked XML references/indexing remain concrete internal risks; layering/sequence semantics are unverified. Finite exposed-state snapshots do not cover every opaque/non-model property, and global teardown, arbitrary executing constructor/callee/destructor, dangling factory returns, throwing cleanup, foreign Qt access, library unload and raw ownership reuse remain unsupported as universal guarantees. Direct failure may leave native journal/dirty/error/assignment/file effects. Import admission/fallback/event changes need a full native rebuild and GUI/legacy/plugin/project/checkpoint/journal/DSP/MIDI regressions. Allocation/traversal costs and CPU/RSS/latency, actual PCM/hardware and real-provider autonomous production remain unmeasured or unrun. Full DAW acceptance remains unproven.

### Native project creation continuation (2026-10-15, source only)

Song now coordinates empty/default/template construction through a private `CreationGuard`: weak Song/PatternStore/Mixer/hidden-track anchors, owner-thread and nested-transition admission, native model-change guard, cooperative event suspension and prior loading/journal restoration. Template creation uses the existing native loader with an internal one-shot permit consumed before callbacks. User default precedes factory default; absent defaults construct the actual Triple Oscillator/Kicker and native Sample/Pattern/Automation tracks. Later stages check earlier created track/instrument identities. Unavailable builtins and invalid default templates are diagnosed, and startup template failures cannot recursively create another project.

Completion validates exposed project state around filename/length/modified notifications and requires a blank filename and clean state. Structured `project.new` checks native errors and validates Song/Mixer replacement generations, filename, expected dirty state and the exposed project snapshot around dirty notification and inspection. GUI entry points preserve weak owners and catch exceptions across new/template creation; no GUI behavior has been executed here. Cancelled or invalid templates no longer trigger recursive project substitution during creation.

`ProjectCreationLifecycleTest` contributes 20 authored cases / 25 scenarios covering real builtin creation/checkpoint reopening, isolated native template precedence, invalid/missing templates, journal/loading/admission cleanup, nested replacement, standard/nonstandard clear/header failures, callback track retirement and final visible-state/dirty-state mutation, borrowed paths, event suppression, cancellation, thread refusal and Runtime rollback/replay/reopen. Actual builtin module build dependencies are registered. All remain uncompiled/unrun. Source diagnostics cover 506 changed/new C++ files and 162 operations.

Seventeen earlier fixture calls in fifteen files incorrectly accessed Song's private boolean `setModified` overload. An inline test-only friend helper corrects those accesses without making the application setter public. A targeted source check now rejects direct boolean calls in test C++ files; the prior checker missed this compile-level error, and source checks still do not prove C++ correctness.

Failure can leave partial state for direct native callers; structured recovery still requires Runtime checkpoint rollback and refusal to continue when recovery fails. Finite exposed-state snapshots exclude opaque plugin bytes and selected non-model contents. Arbitrary executing constructors/callees/getters/destructors/global teardown, unsynchronized deletion and foreign QObject mutation remain unaudited as universal guarantees. Full native rebuild, Qt/MOC/ABI, GUI/startup/template/legacy/plugin/project/checkpoint/journal/DSP/MIDI regressions and sanitizers are outstanding. Native PCM, hardware and real-provider musical workflow acceptance, along with CPU/RSS/latency costs of the added guards/snapshots, remain unmeasured. The complete DAW commission remains active and unproven.

### Native mixer continuation (2026-10-15, source only)

Mixer flag lifetimes now use weak owner guards. Native channel continuations retain weak mixer/effect-chain identities and validate roster membership after returned callbacks across create/reset/delete/move, selector assignment/range updates, solo mask/release, journal restoration and project load. Channel model QObject parents detach before member destruction. Routes detach their graph ownership before amount-model destruction, and returned route operations validate weak route/endpoints. Mixer load retains native validation/serialization semantics with cooperative checks, event suppression and guarded allocation/model/effect/send/completion stages.

Song and Mixer have monotonic, overflow-checked process-local replacement counters. Song advances on admitted clear and valid load before filename publication; Mixer advances on clear. Structured mixer operations capture original channel/endpoint identities before work, resolve surviving indices for dispatch and verify root replacement counters. They capture visible channel/effect/route state after dispatch and validate it after project dirty notification, preventing stale success receipts after callback mutation or a reused master. These counters are not persisted project identity or universal context checks for every operation. Native direct failures can retain partial state; Runtime retains checkpoint rollback responsibility.

`MixerLifecycleTest` adds 20 authored cases / 31 scenarios, including native routing/solo roundtrip, model/effect/solo/journal/route/channel lifetime faults, native destruction ownership, admission/cancellation, context replacement/reordering, dirty-notification refusal and Runtime recovery/replay/reopen. All remain uncompiled/unrun. Full native rebuild, sanitizers, Qt/MOC/ABI/GUI/backend/plugin/DSP/MIDI/legacy/project/checkpoint/journal regressions remain necessary. Raw ownership bypass, unsynchronized deletion, arbitrary executing-callee/constructor/destructor/global teardown and same-address mutation are not universally protected; visible state checks do not snapshot every opaque property. Allocation, graph traversal and realtime CPU/RSS/latency costs are unmeasured. No native audio/hardware/provider or full DAW acceptance is established.

### Native MIDI port continuation (2026-10-15, source only)

MidiPort's output synchronization now uses retained mutex ownership. A move-only lock keeps its recursive mutex alive after an observable port retirement and unlocks old state before releasing that owner during move assignment. Native MIDI client teardown and transfer use the retained lock as well. Weak transition admission and original-client checks cover port settings/name/source/mode/subscription/release work; serializers check all eleven fields and cooperative callbacks. Mode notifications and retarget rollback use weak cleanup, and subscription refresh uses local maps rather than member iterators across backend calls. Output submission/release checks the surviving port/client before updating the note/pedal ledger, refuses recursive cleanup and suppresses ordinary output during retirement/release. Existing uncertain cleanup retirement handling remains authoritative. Queued transfer notifications recheck weak port and selected-client identity after each emission.

`MidiPortLifecycleTest` adds 16 authored cases / 49 scenarios covering model and backend callback retirement, retained locks, exact/unavailable names, native mode/cleanup/retarget behavior, actual MidiController load death and Runtime MIDI recovery/replay/reopen. These tests remain uncompiled/unrun and use deterministic native client/processor fixtures rather than hardware. Partial routing/model/backend state can remain after direct faults; this is not an atomic routing transaction or hardware acknowledgment. Retained lock reference-count cost, per-port allocation, lock order and realtime latency require measurement. Backend/worker lifetimes, executing constructor/destructor/callee destruction, foreign QObject mutation, same-address reuse and global shutdown are not universally protected. Full native build/ABI/Qt/MOC/GUI/backend/legacy/checkpoint/transfer/plugin/DSP/MIDI/audio/provider acceptance remains outstanding, as do Mixer restoration/clear/allocation/route/solo continuations.

### Native controller settings continuation (2026-10-15, source only)

Controller settings use a shared protected guard with owner-thread/retirement/reentry admission, weak cleanup and cooperative lifetime checks. Existing public virtual serializer signatures remain; builtin base/LFO/MIDI/Peak implementations use protected base-field helpers to retain one guard across their settings. LFO checks each model boundary and decodes waveforms into a local shared buffer before publishing to a surviving controller; its public waveform setter also refuses overlapping settings transitions. MIDI guards returned base/port/name boundaries. Peak save checks the selected effect after base serialization.

The XML factory retains weak parent/Song/candidate state and deletes only surviving, unadopted new LFO/MIDI controllers with the original parent identity, no connections and no rack registration. Shared Dummy and effect-owned Peak ownership is preserved, as is explicit rack adoption or detachment before parent retirement. Song restoration checks earlier/restored rack entries and returned registration. Waveform capabilities verify weak rack identity after decode/selection and sample/selector identity after dirty notification, under cancellation and event suppression, then report the surviving index. Optional GUI waveform selection retains weak dialog/model/Song state across the modal call and setter.

`ControllerSettingsLifecycleTest` adds 16 authored cases / 32 scenarios, including native LFO model/wave roundtrip, finite controller buffers, returned lifetime faults, factory ownership, rack registration with Runtime rollback and waveform failure replay/reopen. These remain uncompiled/unrun; source checks are not C++ execution, rendered PCM, hardware or provider evidence. Direct transitions can retain partial state, and custom serializers need their own guards. MidiPort's internal lifetime/locking paths and Mixer restoration/clear/allocation/route/solo continuations remain outstanding. Peak behavior has source checks without an actual effect fixture here. Full native rebuild, ABI/Qt/MOC/GUI/legacy/plugin/checkpoint/controller/link/automation/DSP/MIDI regressions and performance measurement remain required. Complete DAW acceptance remains unproven.

### Native project load continuation (2026-10-15, source only)

Native Song file restoration now has weak loading-state/journal cleanup, explicit nested/foreign-thread admission, an immutable local copy of its requested path, and project/template preflight before filename publication and destructive clear. Returned restoration stages and completion notifications check the original Song/PatternStore/Mixer/hidden-track/journal context. Public save/clear/new/template operations refuse replacement while a file load is active. The structured project-load path runs with cooperative cancellation and event suppression, checks weak ownership after native restoration and before/after dirty notification, and leaves transactional rollback with StudioRuntime's existing checkpoint machinery.

The supporting native clear has separate weak admission/context cleanup, guarded track/controller teardown walks and no posted-event dispatch in headless replacement. TrackContainer restoration uses weak owner/Song admission and a weak shared GUI progress dialog with scope cleanup; it creates only track child elements and reports unsupported track types as native warnings. DOM track counting no longer extends its iteration bound with pattern children. Both track factories replace strong pending ownership across returned native callbacks with weak candidate cleanup, checking survival and original container registration after update/restoration. This does not guard destruction inside an executing constructor. Failed template loads retain the previous project filename. The preceding save fixture's hidden-clip assertion now preserves the actual native count, including default global automation curves.

`ProjectLoadLifecycleTest` adds 15 authored cases / 26 scenarios, including native Runtime notification-fault recovery/replay/reopen and cancellation rollback. All native tests remain uncompiled/unrun. Direct loading can leave partial state after failure; native notification edits remain part of the loaded state. These checks do not replace per-callee lifetime audits, immutable project transactions, GUI/legacy/plugin/checkpoint/journal/DSP/MIDI compatibility testing or actual native execution. Mixer/controller/LFO/MIDI load continuations and default-project creation still need further review. No native audio/hardware/provider or CPU/RSS/latency evidence is established; full DAW acceptance remains unproven.

### Native project save publication continuation (2026-10-15, source only)

Project publication now holds native audio/model ownership and suppresses nested event dispatch while preserving cooperative cancellation. A `NativeProjectSaveSnapshot` captures Song/PatternStore/Mixer ownership, regular/pattern/canonical hidden track snapshots, exposed parameter inventories and linked ControllerState groups, controller identities/persisted names, mixer channel/effect/send/route identities and selected metadata, shared tuning slots, timeline loop/stop state and current pattern. Native serializers still write LMMS's actual project representation. Validation follows all serializers and runs through DataFile's before-backup and final-before-commit hooks. This rejects visible changes made by later serializers to earlier source objects. The canonical Song hidden automation track is explicitly accepted outside the normal track roster, fixing its rejection by the preceding generic track snapshot.

TrackContainer's track-list lock now spans only the weak roster copy, with row checks between native calls and a final full identity walk. Song save flags and mixer topology admission use weak cleanup. Nested Song save/load/clear is refused during a save; mixer topology changes are refused while channels serialize. Native controller rack serialization retains weak ordered entries, and tuning serializers hold local shared slot ownership. Structured project saving uses a cancellable event-suppressed scope, retains its non-project-mutating classification and returns the committed native path without post-commit source calls. GUI save continuation checks the surviving Song.

`ProjectSaveLifecycleTest` contributes 14 authored cases / 28 scenarios, including actual builtin pattern/global-automation checkpoint reopen and Runtime replay/reopen, all uncompiled/unrun. Source checks cannot establish C++ type correctness, native deadlock freedom, plugin compatibility, successful recovery, or file durability. This baseline does not cover every opaque/non-model/GUI/external property; failed saves can leave live callback mutations, backups and resource side effects. Capture and validation costs, duplicate per-track/global groups and mixer/rack scans are unmeasured. Arbitrary destruction inside executing native callees or global engine shutdown is not made safe. The full native build, compatibility/GUI/DSP/audio/hardware/provider workflow and complete commission acceptance remain outstanding.

### Native preset save publication continuation (2026-10-15, source only)

NativeTrackSaveSnapshot now retains the registered weak track/container/Song,
track metadata, selected instrument and ordered effect identities, optional clip
identities, exposed parameter inventory/IDs and independent linked ControllerState
groups. It checks stored/initial values, dimensions, scale/step/strictness,
controller-use flags, bindings and owner threads. Existing assignment checks keep
their strict interpolation comparison; serialization explicitly ignores only the
transient old interpolation value. Full checks occur at boundaries and publication,
with cheaper owner/count checks between native work to avoid a complete scan for
every serialized clip. Finite owner-thread linked state is required.

Track.saveTrack/savePreset use native audio ownership, incidental event suspension
and weak save admission. Nested saves and overlapping track/preset loads or
instrument replacements are refused. Track metadata, weak clips, derived settings
and linked-control completion are validated before success. InstrumentTrack's
native settings serializer checks weak owner, selected plugin and active CC state
after returned model/group/plugin work. Preset mode omits the top-level MIDI port
and now also skips the old automatic-input unsubscribe/resubscribe side effect.
Full-track serialization retains its existing temporary auto-assignment behavior,
with weak cleanup attempting restoration when port serialization fails. That is a
software restoration attempt, not acknowledgment from a physical MIDI device.

DataFile's existing two-argument writeFile entry remains available. A new overload
accepts a validation callback before backup publication and immediately before
QSaveFile replacement, after the final cooperative work check. A thrown validation
failure drops the staged replacement and preserves the old target. Late refusal
can leave a newly written backup; resource bundles can still leave resource files
or folders. This is a target-file publication boundary, not a transaction over all
filesystem effects or external writers/storage durability.

Structured preset.save retains its existing non-project-mutating classification.
It uses the native serializer, validates the source through atomic commit, allows
selected-track recording because presets omit clips, and returns committed=true,
the actual native extension-adjusted absolute path and surviving current track
index/container. It does not dirty the live project or invoke native source work
after committing. Replay reports the historical receipt and does not recreate or
verify the present artifact. The optional GUI save flow also checks weak dialog,
window and track continuations and uses the shared precommit validation; existing
native file diagnostics remain its failure path. GUI execution is unverified.

InstrumentPresetSaveTest authors 13 cases / 29 scenarios for portable XML versus
live flags/values, source owner/plugin retirement, eight native source changes,
standard/nonstandard serializer faults, reentry refusal, full-track clip checking
versus preset omission, interpolation reads, four writer validation refusals,
four late work faults after backup publication, automatic-input preservation,
full-track restoration on MIDI-port hook failure, recording/current-row identity,
and Runtime replay/reopen without rewriting an externally changed preset. All
remain uncompiled/unrun. Most use native test classes and raw fake input; a separate
Runtime case requests the actual tripleoscillator plugin but proves no execution
or sound from its authored source.

The snapshot does not verify every opaque plugin byte, non-model property or note
content, and it does not make a whole project commit atomic across later track
serializers. Faults can leave native serializer/callback changes even when target
publication is refused. Since preset.save remains a non-project-mutating export,
no automatic project rollback is promised; inspect unexpected live changes and
use explicit saved recovery state where appropriate. Runtime receipt persistence
or a crash after file commit remains a separate boundary from native file commit.
No capability or compound native journal was added (162 operations).

Track save/admission virtuals, instrument layout and shared controller-state checks
require a full C++/Qt/MOC/ABI rebuild and broad GUI/plugin/project/preset/clone/
journal/DSP/MIDI regression. Arbitrary invalid execution inside constructors,
getters/callees/serializers/hooks/parents/globals/plugins, throwing retirement,
foreign-thread/concurrent raw Qt mutation, ownership stealing and same-address
aliasing retain limits. Copies and callbacks have no measured CPU/RSS/latency or
hard deadline. Source diagnostics cover 497 changed/new C++ files and 162
operations; native fixtures, compatibility, actual PCM/hardware, real-provider
musical production and full DAW acceptance remain unproved. The commission remains
active.
