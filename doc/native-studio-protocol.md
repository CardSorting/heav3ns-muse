# Native studio development interface

This interface is an in-process LMMS implementation under development. It exposes
native execution and persistence for the larger conversational DAW transformation.
The source now includes configurable conversational inference, durable production
jobs and audio submission. These changes are uncompiled and unverified; complete
DAW coverage and real autonomous musical workflows remain unfinished.
See [the complete architecture and completion gates](agent-native-architecture.md).
No native build or execution has been verified in the current managed workspace.

## Starting and operating the session

Build the full LMMS application with its normal plugin configuration and tests:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
python tests/studio_process_test.py --lmms build/lmms --output build/studio-evidence
```

Install the native plugin dependencies described by upstream build instructions.
Keep all bundled plugins enabled for verification. A minimal plugin build does not
prove compatibility with the commissioned application.

```sh
lmms studio /path/to/session --config /path/to/config.xml
```

Studio mode selects `QCoreApplication` inside LMMS. Its default
`Engine::init(true)` startup uses dummy hardware devices. Send one JSON
object per input line, or an ordinary natural-language line; responses are JSON
objects on stdout. Ordinary text becomes a persisted `session.chat` request.
EOF drains an active job to a completion/pause/budget boundary before shutdown;
`session.shutdown` pauses work and closes after cooperative execution unwinds. The initial `ready`
event reports the loaded session revision, application type and native transport
backend state. Before second-stage argument/configuration processing and engine initialization,
studio reserves the inherited output stream for protocol frames and redirects
ordinary native stdout diagnostics to stderr. The private descriptor/handle is
not inheritable by hosted child processes; Windows standard handles and CRT
descriptors are both updated. The private stream closes after native shutdown;
ordinary stdout remains directed to stderr during static/plugin teardown. Scoped
native callers can request restoration instead; the tests cover both lifetimes.
Native stream isolation and write-failure propagation are implemented in source
but require Windows/POSIX execution evidence. Protocol or diagnostic writes can
still block on a stalled output sink; they have no hard deadline. Callers consuming
JSON must keep stdout and stderr separate.

Live startup uses the same device factories as graphical LMMS, with no window:

```sh
lmms studio /path/to/session --config /path/to/config.xml --studio-live
```

`--studio-live` opens the audio/MIDI backends in the existing LMMS configuration.
Optional `--studio-audio <name>` and `--studio-midi <name>` select a compiled native
backend for this run. Both require `--studio-live`. `auto` requests LMMS's native
probing order; exact backend names come from `audio.backend.list` in a default
studio session. Unsupported names and duplicate/missing selections are rejected
before opening an engine/session. Backend-specific device/port settings remain
the existing native configuration. Temporary CLI selections are restored in
ConfigManager immediately after initialization so later configuration saving
cannot retain them. `requestedName` starts with the startup selection; an empty name
means automatic native selection. A successful `audio.backend.select` or committed
`midi.backend.select` replaces that backend's request/current name for this process.
Inspection distinguishes the loaded
configuration, requested backend and actual selected/fallback backend.

Native startup selection retains fallback to dummy audio/MIDI when a configured device
cannot open. `startFailed` reports audio factory fallback; it is not a universal
hardware health check. The MIDI backend reports actual dummy/device kind and its
engine name, without claiming port connectivity. Selecting a live startup path
with explicit dummy backends still has no sound output or input. Runtime backend
replacement, capture arm/stop transactions, device/port enumeration, latency and
hardware verification remain outstanding. Existing JACK shutdown notifications
now preserve graphical dialogs when a GUI exists and report diagnostics in
headless mode; the MIDI shutdown callback queues presentation onto its Qt thread.
JACK reconnect state, shared MIDI ports and platform lifecycle behavior still need
native failure tests.

Native processing status now separates normal stopped/running state from an
explicit backend failure. Inspection reports `processingFailure` (`none`, `start`,
`stop`, `disconnected`, `invalidCallback`, or `exception`) and the backend's signed
`nativeError` code from one atomic snapshot. `startFailed` continues describing
factory fallback, which is a different event. Normal stop retains a reported
failure until an explicit new start; repeated start of a running device does not
restart it. C++ control-hook exceptions retain their original propagation after
failure publication. This status only covers errors that a backend reports, and
does not prove physical device health. PortAudio start/stop/stream completion,
JACK activation/server shutdown/callback retirement, PulseAudio stream lifecycle and ALSA PCM
preparation/transfer/stop and sndio startup/transfer/poll/stop are connected to it
in source. Shared JACK reconnection now has an owner-thread recovery path; native execution and hardware
verification remain outstanding for all these source changes.

Capture-capable device failure increments the native input-loss epoch before its
failure state becomes observable. Input buffers carry their producing epoch;
active takes and newly started takes encountering stale pre-disconnect PCM reject
that input. `inputErrorEpoch` is reported as a decimal string to preserve all
64 bits. Fresh native input after recovery receives the current epoch. Normal
intentional device stop is not itself reported as input corruption.

PortAudio's production callback now feeds selected interleaved float input into
the native stereo input bus: mono duplicates its channel, and wider input retains
its first two channels. All supplied input channels must be finite, including
channels outside that stereo mapping. Underflow/overflow flags, missing input or
nonfinite PCM reject the take instead of publishing a surviving prefix. Playback
can continue while that take fails. Input-only streams still advance the existing
native DSP clock and discard its output. Channel selections must fit the physical
device and the existing native buffer-view channel type. Callback frame/byte
bounds are checked before buffer access; 256-frame stack scratch blocks avoid a
new callback conversion allocation. Native input accumulation, DSP and plugin
calls can still allocate or wait. PortAudio callback exceptions are contained,
usable output is silenced on abort, and no PortAudio API is called from the audio
or stream-finished callback. Initialization failure now returns through native
factory fallback, and unavailable stream objects are not stopped/closed.

Three additional backend health cases have five native executions; three
conditional PortAudio callback cases have 13 executions. They exercise native
status, input buffers, DSP, recording and clip publication with diagnostic PCM.
They are authored but unrun, and do not open a real PortAudio device. Physical
capture, latency/start offsets, monitoring/routing, callback timing guarantees,
stop/close failures and the complete conversational recording transaction remain
unverified. PortAudio's documented
[callback and input-loss contract](https://files.portaudio.com/docs/v19-doxydocs/portaudio_8h.html)
supports these source changes, not a claim of native execution.

```json
{"id":"discover","op":"studio.capabilities","args":{}}
{"id":"new-project","op":"project.new","args":{"empty":true}}
{"id":"lead-track","op":"track.add","args":{"type":"instrument","name":"Lead"}}
{"id":"lead-synth","op":"instrument.load","args":{"track":0,"instrument":"tripleoscillator"}}
{"id":"lead-clip","op":"clip.add","args":{"track":0,"position":0}}
{"id":"first-note","op":"note.add","args":{"track":0,"clip":0,"position":0,"length":48,"key":60}}
{"id":"render","op":"audio.render","args":{"path":"/path/to/song.wav"}}
{"id":"measure","op":"audio.analyze","args":{"path":"/path/to/song.wav"}}
```

Timing is native LMMS ticks, not seconds. MIDI keys are 0â€“127, with key 60
corresponding to C4. `project.inspect` exposes the meter's actual ticks per bar.
Note volume uses LMMS's 0â€“200 scale rather than MIDI's 0â€“127 velocity scale.
Parameter bounds and step size come from the native model.

## Bounded project observation and arrangement pages

`project.inspect` without arguments, or with only `section: "full"`, retains the
complete legacy snapshot. For large arrangements, request `section: "summary"`
then inspect the native contents in pages:

```json
{"id":"overview","op":"project.inspect","args":{"section":"summary"}}
{"id":"track-page","op":"project.inspect","args":{"section":"tracks","container":"song","offset":0,"limit":128}}
{"id":"clip-page","op":"project.inspect","args":{"section":"clips","container":"song","track":0,"offset":0,"limit":128}}
{"id":"note-page","op":"project.inspect","args":{"section":"notes","container":"song","track":0,"clip":0,"offset":0,"limit":128}}
{"id":"large-load","op":"project.load","args":{"path":"/path/to/large.mmp","summary":true}}
```

The summary reports `inspection: "summary"`, native tempo/master settings, meter,
song length, loop markers, a compact transport state, and counts for mixer
channels/routes, controllers and global automation clips. `nativeWarningsPresent`
indicates current native diagnostics; `nativeWarningCount` counts distinct
messages and `diagnosticGeneration` identifies the current process-local warning
state. It includes at most 16 track headers
from each owner in `tracks` and `patternTracks`; `trackPages.song` and
`trackPages.pattern` contain their continuation metadata. Track headers include
native indices/types, mute/solo state, clip counts, effect counts and routing where
applicable. Names and instrument identifiers use UTF-8 previews of at most 256
bytes, with corresponding `nameTruncated` or `instrumentTruncated` flags. A summary
is checked against a 64-KiB serialized-object budget. It does not traverse clips,
notes, curve points, plugin resources or full MIDI/backend inventories.

| Section | Accepted selection and paging fields | Returned inventory |
| --- | --- | --- |
| `summary` | None | Bounded header, counts and first track headers |
| `full` | None | Existing complete snapshot |
| `tracks` | `container`, `offset`, `limit`, `names` | Native track headers; no nested clips |
| `clips` | Required `track`; optional `container`, `offset`, `limit`, `names` | Clip metadata, sample settings, note/step counts and automation counts; no notes or curve points |
| `notes` | Required `track` and `clip`; optional `container`, `offset`, `limit` | Native note indices, positions, lengths, keys, volumes, panning, types and active detuning metadata |
| `channels` | `offset`, `limit`, `names` | Native mixer channels, gain/mute/solo, routing/effect counts and direct control handles |
| `routes` | `offset`, `limit` | Native send indices, source/destination channels, amounts and parameter handles |
| `controllers` | `offset`, `limit`, `names` | Native rack controller headers and LFO wave metadata; no parameter expansion |
| `globalAutomations` | `offset`, `limit`, `names` | Native hidden automation clip indices and metadata; no points or targets |
| `warnings` | `offset`, `limit`, `messageOffset`, `messageLimit`, `diagnosticGeneration` | Current native message chunks, occurrence counts, saturation flags and nested text pagination |

Warning entry pages use the existing default 128 / maximum 1024 item limit and
512-KiB item-array budget. Pass the summary's `diagnosticGeneration` to reject
stale entry or message offsets. Entry order follows the unchanged native hash;
rediscover after diagnostics change or process restart. Repetition and clearing
change the generation too. It is transient and is not a durable cross-process
identifier or project revision. Work-callback changes are checked before reading
the next entry and before publication, so a mixed-generation page is refused.

Each warning includes `index`, `message`, `occurrences`, `occurrencesSaturated`
and `messagePage` with `total`, `offset`, `limit`, `nextOffset` and `hasMore`.
`messageOffset` addresses UTF-16 code units; `messageLimit` defaults to 1024 and
accepts 2..8192. Valid surrogate pairs stay together, so a chunk can be shorter
than the requested limit. Starting inside a pair is refused. For a long message,
keep the entry `offset` fixed, set `limit: 1`, and follow its nested
`messagePage.nextOffset` using `messageOffset`. Top-level `nextOffset` advances
warning entries instead. An offset past the text end returns an empty completed
chunk. Counts saturate at the native signed integer maximum; a saturation flag
means the exact repetition total is no longer known. Reading warnings does not
clear them, edit the project or acknowledge repair.

These pages describe the live native accumulator. Failed project loads remain
strict: the session rolls back to its prior checkpoint and retains attempted-load
diagnostics in the failed result's `nativeWarnings`. Receipt replay reports that
historical result, while a fresh warning read describes current state. Payload
bounds apply to successful warning pages, not total resident warning storage,
legacy full inspection, native error summaries or failed-result warning strings.
Paging skips preceding hash entries under cooperative cancellation; it does not
prove constant work, hard deadlines, complete compatibility or musical judgment.

StudioWarningInspectionTest authors nine cases / 27 scenarios for native counts
and generation, escaped byte-budget pages and historical receipt replay, Unicode
message reconstruction, stale and invalid selectors, cancellation/event exclusion,
work-callback mutation, both native agent protocols and failed native project
load/checkpoint rollback. All remain uncompiled and unrun.

`container` applies only to tracks, clips and notes. It defaults to `song`;
`pattern` selects the native PatternStore. Notes require a MIDI clip. Mixer, rack
and hidden global automation sections do not accept a container selector. Fields that do not apply to the requested section are rejected
before reading its contents. Paged arrays use the requested section name and report `total`, `offset`, `limit`, `nextOffset` and `hasMore`.
Offsets are nonnegative integers; limits default to 128 and range from 1 to 1024.
Each array also has a 512-KiB serialized-byte budget, so a page can contain fewer
items than requested. Continue at `nextOffset` while `hasMore` is true, preserving
the other selectors and the current project revision. Indices are native editable
indices; rediscover them after structural edits or restoration.

Full names and resource paths are included by default on inventories that accept
`names`. A single
item exceeding the byte budget fails explicitly. Retry with `names: false` for
bounded `namePreview`, `instrumentPreview` and sample `pathPreview` fields and
matching `*Truncated` flags. Previews identify an incomplete view; they never
replace the native stored text. Note detuning pages report point counts,
progression and tension; use `note.detuning.inspect` for the full native curve.
Use `parameter.list` with a discovered `channel` or `controller` for paged control
details, and `effect.inspect` for channel effects. A global automation inventory
index selects `automation.global.inspect` with `global`. Use existing instrument,
resource, MIDI, transport and tuning capabilities for their detailed state.

`project.new`, `project.load` and `project.import` accept `summary: true` to return
the same compact summary after the native operation. Omitted or false preserves
the legacy full response. This allows a large native project to be loaded without
serializing its entire arrangement into the load receipt. Native load validation,
checkpoint publication, failure rollback, revision and replay rules still apply.

Automatic agent observations request this summary directly under cooperative
cancellation and resource checks. They no longer first construct the full project
and then discard notes and curve points. Page and summary reads suspend Qt event
dispatch while native model pointers are held, and cancellation returns failure
without a partial result. The full legacy snapshot, other capability inventories,
native getters, project loading/checkpoint memory, working memory and accumulated
provider history remain outside these response budgets. Native execution and
CPU/RSS behavior require a built application.

## Bounded automation reads and history responses

`automation.inspect`, `automation.global.inspect` and `note.detuning.inspect`
accept `section: "full"`, `"summary"` or `"points"`. The first two also accept
`"targets"`. Omitting the section preserves the existing full response.
`position` remains an optional native evaluation tick, independent of the page's
starting position. Summary metadata reports `exists`, `pointCount`, interpolation,
tension and native automation state; non-note curves also report `targetCount`.
Reading an absent global or note-detuning curve never creates it.

```json
{"id":"mixer-page","op":"project.inspect","args":{"section":"channels","offset":0,"limit":128,"names":false}}
{"id":"global-page","op":"project.inspect","args":{"section":"globalAutomations","offset":0,"limit":128}}
{"id":"curve-head","op":"automation.global.inspect","args":{"global":0,"section":"summary"}}
{"id":"curve-points","op":"automation.global.inspect","args":{"global":0,"section":"points","limit":128}}
{"id":"curve-targets","op":"automation.global.inspect","args":{"global":0,"section":"targets","offset":0,"limit":128,"names":false}}
{"id":"undo-compact","op":"session.undo","args":{"summary":true}}
{"id":"redo-compact","op":"session.redo","args":{"summary":true}}
```

Point pages use the native ordered tick map. Omit `fromPosition` for the first
page, then pass the returned `nextPosition` while `hasMore` is true. The starting
tick is inclusive and supports the native signed 32-bit range; continuation is the
actual next key, without incrementing it. This preserves sparse and signed keys
and the maximum integer endpoint. `nextPosition` is null on the last page. Native
`lowerBound` seeks directly to the requested tick. Point responses report `points`,
`total`, `limit`, `fromPosition`, `nextPosition` and `hasMore`; they preserve incoming
and outgoing values, both tangents and tangent locking. They do not accept `offset`
or `names`.

Target pages use `offset`/`nextOffset` and report the native slot `index`,
`available`, parameter handle, name and bounds. Counts describe slots; an unavailable
pointer is reported explicitly without a fabricated handle. `names: false` returns
at-most-256-byte UTF-8 name previews with truncation flags. Full-name items exceeding
the byte budget fail explicitly. Point and target pages default to 128 items,
accept limits from 1 to 1024 and have a 512-KiB array budget. Fields belonging to a
different section are rejected. Keep selectors and revision unchanged during
continuation; rediscover after editing or restoring native objects.

`automation.global.inspect` requires exactly one of the existing `parameter`
selector or a `global` clip index obtained from the inventory. Index selection can
inspect empty or unbound native curves. Empty bound-curve evaluation uses its first
native target, matching parameter selection; unbound empty curves use the native
fallback value. Other global automation operations continue selecting a parameter.

The following operations accept optional `summary: true` for a bounded
acknowledgement after their native mutation: `controller.add`,
`automation.connect`/`.disconnect`, `automation.global.edit`/`.removePoint`/`.clear`,
and `note.detuning.edit`/`.removePoint`/`.clear`. Controller creation returns its
index and one controller header; curve mutations return counts and metadata.
Omitted or false retains the existing full response. Point writes and arrangement
interpolation/deletion operations already return compact results.

`session.undo` and `session.redo` also accept `summary: true` to return the bounded
project summary after restoring a complete checkpoint. Other arguments are
rejected before restoration; omitted or false preserves the full response. Both
operations are now individually declared native agent tools, with the existing
128-declaration budget maintained. The agent must execute restoration separately
and rediscover indices/handles before subsequent edits. Native history, revision,
rollback, receipt replay and conversation bookkeeping remain the runtime's
responsibility. Replaying an acknowledged old request returns its prior response
without repeating the restoration or native edit.

These limits bound serialized inventory pages and compact acknowledgements.
Native getters, complete model discovery for parameter lookup, evaluation,
mutations, serialization and checkpoint restoration retain their own allocation
and execution costs. Full legacy responses remain available and unbounded by
these page limits. Source implementation and scripted fixtures do not establish
native timing, memory usage, musical judgment or ecosystem compatibility.

## Configuring reasoning and audio understanding

Select an explicit provider endpoint and model. No model, hosted account or API
credential is selected automatically. Both reasoning protocols execute inside
LMMS through Qt Network: Responses and Chat Completions. A local endpoint can
implement either supported protocol. The provider proposes tool calls; only the
declared LMMS native capabilities execute them.

```json
{"id":"provider","op":"session.provider.set","args":{"role":"reasoning","configuration":{"protocol":"chat","endpoint":"http://127.0.0.1:8000/v1/chat/completions","model":"YOUR_LOCAL_MODEL","timeoutMs":120000,"maxOutputTokens":4096}}}
{"id":"intent","op":"session.chat","args":{"text":"Continue this editable composition with a quieter second section, render it, evaluate it and revise the mix.","maxRounds":64,"maxOperations":512,"maximumWallTimeMs":1800000}}
```

For an authenticated Responses service, set `protocol: "responses"`, the full
endpoint (for example `https://api.openai.com/v1/responses`), your explicit model,
and `keyEnv` naming an environment variable. The actual credential is read only
when sending a request; session configuration stores its variable name. Endpoints
reject user-info, query strings and fragments, and authenticated remote HTTP.
Requests do not follow redirects, have bounded request/response sizes and a
cooperative cancellation/deadline check. Function arguments are bounded to 512 KiB
to leave room in the native request envelope. Defaults are 120 seconds and 4,096 output
tokens; the configuration range is 100â€“600,000 ms and 64â€“32,768 tokens. A named
environment variable must be populated before LMMS starts.

HTTPS also requires a usable Qt TLS backend. Windows Qt 6 installation rules now
include available Schannel/OpenSSL backend plugin targets and scan their linked
dependencies, following [Qt's TLS deployment guidance](https://doc.qt.io/qt-6/ssl.html#considerations-while-packaging-your-application).
Qt 5 OpenSSL runtime dependencies and packaged Qt 5/6 HTTPS need real installation
tests; absent TLS reports an inference error. No TLS handshake or packaged build
has been verified here.

Audio understanding has a separate `role: "audio"` configuration. It currently
requires the Chat protocol and an explicitly selected audio-capable model/endpoint;
the reasoning model need not support audio. `audio.listen` submits actual base64
PCM WAV input, with `modalities: ["text"]`. No transcription or RMS calculation
stands in for perceptual evaluation. Missing or incompatible audio providers return
errors. Request/response compatibility and musical quality require live validation
for each selected provider.

The adapters follow the official [function-calling protocol](https://developers.openai.com/api/docs/guides/function-calling),
[stateless reasoning continuation](https://developers.openai.com/api/docs/guides/reasoning),
and [audio input protocol](https://developers.openai.com/api/docs/guides/audio-chat-completions).
Responses reasoning items, including encrypted continuation, are retained and
replayed with function outputs. Optional native arguments use non-strict tool
declarations and LMMS's own validation. There is no fixed song template or
keyword-to-edit mapping in the production runtime.

`studio.capabilities` returns the current operation schemas. The inventory includes
project load/save/new/inspection, native import and MIDI export; song tempo/gain/pitch and loop markers; track and
clip editing/cloning/splitting; sample loading and source playback edits; notes and beat steps; native instrument and preset loading;
plugin and parameter discovery; checked instrument resources and sound-bank programs; native tuning libraries and Scala exchange; parameter edits and tempo sync; rack controllers, waveforms and connections; arrangement/global automation and per-note detuning;
mixer channels and sends; ordered effects; native rendering and technical audio
measurement. The model also receives memory, plan, listening, context compaction,
history retrieval and explicit task outcome tools. The catalog has 165 native
operations plus ten agent tools. Inference requests have a local budget of 128
function declarations: all ten agent tools, the `studio_execute` catalog adapter,
and up to 117 native functions. Discovery, rendering, recording, analysis and the
protected native editing operations are reserved; remaining slots fill in catalog order. Every native operation remains callable through
the adapter, including operations without an individual declaration. This budget
is an LMMS compatibility choice, not a claim about every provider's current limit.
`studio_capabilities` supplies the complete schemas; for example, a model can call
`studio_execute` with `{"op":"mixer.remove","args":{"channel":2}}`. Agent-only
functions remain individually declared. The adapter accepts only exact native
catalog names and object arguments; it does not dispatch session configuration,
shutdown, or agent lifecycle commands. Older persisted direct calls still resolve.

Native calls now preflight the published input schema before dispatch, including
nested objects/arrays, required and additional keys, finite numeric types, declared
bounds/enums, array sizes/uniqueness and string length/pattern constraints. Errors
identify an `args` property/index path. Preflight does not insert defaults or change
arguments. Native owner/plugin/transport and cross-field admission still follows;
passing preflight alone does not prove a native operation can execute.
`container` defaults to `song`; `pattern` addresses PatternStore's
instrument/sample/automation tracks. Adding/cloning/removing a song pattern track
uses LMMS's native pattern management. PatternStore owns its internal clip indices
and positions; the adapter rejects arbitrary deletion/movement of those clips.
`clip.clone` can copy to a compatible destination track. Arrangement destinations
require `position`; PatternStore destinations require an existing `targetClip`
and `replace:true`, preserving the number and order of pattern columns.

Effect operations target either `channel` or `track` plus an optional `container`.
LMMS plugins can receive descriptor-specific `attributes` discovered through
`plugin.list`; set `subplugins: false` for descriptor-only discovery. Host
compatibility and subplugin discovery still require runtime verification. Mixer routing uses native
feedback-cycle rejection. `parameter.list` exposes current model IDs; rediscover
them whenever the project is restored. Index and parameter requests can include
`expectedRevision` to reject edits against obsolete state.

`instrument.inspect` addresses `track` and optional `container`. It exposes the
native instrument, file resources, parameter handles, arpeggio state, `slicingSupported`, `programCount`
and `programSelection`. Program selection reports the native MIDI bank/program,
whether that pair is available, and its name when available. Parameter
metadata includes `kind`, `modelPath`, `writable`, `automatable`, actual bounds, effective/manual values,
controller connections and tempo-sync state. Native combo boxes expose their
current `choices` as paged numeric value/name pairs without loading GUI pixmaps. Waveform
and modulation labels are also provided for TripleOscillator and LFO selectors,
along with AudioFileProcessor's native loop modes.
The native chord tables include names, indices and semitone intervals; they are
LMMS's existing instrument-function tables. Set `choices: false` to omit choices
and those tables from an instrument inspection. `parameter.inspect` inspects one
current handle. `parameter.list` can filter by `track`/`container` or `controller`;
these filters cannot be combined. Discrete integer/boolean controls reject
fractional values. Arpeggio, microtuning, MIDI CC and oscillator anti-alias controls
have native model owners so discovery can reach them. Instruments are explicit
discovery roots because their native QObject parent is intentionally null;
instrument-owned controls still resolve to their actual track. Complete selector
metadata across all plugins still requires coverage and execution evidence.

Parameter inventories are paged. `parameter.list` and `instrument.inspect` accept
`offset` (default zero) and `limit` (default 128, maximum 1024). The list operation
returns `total`, `offset`, `limit`, `nextOffset` and `hasMore` beside `parameters`;
instrument inspection places that metadata in `parameterPage`. Continue with the
returned `nextOffset` until `hasMore` is false. Each parameter array is bounded to
512 KiB, so a page may contain fewer than `limit` entries. Filters apply before
paging. Use the same selectors and `expectedRevision` across requests; restart
discovery after edits, restoration or a plugin control-layout change.

Embedded choice lists also default to 128 entries. A model with choices reports
`choicePage`; continue with `parameter.inspect` using that model's `parameter`
handle and `choiceOffset`/`choiceLimit` (maximum 1024). Choice arrays are bounded
to 64 KiB. `choices:false` omits labels while retaining native ranges and writable
handles. One entry that exceeds its array's byte budget is explicitly refused;
the caller can request fewer details or omit choices. These are response bounds,
not truncation of the underlying native control or choice layout.

Hosted native parameters can override the model's value type and edit/automation
capabilities. Carla publishes disabled/output controls as non-writable and uses
its native automatable hint for controller and automation connections. Direct
parameter edits reject non-writable controls; new controller connections, clip
automation bindings, and global automation point/settings edits reject controls
that do not support automation. Inspection and removal of existing bindings remain
available. Integer/boolean hints also apply to Carla's float-backed models, and
finite integer scale points are exposed as choice labels. Fractional scale-point
labels are not represented by the current integer-label interface.

Carla synchronizes native values during parameter/instrument inspection with
processing suspended, and before saving LMMS knob settings. This catches native
changes while its editor is closed without replacing stored manual values on
automated or controller-bound controls. Native output values cannot become
writable through saved project metadata.

Carla project/preset XML is validated before invoking native restoration. Missing,
ambiguous or unsupported state reports a native load error. Failed or invalid
state serialization marks the native DataFile invalid, preventing project,
preset and checkpoint publication. An exception during native restoration keeps
saving disabled until a successful restore replaces the uncertain state. These
checks do not confirm every third-party plugin's internal load outcome: Carla's
native `set_state` callback returns no success value. Real Carla compatibility,
hosted asset recovery and native test execution remain unverified.

`host.inspect` addresses `track` and optional `container`. It reports whether the
instrument exposes native hosted-plugin management, with a `reason` when
unavailable. `instrument.inspect` also reports the `hostManagement` flag. For an
available host, inspection returns recognized format names and the current
plugins: `index`, `format`, `name`, `label`, `path`, `maker`, decimal-string
`uniqueId`, and `active`. The Carla bridge attaches to the instrument's existing
engine; it does not require the Carla editor.

All hosted-plugin edits address the same `track` and optional `container`:

| Operation | Additional arguments | Native change |
| --- | --- | --- |
| `host.plugin.add` | Required `format`; optional `path`, `name`, `label`, `binary`, `uniqueId` | Append a plugin; return its index as `added`. |
| `host.plugin.remove` | `hostedPlugin` | Remove the plugin at the current index. |
| `host.plugin.rename` | `hostedPlugin`, nonempty `name` | Rename the native instance. |
| `host.plugin.swap` | `hostedPlugin`, `targetPlugin` | Exchange two current positions. |
| `host.plugin.active` | `hostedPlugin`, boolean `active` | Change native activation and check the resulting state. |

Successful edits return a fresh host inspection. Indices are zero based and must
be rediscovered after adding, removing, swapping or restoring plugins; requests
can include `expectedRevision`. The five edits use StudioRuntime's existing
checkpoint and rollback path. Direct internal capability calls report failures
but do not themselves roll back a partially changed native engine.

Native Carla plugin removal and swapping remap the existing LMMS parameter model
objects with their controls. Automation/controller bindings and saved model IDs
therefore follow surviving controls within the host's native parameter window.
Removing a plugin deletes its own model targets and detaches their bindings.
Appending a plugin preserves existing targets. Synchronous parameter-reload
callbacks are deferred until the structural edit and model remapping finish.
If layout counts are unavailable while controls have bindings, or a swap would
move a bound control outside the native window, the edit fails before mutation.
The pinned Carla engine exports 110 flattened slots; extending automation beyond
that native interface is unfinished. Direct structural edits inside Carla's own
editor or a plugin are not yet reconciled by this bridge.

Recognized Carla formats are `internal`, `ladspa`, `dssi`, `lv2`, `vst2`, `vst3`,
`au`, `dls`, `gig`, `sf2`, `sfz`, `jsfx`, and `clap`. This list describes accepted
API requests, not installed plugins or working backends. `internal` requires its
native `label`; `lv2` requires its URI in `label`. Other formats except `au`
require an existing `path`; any supplied nonempty path must exist. Directories
are allowed for plugin bundles. `binary` defaults to the process's native
platform/word size; explicit values are `posix32`, `posix64`, `win32`, or `win64`.
The selected binary still needs a compatible native Carla backend or bridge.
`uniqueId` defaults to zero and must be a signed 64-bit decimal **string** when
provided, preserving identities outside JSON's exact numeric range.

The management bridge requires compatible Carla headers and runtime symbols;
missing support reports an explicit unavailable reason. Hardware patchbay routing,
host-specific load options and dependency bundling inside opaque Carla state
remain unfinished. Actual library loading, plugin execution and recovery require
native verification.

`host.inspect` reports internal routing availability in `patchbay`. The following
operations address `track` plus optional `container` and require a Carla Patchbay
instrument. Carla Rack keeps its native serial chain and reports this interface
unavailable. Missing optional graph symbols leave plugin management, parameters
and discovery available when their own APIs are present.

| Operation | Arguments | Result |
| --- | --- | --- |
| `host.patchbay.inspect` | `track`, optional `container` | Refresh the internal graph and return clients, ports and connections. |
| `host.patchbay.connect` | `track`, `sourceGroup`, `sourcePort`, `destinationGroup`, `destinationPort`; optional `container` | Connect an output to an input of the same signal type and return a confirmed graph. |
| `host.patchbay.disconnect` | Same endpoint arguments as connect | Resolve the current connection ID, disconnect it, and return a confirmed graph. |

All three operations count as project mutations. Carla's native refresh removes
illegal connections before emitting its graph, so inspection also receives the
studio runtime's checkpoint, revision and rollback handling. `host.inspect`
reports availability without refreshing the graph.

Graph results contain `clients` with `group`, `hostedPlugin`, `icon` and `name`;
`hostedPlugin: -1` identifies native I/O clients. `ports` identify their `group`
and `port`, `direction` (`input` or `output`), `signal` (`audio`, `midi` or `cv`),
`name`, native `portGroup` and raw `hints`. Connections include their native `id`
and the four endpoint fields accepted by the edit operations. `external` is
always false: these operations route inside the instrument's existing engine.
No external hardware graph is selected.

Use endpoints from a current snapshot; group IDs are different from hosted-plugin
indices. Endpoint arguments accept unsigned 32-bit JSON integers. Structural
edits and project restoration can invalidate them. Native connection IDs can
change on every refresh and are diagnostic values, never mutation arguments.
Connecting an existing route or disconnecting an absent one fails explicitly.
Direction/type mismatches and missing endpoints fail before the requested native
edit; the preceding refresh can still prune invalid native routes. Native graph
rules decide other constraints, including feedback connections.

Snapshots are limited to 65536 total client, port and connection records. Partial,
malformed, duplicate, dangling or asynchronous graph metadata causes failure.
LMMS installs its observer while the instrument's engine is empty and inactive,
copies callback strings while they are valid, and collects graph records only
during refresh. Its observer remains alive through native engine cleanup so
runner threads cannot call a freed context. Unexpected preloaded engines report
routing unavailable instead of replacing a running engine's callback slot.
Unrelated parameter/transport callbacks are ignored by
this observer; Carla's independent native-host and editor notifications remain
under Carla's control. Native bool success is followed by a graph refresh to
confirm the requested route. Failure after a partial native edit propagates to
StudioRuntime for checkpoint rollback; direct capability calls lack that durable
transaction boundary. Actual Carla engine routing, audio, editor concurrency and
native project restoration still require compiled execution evidence.

`host.inspect` reports native discovery availability in `discovery`. Catalog
operations use Carla's own discovery API and address `track` plus optional
`container`; they do not add plugins or modify project content.

| Operation | Arguments | Result |
| --- | --- | --- |
| `host.catalog.start` | Required `format`; optional `binary`, `path`, `discoveryTool`, `maximumPlugins`, `timeoutMs` | Begin a scan and return its ID, status and first result page. |
| `host.catalog.poll` | Required `scan`; optional `offset`, `limit` | Advance native discovery once and return a page of retained candidates. |
| `host.catalog.stop` | Required `scan` | Request native cleanup and retain collected results. |

Discovery accepts the same format names listed above. `binary` defaults to
`native`; internal, LV2, SFZ, JSFX, DLS, GIG and SF2 discovery require the native
binary type. `path` is one existing search directory. It is optional for internal
and LV2 discovery and required for the other formats. Cached internal/LV2/SFZ/JSFX
discovery runs inside Carla. Binary formats use Carla's installed discovery
helper; `discoveryTool` can specify its path, otherwise LMMS checks for a native
or matching binary helper beside the loaded Carla library. Matching bridge
dependencies still need to be installed for foreign binaries.

`maximumPlugins` defaults to 4096 and accepts 1 through 65536. `timeoutMs` defaults
to 60000 and accepts 1 through 600000. The deadline is checked when polling;
native startup, directory enumeration, callbacks and cleanup cannot be preempted
by this interface. Poll with `offset` default zero and `limit` default 128
(1 through 1024). Results include `scan`, `state`, `error`, `plugins`, `offset`,
`total`, `hasMore`, and `elapsedMs`. `total` and `hasMore` describe retained records
so far. Continue polling while `state` is `running`, even when `hasMore` is false.
Advance the offset by the number of records consumed to avoid reading them again.

Terminal states are `finished`, `stopped`, `timedOut`, `limited`, `failed`, and
`noHandle`. `limited` retains partial results after reaching the record cap.
`noHandle` preserves the native API's ambiguity between no candidates and startup
failure. `finished` means the native discovery loop ended; Carla may have skipped
unsupported or failed binaries, and discovery does not prove load or audio
compatibility. Read the scan state even when the enclosing operation returns
`ok: true`.

Each candidate includes a `load` object with format, binary, path, name, label and
exact signed decimal `uniqueId`, ready to combine with `track`/`container` for
`host.plugin.add`. Other metadata includes maker, native category, instrument
hint, binary hash when supplied, and audio/MIDI/CV/parameter I/O counts. LV2 uses
its URI; SFZ file labels and JSFX paths relative to the search directory are
converted into load paths. Dependencies and JSFX import-root configuration still
need compatible native host settings.

One scan runs per instrument instance. Stop or finish it before starting another.
Old IDs become invalid when a new scan begins. Instrument destruction releases
the native scan handle. Scan state and retained metadata are not a durable plugin
inventory and cannot resume after instrument replacement or application restart.
Native library symbols, helper processes and installed plugin scanning remain
unverified in this environment.

`host.parameter.list`, `host.parameter.inspect`, and `host.parameter.set` address
controls inside a hosted plugin using `track`, optional `container`, and
`hostedPlugin`. `host.inspect` reports their availability in `parameterAccess`,
including a reason when optional parameter symbols are missing. Plugin management
remains available when those parameter symbols are absent.

`host.parameter.list` accepts `offset` (default zero) and `limit` (default 128,
1 through 1024). It returns `parameters`, `total`, `offset`, and `hasMore`.
Each parameter includes its current `index`, plugin-facing `nativeIndex`, name,
symbol, unit, comment, group, numeric value and custom display text. Metadata
includes native bounds, default, three step sizes, number/integer/boolean `kind`,
and enabled, output, writable, automatable, logarithmic and saved flags. The
`scalePointCount` reports available choice labels without copying them into each
list entry.

`host.parameter.inspect` requires `hostedParameter`, the current native parameter
index. It returns that metadata plus `choices`, `choiceOffset`, and
`hasMoreChoices`. Choice pagination uses `choiceOffset` (default zero) and
`choiceLimit` (default 128, 1 through 1024). Choice values retain fractional
numbers; they are not restricted to integer labels. Offsets beyond the current
parameter or choice count return an empty page.

`host.parameter.set` requires `hostedParameter` and numeric `value`. Disabled,
output and read-only controls reject writes. Values must be finite and inside
the actual native range; integer controls require whole numbers and boolean
controls require one of their minimum/maximum endpoints. The response includes
fresh native metadata, `requestedValue`, the float-converted `nativeRequestValue`,
and `matchesNativeRequest`. Carla's setter returns no success status: a plugin
can quantize or ignore a write, so the result reports readback explicitly. A
mismatched readback is visible with `matchesNativeRequest: false`; exceptions
or invalid readback fail the operation and use the studio's existing rollback
boundary. A successful call alone does not prove audible behavior.

Hosted plugin/parameter indices must be rediscovered after structural edits,
native layout changes or restoration. They are separate from LMMS model handles.
The `automatable` flag describes Carla's native hint; these operations do not yet
create LMMS automation bindings for every nested parameter. The `saved` flag is
also a native hint: transient controls can be edited but their persistence is not
promised. Actual third-party state restoration retains the limitations above.

`instrument.program.list` addresses `track` and optional `container`. It returns
the current native bank's cached programs and selection without constructing an
editor. Optional `bank` filters by MIDI bank, `query` matches names ignoring case,
`offset` defaults to zero and `limit` defaults to 128 (1 through 1024). `total`
counts matching programs before pagination and `hasMore` reports remaining matches.
`instrument.program.select` requires `bank` (0 through 16383) and `program`
(0 through 127). SF2 Player, GIG Player and OpulenZ reject unavailable pairs and preserve
their prior selection. Other instruments explicitly reject selection unless they
implement this interface. Structured `parameter.set` on SF2/GIG bank or patch
controls checks the resulting pair too; use `instrument.program.select` to change
both together. Legacy GUI/MIDI writes retain native model behavior.

OpulenZ provides its native 128-entry GM table in bank 0. Names currently use
numeric `GM patch N` labels. Selecting a program copies its OPL2 registers into
the native controls, including levels and envelopes; those controls are also the
project/preset state. The discovered `Patch` parameter delegates to this selection,
including explicit same-value writes. Editing patch controls subsequently makes
the sound custom and clears the program-selection marker. Custom sounds report
an empty `programSelection`. The two native waveform selectors expose their four
existing waveform names. This source implementation still needs native execution.

`tuning.list` reports LMMS's ten scale and ten keyboard-map slots. Its optional
`details: true` includes interval and mapping data; `project.inspect` includes the
concise library. `tuning.scale.inspect` and `tuning.keymap.inspect` inspect one
`slot` (0â€“9). Scale details include implicit degree-zero 1/1, native ratios,
ratio/cents representation and `intervalText` excluding that implicit degree.
Keymap details include MIDI bounds, middle/reference keys, reference frequency,
formal `octaveDegree`, `linearFromMiddle`, and `referenceMapped`. `entries` uses
JSON null for unmapped keys and signed integers for mapped degrees, including -1.
`mappingText` uses native integer/x lines.

`tuning.scale.set` requires `slot`, one-line `name`, and `intervals` text. Supply
only the degrees after implicit 1/1. Integer or rational lines specify positive
ratios; decimal lines specify cents, including negative cents. Empty text creates
the native constant-pitch, zero-degree scale. `tuning.keymap.set` requires `slot`,
`name` and `mapping` text. It accepts optional `firstKey`, `lastKey`, `middleKey`,
`baseKey`, `baseFrequency`, `octaveDegree` and `linearFromMiddle`; omitted fields
retain the current slot's values. Bounds and reference keys use MIDI 0â€“127;
reference frequency must fit a finite positive native float. Keyboard degrees and
formal octave degrees are signed native integers. Zero formal octave uses the
scale's final degree. An unmapped reference can be retained in native state and
is explicitly reported; it produces zero frequency and cannot be exported as KBM.

`tuning.scale.load`/`.save` and `tuning.keymap.load`/`.save` take `slot` and `path`
and use the same core parser and checked atomic writer as the optional GUI.
Imports check readable files, headers, declared counts and native bounds before
replacing a slot; rejected imports preserve the previous definition. The parser
accepts comments and trailing pitch annotations, empty scale descriptions,
UTF-8/BOM or Latin-1 input, and omitted trailing KBM entries as unmapped. Text/files
are bounded to 1 MiB; scales and maps to 4096 entries. Exports retain native float
precision and write UTF-8. These rules follow the primary
[Scala scale format](https://www.huygens-fokker.org/scala/scl_format.html) and
[keyboard mapping documentation](https://www.huygens-fokker.org/scala/help.htm#mappings).

Native XML adds optional `octave_degree`, `linear_from_middle` and per-degree
`mapped` attributes. Their absence retains legacy LMMS defaults, including -1 as
unmapped. New signed -1 mappings persist explicitly. An imported zero-size Scala
map starts at its middle key; legacy empty LMMS maps use absolute key degrees.
Exporting a legacy empty map writes middle key zero to preserve its frequencies
on Scala exchange; its original middle-key metadata remains in native XML.
Project clearing/replacement resets every library slot before restoring the new
project, preventing older projects without tuning data from inheriting a library.
Older LMMS versions' treatment of the added fields requires compatibility testing.

`instrument.tuning.inspect` addresses `track` and optional `container`, plus an
optional MIDI `key`; otherwise it reports all 128 keys. Frequencies use the same
native calculation as NotePlayHandle, before master/instrument pitch or per-note
detuning. `instrument.inspect` includes tuning selections; use its discovered
parameter handles to enable tuning, select slots, or change range import. Library
and selector changes invalidate active native note frequencies. Unmapped,
unreferenced, nonfinite or unrepresentable frequencies are zero; zero-frequency
notes skip instrument DSP and MIDI note-on. The native GUI delegates to these
core parsers, exposes the formal octave degree and retains full reference-frequency
precision. Actual synthesis, GUI behavior and hosted-plugin tuning compatibility
remain unverified.

`instrument.resource.set` addresses `track`, optional `container`, a discovered
`resource` key and a `path`. It delegates to a checked native plugin setter:
AudioFileProcessor and SlicerT expose `sample`; TripleOscillator exposes `oscillator1`,
`oscillator2` and `oscillator3`; SF2 Player exposes `soundfont` (SF2/SF3), and GIG
Player exposes `bank` (GIG); Patman exposes `patch` (GUS PAT), and OpulenZ exposes
`patch` (SBI). An empty path clears that resource. Nonempty paths
must resolve to readable files accepted by the native decoder or bank loader. Resource metadata
includes the current path and advertised file types, plus a selector parameter and
value where applicable. `requiredForRestore` indicates whether the nonempty path
is needed to reconstruct the native sound. OpulenZ copies imported SBI registers
into project controls, so its path is optional import provenance and this flag is
false. Empty resource paths also report false. Optional `select: true` activates that native waveform
selector; waveform loading alone retains its current selection. Loading preserves
a user-named track.
Decode/bank-loading failure keeps the prior resource; runtime rollback may replace parameter
handles. The libsndfile decoder rejects incomplete, empty and nonfinite audio,
checks native frame bounds and
now reads 4,096 frames per block with scoped cancellation checks, retaining native
mono upmix and first-two-channel selection. Scratch storage scales with block
size and input channel count rather than the entire file; decoded output still
requires its full native buffer. The Vorbis fallback now consumes per-channel
float PCM directly, checks complete frame totals and releases native state on
every exit. Callback reads return complete items, failed seeks propagate failure,
and oversized callback positions fail instead of narrowing on platforms with a
32-bit long. Vorbis-enabled discovery includes OGG even when libsndfile lacks
that format. Chained Vorbis now uses the first link's rate as its native output
rate, converts later rates through LMMS AudioResampler/libsamplerate, drains each
link and handles its own channel layout. Recognized Vorbis is routed through this
reader so libsndfile's first-link success cannot truncate a chain. Conversion
uses bounded scratch blocks and retains the native ratio/frame limits; complete
chain/corpus compatibility remains unverified. Native tests are authored but
unrun. DrumSynth now reads each native profile once under scoped transfer checks,
keeps envelope/synthesis buffers in the decoder instance and checks cancellation
between its existing 1,200-frame generation blocks. Its output is published only
after complete generation; malformed envelopes, unsafe lengths and nonfinite
or overflowing numeric conversions fail without publishing a partial sample.
The native waveform/filter/distortion generation remains authoritative. Envelope
parameters require at most 31 finite, ordered points within the existing 255-byte
value capacity; one slot remains for the native sentinel. Parameter values that
exceed their native parser capacity fail rather than being silently truncated.
Comments keep their existing display truncation. Authored native tests cover the
764 shipped profiles, nested decoding, failure/cancellation and sample loading,
but their PCM compatibility and execution are unverified. Allocation, blocked
system/library calls and individual synthesis blocks remain outside hard bounds.
Decoder fallback lists no longer contain a null entry
when Vorbis support is absent. SF2/GIG loading discovers native programs and selects
one; choose another afterward through `instrument.program.select`. Banks with no
usable MIDI programs fail explicitly. The optional GUI and native XML restoration
delegate to the same bank loaders. SF2 uses checked FluidSynth loading/selection
and UTF-8 paths with LMMS's existing minimum FluidSynth 2.2 requirement; native
Unicode-path execution and valid GIG playback remain unverified. See
[FluidSynth SoundFont management](https://www.fluidsynth.org/api/group__soundfont__management.html)
and [native program selection](https://www.fluidsynth.org/api/group__midi__messages.html).
SF2 voice references are associated with a synth generation. Native sample-rate
rebuild and explicit bank clearing retire the old generation; held notes clear
cached references before using the current pool, and old note deletion cannot
send note-off into a later generation. Voice inspection, expression, native
rendering, note counters and synth lifetime share a recursive mutex. Tracking
follows native polyphony rather than a four-voice limit, applying initial and
subsequent detuning and panning to every captured layer. The optional patch dialog
uses copied program metadata and checked, locked selection instead of retaining
a raw synth pointer; preview does not change project controls, Accept commits,
and Cancel restores their current selection. These are source changes with native
tests authored but unrun. Synth recreation now prepares independent settings,
a replacement native synth and voice scratch storage before retiring the working
pool. Preflight failure retains that pool and bank, and its resampling ratio follows
the current engine output rate. Typed native settings are copied without sharing
settings callbacks; string memory is released through FluidSynth's own allocator.
SF2 loading captures the source through 64 KiB storage into an owned temporary
directory, then passes that file to the native loader. Original resource paths
remain project provenance for plain project saves. Native bundles and Studio
checkpoints copy the accepted snapshot into their owned asset storage. Reload
loads and selects an independent bank in the replacement synth
before retiring the current pool; it no longer detaches/re-adds a shared bank.
Failed replacement loading or selection retains the working synth. Retired file
snapshots remain available to lazy native readers, with cleanup at control
operations when the bank is absent and synthesis is idle, or after native-synth
destruction. These are unrun source paths. Cache I/O, disk retention, peak memory,
crash cleanup, durable asset quotas and full SF2/SF3 compatibility still need
implementation or native verification as applicable.
Patman uses the native GUS reader and Sample DSP. Its complete candidate is
published only after every declared sample decodes successfully; malformed later
samples preserve the previous patch. It captures an owned input file before this
decoder runs and publishes the snapshot with the fully decoded candidate. Native
bundle/checkpoint serialization binds that accepted file, preserving the loaded
version after the original changes or disappears. Clearing the patch releases its
snapshot. Held notes retain their existing decoded Sample objects. The reader accepts native signed/unsigned
8/16-bit PCM and checks source rate, root frequency, complete reads, native frame
capacity and active loop bounds. `Loop sample` and `Tune sample` are discoverable
native parameters. It retains Patman's existing single-instrument/layer and
nearest-root/forward-loop behavior; complete GUS corpus compatibility and native
execution remain unverified. The GUI and native XML use the same checked loader,
and empty XML resource restoration clears stale samples. Other plugins, including hosted plugin resources, still need
checked resource adapters; their legacy `loadFile` method cannot report success. The current empty resource
list does not establish that an instrument has no file dependencies. `plugin.list`
includes each descriptor's advertised file-type CSV, which is discovery metadata.
All native plugins must be rebuilt with the changed model/plugin interfaces;
compatibility with existing native plugin binaries has not been established.

OpulenZ checks the complete four-byte `SBI`/0x1a signature and all eleven native
OPL2 register bytes before changing controls. It preserves the existing acceptance
of 47-byte files without reserved tail bytes and ignores trailing data; reads are
bounded to 52 bytes. Names use the bounded 32-byte field, stop at NUL and decode
as Latin-1. GUI file loading delegates to the checked loader and retains its naming
rule; structured loading preserves the track name. Failure leaves both sound and
provenance intact. Empty loading restores constructor control defaults, including
after XML loading has changed native model reset values.
XML retains the existing version-1 control fields and adds optional `patchIndex`,
`gmProgram` and `patchSource`. The last field records provenance and is not an
asset dependency. Restoration uses saved controls without opening the SBI file;
stored GM identity is retained only when its register values match those controls.
The existing two-operator, nine-voice OPL2 emulator remains authoritative. OPL3,
complete SBI/project corpus compatibility and actual sound correctness remain
unverified; these changes do not claim additional synthesis or tuning support.

SlicerT stages decoding and native slice analysis before replacing its `sample`;
failed loading preserves the previous source and boundaries. Empty paths clear
both. Native sample snapshots retain decoded buffers for held voices and trim
each voice to its selected source interval. Zero fade uses a finite gain, and
fade/progress timing accounts for source and output sample rates. Loading estimates
tempo with SlicerT's duration-based 100â€“200 BPM heuristic; it does not establish
musical tempo understanding. Source implementation and authored audio tests remain
unverified by native execution.

`instrument.slices.inspect` returns source frame count/rate, original BPM, tempo
sync, base key and normalized `points`. The base key plays the complete sample;
subsequent keys play individual slices. `sliceKeys` contains null for slices outside
MIDI 0â€“127, and `addressableSlices` reports the accessible count. Master pitch can
shift the effective base key outside that range; `wholeSampleKey` is then null. An unloaded
SlicerT still supports inspection and reports no points.
`instrument.slices.analyze` runs SlicerT's native spectral-flux detector with its
current threshold and snap controls. Optional `estimateTempo: true` also updates
its duration-based tempo estimate. Parameter changes alone do not rerun analysis;
invoke this operation after changing threshold/snap, or use the optional GUI reset.

`instrument.slices.edit` requires `points`: 2 through 129 finite, strictly
increasing normalized positions including exactly 0 and 1. Every interval must
contain at least one native source frame. Validation precedes replacement; these
are the same checked boundaries used by optional GUI edits. Native XML retains
the existing slicing attributes and adds optional `sampleRate` and `snap` fields.
Boundaries retain native float precision. New embedded samples retain their source
rate; legacy embedded data without a rate uses the existing engine-rate default.
Malformed stored boundaries restore the complete sample with a native diagnostic.
Empty XML source data clears the sample and boundaries.

`instrument.slices.midi` previews native notes without changing revision. Timing
uses source duration, native tempo sync and instrument pitch, rounded to integer
ticks. Notes are contiguous with no added tick overlap. Generation explicitly
rejects inaccessible keys, intervals below tick resolution, and timeline overflow.
`clip.fromSlices` inserts those same notes into the existing MIDI `clip` with an
optional nonnegative tick `position`. `targetTrack` and `targetContainer` default
to the source track/container and may address a different native song or pattern
clip. Optional `replace: true` clears existing notes only after all generated notes
and the target have been validated; otherwise notes are appended. Other instruments
reject slicing unless they implement the native interface.

`sample.load` uses LMMS's existing decoder and immutable sample buffer. It requires
a sample track and clip, a readable audio file, and a nonempty decoded buffer that
fits native signed frame bounds. `sample.edit` accepts `startFrame`, `endFrame`,
`gain` and `reversed`. Gain is a finite signed linear multiplier; a negative gain
inverts polarity. Source bounds are half-open and use playback-order coordinates.
Reversal alone mirrors the bounds to retain the same physical source region. If
bounds and reversal are submitted together, explicit bounds refer to the final
playback order. Inspection returns the path, source rate and frame count, selected
bounds, gain and reversal. Native project XML persists these settings, including
embedded sample data. Legacy projects without source bounds use the full buffer.
Embedded sample loading keeps LMMS's existing binary SampleFrame format and uses
the native Qt decoder in at most 16,384-character blocks. It accepts standard
Base64 with ASCII whitespace or omitted trailing padding, rejects malformed
characters/padding/length, incomplete frames, nonfinite audio and invalid rates,
and checks scoped cancellation while scanning and decoding. A checked factory
publishes a buffer only after complete validation. Sample clips, AudioFileProcessor
and Slicer collect native load errors for rejected data. Newly saved embedded AFP
samples include `sampleRate`; legacy AFP data without it uses the engine rate.
Studio `project.load` and `preset.load` reject collected native load errors, so
the session restores its prior checkpoint instead of committing an incomplete
project. Session recovery also rejects checkpoints that produce native load errors
after their digest and declared assets pass verification. Native regression cases
are authored but unrun. Embedded encoding also uses checked native Qt blocks:
1,536 frames per block preserve Base64 group boundaries, and the complete output
is published only after finite-audio, frame/text-capacity and cancellation checks.
Sample serializers record a transient document failure when encoding fails.
Native DataFile string/stream/file writers reject failed documents, file saving
checks stream/write status before publication, and journal capture/undo/redo
preserve their entries when sample serialization fails. The complete encoded
string and DOM still require full storage; whole-DOM parsing/writing, allocation,
compression and operating-system calls remain outside hard execution bounds.
Native encoding, file-preservation and journal cases are authored but unrun.
Qt's native
[checked Base64 result](https://doc.qt.io/qt-6/qbytearray-frombase64result.html)
supports explicit failure reporting; native PCM byte order is unchanged.
Each playback handle holds separate frame bounds; rendering does not alter the
editable source window. Decoder failure and missing resources remain visible.

Native audio recording has separate source corrections and a bounded capture
operation described below. Indefinite capture-arm/stop and runtime backend-selection
operations remain unavailable. Default studio startup
selects the native render-only dummy audio/MIDI devices, with no physical
input; explicit live startup uses the native configured factories as described
above. The native recorder now owns checked input blocks, retains the first input
rate, rejects rate changes/nonfinite/invalid input and publishes a completed take
on its clip's thread. Empty or rejected takes preserve the previous sample. Native
input-buffer failures invalidate active takes rather than publishing their surviving
prefix; per-period fault flags cover a rejected first capture period. The native
stop slot now retires active and newly queued record handles, and completed capture
state is retained on its native clip. Native save/checkpoint and SampleClip
serialization synchronously finalize pending captures on the application/clip
thread, even if the posted notification has not run. Saving during an active take
or reentrant publication fails without publishing an older clip buffer. Native
shutdown attempts completion before clearing the project. These source paths are
authored but unrun; device configuration, start offsets/latency, input monitoring, allocation budgets,
GUI lifecycles and physical input verification remain outstanding. Authored native
capture tests exercise LMMS's input bus with diagnostic PCM and do not establish
hardware recording or a complete conversational recording workflow.

`audio.record` now implements a bounded recording transaction in source. It takes
required `track`, `clip` and `durationMs`, optional `container`, `maximumBytes`,
`timeoutMs`, `playback` and `position`. The target must be an existing native sample
clip; transport must be stopped, existing recording must be disarmed/completed,
and the current device must be running and capture-capable. Default dummy devices
are rejected. The operation uses the actual native input bus and SampleRecordHandle;
it does not generate replacement audio or advance a substitute clock.

`durationMs` is an integer from 1 to 86,400,000. The frame quota is the requested
duration rounded up to a whole frame at the backend's native rate, subject to the
native sample frame limit. The final callback is truncated to that quota.
`maximumBytes` bounds the captured stereo PCM payload (default 256 MiB, maximum
exact JSON integer 2^53âˆ’1); block overhead, flattening, embedded serialization,
checkpointing and other process allocations require additional memory.
`timeoutMs` is a cooperative deadline (default duration plus 10 seconds, capped
at one day). Input/backend/rate failure, timeout and cancellation reject the take.
Native model locks, plugin execution and allocation can delay deadline handling;
this is not a hard execution or memory guarantee.

Before capture, stale input buffers are discarded and the native clip reserves
completion. Ordinary recorders cannot start on that reserved clip. The previous
sample remains authoritative while native input is collected; save/checkpoint,
journal serialization and rendering reject active or reserved capture. Completion
must contain exactly one successful take with the expected frames/rate before
publication on the clip thread. Cancellation discards reserved buffers. Backend
replacement also discards input and invalidates prior capture epochs.

`playback: true` starts the native song as accompaniment from `position` (default
target clip position). It suppresses the target's prior sample with its native
record flag, creates one reserved recorder, temporarily disables song looping,
and stops accompaniment/restores looping afterward. Ordinary clip/tempo refreshes
preserve that reserved recorder; normal recording and track destruction retain
their existing retirement paths. Input-only capture leaves song transport stopped.
Placement and other sample-clip editing follow native publication/autoResize
behaviour; detailed monitoring/routing and latency compensation remain unfinished.

Studio Runtime treats capture as a project mutation. A successful acknowledged
take gets a native checkpoint/revision and is available to undo, redo and recovery.
Failure restores the previous checkpoint and history; native object replacement
still increments revision. Replaying the same request ID returns its saved result
without opening another take. Normal busy-operation admission rules remain:
metadata inspection, agent cancellation and session shutdown can run while capture
waits. Direct capture can be interrupted through session shutdown; agent-owned
capture also observes agent cancellation and its execution budget.

For an empty project with a configured live capture backend, the source interface
accepts this sequence (native execution remains unverified):

```json
{"id":"mic-track","op":"track.add","args":{"type":"sample","name":"Mic take"}}
{"id":"mic-clip","op":"clip.add","args":{"track":0,"position":0}}
{"id":"capture","op":"audio.record","args":{"track":0,"clip":0,"durationMs":10000,"maximumBytes":33554432,"timeoutMs":20000}}
```

Five authored native recording cases have 12 executions covering frame-accurate
publication with/without accompaniment and tempo refresh, timeout/cancellation/
disconnect/nonfinite/budget/backend rejection, reservation/save/render exclusion,
durable history/replay/recovery and failed-take rollback. The whole-process harness
adds default-dummy refusal, rollback and replay checks. None has run. Physical
device capture, indefinite arm/stop, multichannel routing, monitoring, offsets,
crash durability of an in-flight take, GUI coexistence and complete autonomous
recording verification remain outstanding.

Five native transport operations are implemented in source, with native tests
authored but unrun. `project.inspect` includes the same transport observation.

| Operation | Arguments and native behaviour |
| --- | --- |
| `transport.inspect` | No arguments. Reports native play mode, playing/paused/stopped/exporting flags, current ticks, per-mode positions and loop markers, selected pattern/MIDI clip, recording state, backend metadata and the last `midiOutputCleanupError` (empty after successful cleanup). |
| `audio.backend.list` | No arguments. Reports compiled audio/MIDI factory names, loaded configuration selections and current transport/backend state. It does not probe devices or change a backend. |
| `audio.backend.select` | Required `name` from `audio.backend.list`. Recreates that exact audio backend using loaded native settings, starts its processing and returns transport/backend observation. Requires stopped, disarmed transport and no export/restoration reservation. MIDI is unchanged. |
| `midi.backend.select` | Required `name` from `audio.backend.list`. Preserves port models and requested endpoint names. Requires stopped/disarmed transport; enabled inputs require reset after transfer. Optional boolean `abandonFailedOutput` defaults to false and permits retiring only eligible terminal output, with uncertainty recorded in the result. Returns commit, previous backend, cleanup/notification counters, audio recovery state and current transport. Process-local. |
| `audio.backend.inspect` | Required `direction`: `input` or `output`; optional `offset`/`limit`. Discovers native master channels, external audio peers and their `connectedChannels`, separately from project mixer routes. |
| `audio.backend.route` | Required `direction`, zero-based `channel`, exact external `port`, boolean `enabled`. Connects or disconnects one native master edge with stopped/disarmed transport, DSP joining and verified mutation/rollback. |
| `audio.record` | Required `track`, `clip`, `durationMs`; optional `container`, `maximumBytes`, `timeoutMs`, `playback`, `position`. Runs the bounded native capture transaction described above. |
| `transport.play` | Optional `playMode`: `song` (default), `pattern`, or `midiClip`. Pattern playback accepts a checked `pattern` index. MIDI clip playback requires `track` and `clip`, accepts `container` and optional `loop` (default true), and resolves the actual native clip. |
| `transport.pause` | Required boolean `paused`. Sets an existing playback state explicitly; repeating the same value preserves it. Pausing cancels pending MIDI output and attempts release, including retries while already paused. Cleanup failure returns an error with playback still paused. Resuming requires a running backend. |
| `transport.stop` | No arguments. Calls native Song stop and honours that timeline's BackToZero, BackToStart or KeepPosition behaviour. Cancels pending MIDI output and attempts release even when already stopped; a cleanup error leaves playback stopped and can be retried. |
| `transport.seek` | Required integer `position` in ticks, `0..MaxSongLength`. Optional `playMode` defaults to the active mode, or song when stopped. During playback it must match the active mode. Uses the native timeline jump and resets its frame offset. |

Controls reject native export/device reservations and any recording flag, armed
sample/automation clip, active or pending take, or reentrant take publication.
Finalizing a take can modify the project and requires a recording transaction;
these playback commands do not supply that transaction. Stopping recording remains
a native application operation, unavailable through these five playback controls.
Invalid selection or arguments are checked before stopping or changing playback.
Deleting a selected native MIDI clip now stops playback during its destruction
signal, before its notes disappear, and clears the stored playback pointer.

Backend `kind` distinguishes `dummy`, `file`, `device` and `unavailable`.
`engineReportedName` is the engine's configured/fallback name, which can be stale
while a temporary export device is installed; `kind` examines the actual device.
Running status, capture support, render-only mode, reservation, sample rate and
native period size are also reported. Dummy playback advances the actual LMMS
clock and DSP without physical sound output or capture. A reported device does
not prove that hardware output is audible or that recording succeeds.
Playback state and pattern selection are transient controls: they do not create a
project revision or history entry, and recovery does not restart playback. A
replayed request ID returns its original observation without repeating a start,
pause, stop or seek; use a fresh ID to request a new action. Busy native operations
retain the existing inspect/cancel/shutdown admission rules. Whole-process
transport checks are authored in the executable harness but have not run.

`audio.backend.select` is a process-local native device transaction. It reserves
the old backend and its running/stopped state, stops it outside the model mutex,
constructs exactly the named compiled factory, registers existing enabled bus
ports, delivers native quality/sample-rate notifications and starts the new
device. It accepts the backend's reported running state before retiring the old
device. It does not restart song playback, clear a DSP failure latch or edit the
project. Success clears render-only mode, even when explicitly selecting Dummy;
the MIDI backend and its initialization policy remain unchanged.

Construction, callback, registration, reported startup and cooperative cancellation
failures attempt to restore the previous device. Failed restoration retains its
existing ownership reservation and reports the need for `audio.device.restore`;
another selection cannot replace that pending recovery. Reentrant and foreign-thread
requests fail. Armed sample/automation clips, note recording and unfinished native
takes must be completed/disarmed before selection.

Runtime selection never falls back to another backend. Settings inside the selected
native backend retain their existing physical-device fallback behavior. The old
device keeps its open handles until commit, so exclusive drivers can refuse a new
open; an explicit selection of Dummy can release those handles before trying the
desired backend again. Driver calls and plugin notifications can block cooperative
cancellation. A reported running state does not establish audible hardware output
or universal asynchronous startup acknowledgment; native backend observations have
the limits described above.

JACK MIDI can retain a raw pointer to the AudioJack connection. Replacing that shared
audio owner is explicitly refused until `midi.backend.select` selects an independent
MIDI backend. Runtime audio selection also uses the
existing engine base rate/buffer size and loaded backend-specific configuration;
it does not provide live editing of those configuration fields or save a new default.
Receipts persist, but the selection itself does not: reopening uses ordinary startup
options. Replaying an old request returns its original observation without opening,
starting or replacing a device. Use a fresh observation/request ID for current state.

`midi.backend.select` requires `name` from `audio.backend.list`. It opens exactly
that native MIDI backend using loaded native settings, with no factory fallback.
Like graph changes, admission requires stopped/disarmed transport, completed takes,
no export/device reservation, and the engine owner outside a model edit. It joins
audio processing before preparing the replacement. Candidate registration and
validation must complete for every existing native MIDI port before any model
changes client. Engine ownership and selected/requested names are then published,
and the old MIDI client is destroyed before returning to the event loop.

Port objects, parameter models, controller/track ownership and requested endpoint
names survive the switch. Current named endpoint inventories are rebuilt; requested
names absent from the replacement stay available for later matching. Raw/named
GUI menus follow the same models. Native client-wide graph edges and retained
reconnect preferences are not copied. Neither project history nor saved startup
configuration is rewritten. Startup failure observations remain startup history.
Receipt replay returns the past result; use a new observation to inspect the current
client, especially after restarting the application.

Queued input and scheduled output are discarded across normal transfer. Old held notes
and pedals are released first; JACK waits for cleanup submission and a later
native callback cycle. Enabled input deliveries require an explicit `midi.input.reset`
or controller equivalent. Intentional transfer has failure code 6 (`ClientTransfer`)
and does not by itself demand a backend-health observation. Existing backend faults
still require their original health checks. For shared JACK, native port readiness
is checked while audio is paused; input recovery additionally requires audio running.
Native subscription methods retain intent without universal connection acknowledgement.
No native graph observation, retirement counter or cleanup submission proves
physical receipt, and raw parser/operating-system queues remain backend-owned.

Preparation or cancellation before commit keeps all original model/client bindings
and discards the candidate; original input may still need reset, and discarded
music is not replayed. Audio is restored to its original running/stopped state.
A failed stop or restart retains an audio reservation for `audio.device.restore`.
A successful selection returns `committed: true`, `ports`, `inputResetsRequired`,
`retirementExceptions`, `notificationFailures`, `previousBackend`,
`outputAbandonment`, `audioRestored`, `recoveryRequired` and `current` transport
observation. MIDI is already committed when audio restarts:
`audioRestored: false` therefore requires `audio.device.restore`, not MIDI rollback.
Late cancellation does not change that committed result. Retirement exceptions count
caught native cleanup errors, not confirmed handle closure or physical delivery.

An optional `abandonFailedOutput: true` explicitly permits replacement without
successful old output cleanup. It is supported only by ALSA raw MIDI and sndio
after a terminal failure has stopped their worker. Inspect `abandonmentSupported`
and `abandonmentEligible` in `transport.inspect.midiBackend.outputBackend` or
`midi.inspect.outputBackend`. Eligibility requires observed inactive output that
cannot resume native submission. Temporary backpressure, pending cleanup, overflow
recovery, running workers and other/unobserved backends refuse the option. It is
never enabled implicitly, and non-boolean values are rejected.

This path still validates every replacement port, preserves models/subscription
intent, performs local input-voice cleanup and applies the normal input-reset and
audio-restoration rules. Old output note/pedal records remain intact through
failed preparation or cancellation. Only a successful commit clears them; they
are not sent to the replacement. `outputAbandonment` reports `abandoned`,
decimal-string `trackedNotes` and `pedalHolds`, old `pendingEvents` and
`cleanupPendingChannels`, current/last native error codes, and
`deviceCleanupVerified: false`. These are local records and queue observations,
not physical note counts. A removed port can leave zero local records while client
cleanup is still pending. `previousBackend` identifies the old backend name, not
a verified physical device. The retired client is joined/closed without replay;
its kernel/device buffers and external held notes may remain unconfirmed. Receipts
retain that uncertainty and replay never repeats the replacement or cleanup.

The old client stays open during candidate preparation. Exclusive native drivers
can reject a second open. JACK sharing one AudioJack connection cannot stage another
shared MIDI client on that connection: select Dummy first to release the old one,
then select JACK. Eligible failed raw clients can also select Dummy with explicit
abandonment before attempting an exclusive reopen. Old JACK output cleanup must
succeed; JACK does not support this abandonment option. Its deadline is cooperative (250 ms),
and native/plugin calls and thread joins can still block.

The transfer/engine fixture has 34 portable cases / 134 scenarios; six JACK transfer
cases cover 14 scenarios. New abandonment coverage includes admission, candidate
failure, cancellation, eligibility loss, local cleanup refusal, final validation,
model retention, audio restart failure, strict argument/default behavior and
historical receipts across restart. JSONL selection/refusal/replay/history and scripted Chat/Responses cases are also
authored. All remain uncompiled/unrun. Real backend replacement, native GUI behavior,
physical MIDI/audio, provider execution and resource/compatibility acceptance remain
unverified.

SDL device cleanup now closes owned handles and balances its audio subsystem
reference instead of shutting down SDL globally, following the
[SDL2 initialization contract](https://wiki.libsdl.org/SDL2/SDL_Init).
This preserves a replacement device and other native SDL users during retirement
or failed construction. Missing capture handles are guarded, and stopped capture
callbacks no longer submit input.

PulseAudio startup now waits for `PA_STREAM_READY` with a ten-second handshake
deadline and cooperative cancellation checks. Context readiness alone cannot
commit `audio.backend.select`. Context/stream connection errors, termination and
mainloop errors publish a numeric native failure and stop rendering; failure no
longer starts an output-free DSP loop. A failure before stream readiness reports
`start`; connection loss after readiness reports `disconnected`. Invalid callback
buffer shapes report `invalidCallback`; caught callback exceptions report
`exception`. The existing device observation exposes the PulseAudio error code.

Stop wakes a blocked PulseAudio poll and joins the worker, including partial
startup, failure cleanup, restart and destruction. A stop check after mainloop
preparation closes its wakeup-clearing race. The selected `audiopa/device` (or
`AUDIODEV` when no setting exists) is passed as the sink; `default` delegates sink
choice to the server. The requested latency is one native period in bytes; actual
server/hardware latency can differ. Each requested chunk is checked for alignment,
size and buffer ownership before native LMMS rendering. Borrowed buffers are
submitted once or cancelled, and callback exceptions stay within the C boundary.
These details follow the [PulseAudio stream contract](https://raw.githubusercontent.com/pulseaudio/pulseaudio/master/src/pulse/stream.h)
and [mainloop implementation](https://raw.githubusercontent.com/pulseaudio/pulseaudio/master/src/pulse/mainloop.c).

`PulseAudioTest`, conditional on PulseAudio being compiled, authors 14 cases / 53
executions. It replaces only transport entry points and exercises the production
worker, lifecycle, native DSP and engine selection. Rows include 17 startup faults,
two cancellation phases, sink/latency settings, delayed readiness, seven runtime
failures, cleanup/restart ordering, stop/destruction, the prepare/poll race, native
sample PCM with short server loans, write failures, empty/unaligned requests,
stop during borrowing and six selection rollbacks. One optional real-server row
requires `LMMS_TEST_PULSEAUDIO_SINK` naming an existing sink (`default` is accepted);
`PULSE_SERVER` can select an isolated server. It checks startup, native clock
advancement, missing-sink rollback and repeated replacement. Without that explicit
sink it reports a skip; an opted-in connection failure fails the test.

All these native tests are authored, uncompiled and unrun. Stream readiness does
not verify audible output, measured latency, physical capture or CPU/RSS limits.
The ten-second deadline applies to the handshake wait; joining cannot interrupt a
blocked driver call or native plugin callback. Other backends still need their own
asynchronous startup and device-loss verification.

ALSA opens its configured PCM in nonblocking mode. Start waits for native PCM
preparation, with a ten-second cooperative deadline. A short write retains the
unwritten suffix of the current LMMS buffer; a retry does not render a replacement
buffer. Interrupted/not-ready writes, underruns and suspend/resume use checked
native recovery. A ten-second interval with no accepted frames reports
`disconnected` / `-ETIMEDOUT`; native errors are otherwise retained. Native waits
and suspend retries use 25-ms intervals, with a short backoff when a plugin reports
readiness without accepting frames. Stop joins the worker and drops queued PCM;
a failed drop reports `stop`, and an explicit restart prepares the PCM again.

ALSA construction checks parameter allocations and hardware negotiation, records
a fallback to the engine base sample rate, and releases partial resources on
failure or cancellation. Only actual returned PCM descriptors receive `FD_CLOEXEC`;
zero descriptors are valid, and adjacent descriptor numbers are never inferred.
This follows the [ALSA PCM transfer and recovery contract](https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html).

`AudioAlsaTest` authors twelve cases / up to 34 rows using a real ALSA `null` PCM
for construction/preparation/drop and controlled transfer fault entry points.
They cover descriptor ownership and cancelled construction; startup errors,
cancellation and deadlines; exact short-write suffixes; underrun/suspend recovery;
fatal and malformed results; no-progress bounds; stop/destruction/restart; drop
failures and late cleanup during restart; native backend rollback; and actual LMMS sample PCM. A separate row uses
the unmodified native I/O entry points for repeated null-backend startup, native
transport advancement and replacement. Seven suspend rows require `ESTRPIPE`, and
the wide-error row skips where ALSA's frame-result type is not wider than `int`.
All rows remain uncompiled/unrun. The null PCM discards audio and supplies no
physical-output or real-time pacing evidence. Preparing a PCM does not prove
sound output; recovery can leave audible gaps and cannot restore already accepted
hardware-buffered samples. A blocked native library/plugin call still delays a
safe join despite these cooperative deadlines.

sndio now configures its native handle without starting playback in the constructor.
It requests nonblocking writes and signed PCM with the native byte order and exact
sample width. Negotiation checks channels, bit/byte width, signedness, endianness,
rate and usable buffer parameters. The existing integer-percent rate tolerance is
preserved using wide arithmetic; accepted rate differences update the AudioDevice
rate. Start waits for successful native preparation with a ten-second cooperative
startup deadline. This is preparation, not confirmation of audible playback.

sndio retains the exact byte suffix after a partial write, including writes ending
within a sample or frame. It processes the library's decoded poll events, handles
interrupted polls, checks EOF/hangup/errors and stops after ten seconds without an
accepted byte. Poll intervals are 25 ms. A ready stream accepting no bytes yields
between retries, and no new native buffer is rendered while one remains pending.
Stop joins the worker before closing or replacing its handle. CMake detects
`sio_flush` for immediate discard; older libraries keep `sio_stop`'s draining
semantics and may take longer to retire.

An explicit restart reopens the same configured device after EOF or a failed stop.
Reopening must retain the current engine rate; a changed rate is refused so normal
`audio.backend.select` can deliver the required engine notifications. Failed reopen
releases partial resources and permits another retry. Late cleanup status is read
after the join and cannot overwrite a fresh start. sndio reports boolean errors,
so LMMS supplies errno-style categories (`-EIO`, `-EINVAL`, `-EOVERFLOW`, `-EDOM`,
`-ETIMEDOUT`, `-ECANCELED`) in `nativeError`; these are not claimed as a libsndio
error number. A failed OS poll retains its actual negative `errno`. The relevant
contracts are [sndio's manual](https://man.openbsd.org/sio_open.3) and
[its handle/start implementation](https://raw.githubusercontent.com/ratchov/sndio/master/libsndio/sio.c).

`AudioSndioTest` authors fourteen cases / 61 executions: format negotiation and
construction cleanup; cancellation; startup errors/deadlines; exact odd-byte
transfers; malformed/fatal write and poll results; no-progress bounds; joined
stop/destruction/restart; EOF reopen and rate-change refusal; failed stop/reopen
recovery; late cleanup; retained engine ownership; and actual native sample PCM
conversion. The private transport table controls libsndio entry points while LMMS
worker/DSP/ownership code remains in use. One optional real-device row requires
`LMMS_TEST_SNDIO_DEVICE` (a native device name, or `default`); it exercises native
transport, repeated restart, failed-selection rollback and replacement with Dummy.
Absent that setting, it skips; opted-in failures fail the test.

All 61 scenarios and the flush feature probe remain uncompiled/unrun here. Neither
controlled transport nor prepared status proves audible hardware output, latency
or real-time performance. Startup and no-progress deadlines cannot interrupt
blocking native open/configuration/start/stop/close calls or plugin code. Shared
JACK lifecycle and runtime MIDI selection still require native execution evidence.

JACK now has an independent atomic callback admission gate. Ordinary device stop
closes that gate and waits for admitted MIDI, DSP and capture work to finish;
the native client stays active and later callbacks clear master output without
advancing the project or reading input. Restart reopens admission after any prior
callback has finished. Destruction joins admitted processing before native
deactivation/close and before freeing callback storage. Keeping the client active
on an ordinary stop preserves its existing graph connections: JACK deactivation
disconnects its ports, as documented in the
[JACK client API](https://jackaudio.org/api/group__ClientFunctions.html).

JACK construction checks both input and output port registration and both callback
registration results. Refusal or an exception closes the partial native client;
callback pointer storage has automatic ownership. Process and buffer-size callback
exceptions are contained at the C boundary. The process callback rejects missing
buffers and a frame count beyond its prepared input capacity, and attempts to
silence current output buffers after an exception. Driver diagnostics go to stderr
so they cannot become JSONL responses. The native sample/DSP engine remains the
source of audio, and capture still enters its existing input/recording path.

`AudioJackTest` authors eight cases / 31 scenarios using private native transport
entry points with the production callback and engine. They cover thirteen
construction outcomes, early/startup callbacks, stopped callbacks at three frame
sizes, six malformed/exception paths, stop and destruction during native DSP,
signed planar sample playback and recording, and two activation rollback states.
One optional real-server row requires `LMMS_TEST_JACK=1` and checks transport,
stop/restart and replacement with Dummy. All rows remain uncompiled/unrun.

JACK shutdown now publishes lock-free flags and closes callback admission. It
retains the old handle until owner-thread cleanup has joined callbacks and closed
the client. Qt notifications and device failure publication run on the owner
thread, through a nominal 100 ms health poll. Shared MIDI has a separate reader
gate: its port is registered before publication, detach joins its MIDI readers,
and retiring audio invalidates a surviving MIDI owner. Native port retirement
can still block inside JACK.
This follows the signal-safe shutdown requirements in the
[JACK callback API](https://jackaudio.org/api/group__ClientCallbacks.html).

Automatic owner-thread recovery retries an existing server with at least one second between
attempt starts. It preserves the original client name, refuses a changed sample
rate, recreates the audio and shared MIDI ports, and leaves failures retryable.
A stopped device stays stopped; an export reservation prevents a timer from
starting another DSP clock. A MIDI-only failure can recreate its ports while
audio remains active. Standalone JACK MIDI uses checked native availability;
the former sleeping placeholder thread and forced termination are removed.

MIDI input checks loss, event access, timestamps, status/data bytes and packet
lengths before publishing any channel-message prefix. A fixed scratch array
bounds each block to 1024 events. Valid packets enter the existing native raw
parser and owner-thread MIDI delivery queue. This uses JACK's documented
[normalized packet format](https://jackaudio.org/api/group__MIDIAPI.html).
Backend recovery leaves per-port delivery failures latched until an explicit
`midi.input.reset`. Callback storage is copied before delivery; frame offsets are
validated but the raw queued path does not provide sample-accurate timing.

`JackLifetimeTest` has fourteen lifetime cases / 51 scenarios for partial standalone
construction, shared publication/detach, queued native events, malformed blocks,
reader and DSP lifetime, failed/retried reconnect, rate refusal, late shutdown
notifications, stopped/reserved state, MIDI-only repair and both destruction
orders. Two optional rows use `LMMS_TEST_JACK=1` for real shared port retirement.
The output-port construction and reconnect rows extend the original 47 scenarios.
These and the prior 31 `AudioJackTest` scenarios remain uncompiled/unrun.

JACK MIDI output now has a native `MIDI out` port and a bounded queue of 1024
complete channel messages. Notes, key/channel pressure, controllers, program
changes and pitch bend use the existing native encoder. Outgoing whole-tick
delays are converted to native sample frames using the tempo and sample rate at
enqueue; later tempo changes do not retime queued messages. Delays exceeding
`INT32_MAX` native frames are refused. Delivery sorts by native frame time and
enqueue order, handles frame-counter wrap, and clamps overdue events to the
start of the next available cycle. These clocks follow the
[JACK timing API](https://jackaudio.org/api/group__TimeFunctions.html).

The process callback clears the native output buffer every cycle, including
stopped playback, and uses a nonblocking queue-lock attempt. Contention defers
delivery. Shared output is submitted after native DSP; failed or stopped audio
discards its pending musical events and drains note cleanup. Stop also clears the
schedule before returning and changes its generation, so an immediate restart
cannot publish events from a producer admitted before the stop. Native write
errors discard the current normal-message prefix and pending schedule. Cleanup sends
sustain-off, sostenuto-off, all-notes-off and all-sound-off, retaining progress
across small native buffers. Retargeting/removing a native `MidiPort` cancels its
queued events; retired addresses cannot replay their previous owner's schedule.
Failed native port unregistration retires the owning JACK client before recovery.

`midi.inspect.outputBackend` exposes observed/active state, pending event count,
queue limit, channels awaiting cleanup, current native error and last native
error. This describes submission to JACK; it does not acknowledge external
device receipt. Backend delivery can resume after cleanup succeeds, while the
last error remains inspectable. Ordinary MIDI-port routing and note tracking
continue to use LMMS's native models.

Eighteen output cases / 45 scenarios supplement fourteen lifetime cases /
51 scenarios in `JackLifetimeTest`. They cover native packet bytes, model routing, ordering/clock wrap,
delay bounds, cancellation, retired-address reuse, overflow, native errors,
partial cleanup, contention, stop/export/immediate restart, reconnect, registration/unregistration
failures, concurrent producers and held output callbacks. One additional optional
`LMMS_TEST_JACK=1` case connects a real receiving JACK client and checks note and
cleanup packets. All remain uncompiled/unrun.

External MIDI/graph connections are not restored automatically after server loss.
Client-wide native MIDI graph routing is available through the operations below;
the raw backend still has no independent per-track destination subscriptions.
System/SysEx input and output remain unsupported.
Cleanup affects a whole external channel and can interrupt other sources sharing
that channel. Server loss or native port retirement can prevent cleanup from
reaching a receiver. Native close failure behavior remains unverified. The legacy conditional
external bus path limits its copy to the native bus buffer and clears its tail,
but its routing and map synchronization still require repair and execution.
Native JACK/weakjack builds, real device capture, GUI coexistence, runtime MIDI
selection execution, latency and CPU/RSS measurements remain outstanding. Health polling
can be delayed by the event loop, and joins cannot interrupt blocked native or
plugin calls. An active connection does not establish physical sound or delivery.

`audio.backend.inspect` reads the current audio device's native master graph.
For JACK, `direction: "input"` lists external audio sources and `"output"`
lists destinations. `channels` contains zero-based `index`, exact `localPort`
and the loaded `startupPeer` setting. `startupPeerTruncated: true` marks a
setting preview limited to 1024 UTF-16 units; that preview is not an exact name.
Each `ports` entry contains an exact `name` and its `connectedChannels` array.
`scope: "master"` distinguishes these device channels from project mixer sends.
`offset`/`limit` page sorted external endpoints (default 128, maximum 1024),
with `total`, `nextOffset` and `hasMore`. Discovery rejects more than 1024 native
names or 256 KiB of name bytes, invalid UTF-8, and names at least 4096 bytes long.
Endpoint result arrays have a 512 KiB budget; JACK allocates the native name
array before application checks, and LMMS releases it on success or failure.

Other audio backends currently report `supported: false`, `observed: false`,
and empty channel/endpoint arrays. Offline JACK is supported but unobserved.
`connectionVerified` refers only to that graph observation;
`deviceReceiptVerified` remains false. External graph changes can invalidate a
snapshot immediately. Use actual native capture or external monitoring to
establish audio delivery; a connection or running flag is insufficient.

`audio.backend.route` takes `direction`, a zero-based native `channel`, an exact
external `port`, and boolean `enabled`. JACK validates the peer's audio type,
direction and external ownership. Only the requested edge changes, and an
already matching state returns `changed: false`. New changes require the current
device, stopped/disarmed transport, completed takes, no export/device reservation
and no active render scope. Native DSP/capture callbacks are paused and joined
without holding the audio model lock. Previously running processing resumes
after a verified result or verified rollback; a previously stopped device stays
stopped. `processingInterrupted` reports whether a running callback path was
paused. During that pause, callbacks clear master output and may cancel scheduled
shared JACK MIDI output and submit its cleanup. Routing is not a seamless switch.

Input change attempts invalidate queued native input before touching the graph,
including attempts later cancelled or rolled back. Successful changes report
`inputBuffersInvalidated: true`. Native errors, cancellation after mutation or
failed observation attempt to restore and verify the previous edge. If that
restoration cannot be confirmed, processing stays stopped and automatic server
recovery is disarmed. Inspect the graph and `transport.inspect` before explicitly
restarting. Joins and native calls cannot forcibly interrupt a blocked plugin or
server call; cancellation is cooperative.

These operations do not create project revisions, save routes in project files,
or edit loaded startup settings. Native client restart can reconnect configured
`startupPeer` destinations or sources, even after an explicit live disconnect.
Manually added edges are not automatically retained. Receipt replay returns the
historical result without changing the current graph. Startup settings use UTF-8
names. Track outputs use the separate `audio.bus.*` operations below.

```json
{"id":"audio-sinks","op":"audio.backend.inspect","args":{"direction":"output","limit":32}}
{"id":"audio-right","op":"audio.backend.route","args":{"direction":"output","channel":1,"port":"interface:playback_2","enabled":true}}
{"id":"audio-source","op":"audio.backend.route","args":{"direction":"input","channel":0,"port":"interface:capture_1","enabled":true}}
```

Eighteen master-audio graph cases / 66 scenarios are authored in
`JackLifetimeTest`: direction/type filtering, paging/bounds, native errors,
channel isolation, input invalidation, processing restoration, cancellation,
held callback joining, shared MIDI cleanup, twelve native admission refusals,
startup/replay behavior, device-bound queued selections, scripted Chat/Responses execution and native sample
playback/capture. `LMMS_TEST_JACK=1` also enables an optional real-server case
connecting independent JACK source/sink clients through these operations,
checking native rendered PCM and captured sample data. The JSONL harness checks
unsupported backend results and receipt replay. All native cases and process
execution remain uncompiled/unrun.

The contracts use the [JACK port API](https://jackaudio.org/api/group__PortFunctions.html)
and [callback API](https://jackaudio.org/api/group__ClientCallbacks.html).

`audio.bus.inspect` takes a native instrument/sample `track` index, optional
`container` (`song` by default, or `pattern`), and endpoint `offset`/`limit`.
It returns `scope: "track"`, the same channel/endpoint inventory as master
inspection, and `busId`, `label`, `labelTruncated`, `enabled`, `published`,
`pending`, `portErrorCode` and `portError`. The label preview is limited to 1024
UTF-16 code units. Native names are stable numeric `track <busId> L/R` names
within the owning JACK device, so renaming a track preserves existing edges.
Duplicate and long track names do not collide. Preview/internal buses have no
native ports. Unsupported devices return `supported: false`, `observed: false`
and no published channels; mutations fail explicitly.

These stereo ports carry the native track signal after gain, pan and track
effects, before mixer sends, mixer effects and master gain. The engine copies
master and bus destinations with the same delayed period cursor, including
partial periods and callbacks spanning several periods. Muted, render-excluded,
disabled, removed or failed sources are silent. This is independent track
output; native mixer routing still controls the master mix.

`audio.bus.configure` takes `track`, optional `container`, and `enabled`.
`audio.bus.route` additionally takes `channel` (0 left, 1 right) and an exact
external audio destination `port`. Configuration synchronizes the requested
port state; routing requires an already published, enabled stereo pair. Both
require the current device, owner thread, stopped/disarmed transport, completed
takes, no export/render/device reservation and no enclosing native model edit.
Routing uses the same verified edge change, inverse rollback, shared MIDI
cleanup and processing-restoration contract as master output routing.

Track construction and model changes stage intent without calling JACK or
joining callbacks under the model lock. Enabled project buses are prepared
before initial native activation and client recreation. Healthy owner polling
otherwise defers native publication/removal until stopped/disarmed admission,
attempts at most eight due buses per tick, and spaces retries for each bus by at
least one second. Explicit configuration retries immediately. At most 256 stereo
track buses (512 ports, in addition to master/MIDI ports) can be published per
device. Further tracks remain editable with pending output requests.

`enabled` is requested state, not evidence that ports exist. Inspect `published`,
`pending`, `observed` and both error fields after a failure. A failed update can
retain the new requested state. Disabling first silences the source; retirement
of an active client then waits up to 250 ms for a callback that clears the
buffers, disconnects and verifies all edges, joins readers of the old snapshot,
and unregisters the native handles. A stalled callback or disconnect failure
leaves owned silent ports pending. This timeout measures native buffer progress,
not receiver or physical-device acknowledgement. Cancellation before disconnect
starts restores the previous enable request and leaves existing edges intact;
after disconnect starts, handle retirement finishes. Registration failures
clean up provisional ports. Uncertain unregister ownership closes the complete
JACK client, including shared MIDI, and disarms automatic restart until an
explicit restart. Snapshot joins and native calls cannot interrupt blocked DSP
or library calls; the 250 ms limit does not bound those operations.

Bus IDs and enable requests last for the bus/device lifetime. Client recreation
rebuilds enabled ports with their IDs but loses track graph connections. Project
files, revisions, undo and historical receipt replay do not save or restore
these process-local requests or routes. Inspection is a current graph snapshot
that can race changes made by other JACK clients. `connectionVerified` verifies
that observation; `deviceReceiptVerified` remains false.

```json
{"id":"track-ports","op":"audio.bus.configure","args":{"track":0,"enabled":true}}
{"id":"track-sinks","op":"audio.bus.inspect","args":{"track":0,"limit":32}}
{"id":"track-right","op":"audio.bus.route","args":{"track":0,"channel":1,"port":"interface:playback_2","enabled":true}}
{"id":"track-retire","op":"audio.bus.configure","args":{"track":0,"enabled":false}}
```

Nineteen track-port cases / 70 scenarios are authored in `JackLifetimeTest` for
period alignment, changing native buffer addresses, label/identity stability,
model-lock staging, held-reader retirement and address reuse, partial resource
failure, cancellation, native admission, output faults, restart, port limits,
selection binding, receipt/history behavior, invalid targets and scripted
Chat/Responses execution. An optional `LMMS_TEST_JACK=1` case connects an
independent JACK sink to an actual sample track with master volume zero.
The combined fixture has 101 cases / 339 scenarios. The JSONL harness also checks
unsupported bus observations, mutation refusal and receipt replay. The retained
engine buffer foundation has another 17 cases / 68 scenarios. All C++ fixtures,
real-server cases and process executions remain uncompiled/unrun; source
diagnostics alone do not prove native audio or full DAW completion.

`midi.backend.inspect` reads the selected MIDI client's native graph. Supply
`direction: "input"` for external sources or `"output"` for destinations.
`offset`/`limit` page sorted endpoint names (default 128, maximum 1024), with
`total`, `nextOffset` and `hasMore`. `localPort` names LMMS's endpoint; each `ports`
entry contains `name`, `available`, `connected`, `reconnect`, `pending` and a
`nativeErrorCode` for the last failed recovery attempt. Missing retained names
stay in the inventory with `available: false` and `pending: true`. When the
backend is offline, `observed: false` means availability and connection state
are unknown; these entries describe preferences, not live observations.
`reconnectSupported` indicates policy support, and `reconnectDeferred` indicates
that retained routes cannot currently pass owner/transport/backend admission.
`scope: "client"` means these connections
affect all native MIDI models sharing that client. JACK supports this interface;
other backends currently report `supported: false` and `observed: false`.
`connectionVerified` refers to the observed graph, while `deviceReceiptVerified`
remains false. A fresh observation is required after external graph changes.

`midi.backend.route` takes `direction`, an exact external `port`, and `enabled`.
It connects or disconnects that single edge, checks native type/direction and
rejects LMMS-owned peers. It requires stopped/disarmed transport, completed takes,
and no export or device transition. The operation runs on the owner thread and
joins readers or drains output outside the audio model lock. An already matching
connection returns `changed: false`. A successful disconnect also forgets its
reconnect preference, reported by `reconnectChanged`; `reconnect` reports the
remaining preference. A failed disconnect preserves the preference.
Success includes the verified connection
state; it does not confirm that a device received notes or produced sound.

Before output changes, queued musical events are discarded and channel cleanup
must be submitted with a subsequent JACK callback observed. A cooperative 250 ms
drain deadline preserves the old graph if cleanup cannot finish; large periods or
stalled callbacks can cause this refusal. Native calls themselves are not bounded
by that deadline. Input changes join readers, reset the raw parser and latch
failure on affected readable deliveries, including an attempt later rolled back.
Use `midi.input.reset` for those track/controller ports after routing succeeds or
the old connection has been restored. Successful input changes return
`inputDeliveryResetRequired: true`.

Native errors, exceptions, failed verification or cancellation after a change
trigger restoration of the previous edge. Failure explicitly distinguishes a
verified restoration from unconfirmed native state. Cleanup can release notes
even when the graph change fails. Routes are process-local external state: they
do not create project revisions or persist in project files. Reconnection is
opt-in, through the separate preference operation below. Receipt replay returns
historical results without changing connections or recreating preferences.

`midi.backend.reconnect` takes `direction`, an exact external `port`, and
`enabled`. Creating a preference with `enabled: true` requires a connected,
available endpoint and stopped/disarmed transport. It retains that exact name
for future recovery without changing the current graph. Repeating an existing
preference is idempotent, including while offline. `enabled: false` forgets the
preference even during playback or an outage; it leaves any current connection
alone. Results include `reconnect`, `changed` and `graphChanged: false`. Use
`midi.backend.inspect` for a fresh connection observation. Unsupported backends
refuse the preference operation explicitly.

The native owner timer attempts at most one due retained route per 100 ms tick,
with at least one second between attempts for the same route. It rotates through
preferences so a missing endpoint does not prevent others from recovering.
Playback, pause, recording/armed clips/reserved takes, exports, device changes,
render scopes, or a different current backend defer graph recovery. Retry
timing is cooperative: a busy owner, reader joins or blocking native calls can
delay it. Discovery does not trigger recovery. Preferences survive native JACK
client/port recreation within this process, but not backend-object destruction
or application restart. Project load neither saves nor recreates them, and does
not clear existing process-local preferences. Names can identify replacement
devices; they do not establish durable physical identity. Disconnect externally
without forgetting a retained preference and the timer will try to restore it.

Recovered input routes keep delivery latched until explicit `midi.input.reset`.
Output recovery first drains cleanup on the old graph, then queues sustain,
sostenuto, all-notes-off and all-sound-off releases on all sixteen channels after
connecting. This cleanup also reaches other destinations sharing the raw output.
New musical output is refused until these releases are submitted; partial native
buffers retain progress. `midi.inspect.outputBackend` exposes pending cleanup and
activity. Graph connection success alone does not prove that cleanup reached a
device, and output recovery does not replay old scheduled notes.

```json
{"id":"midi-sinks","op":"midi.backend.inspect","args":{"direction":"output","limit":32}}
{"id":"midi-connect","op":"midi.backend.route","args":{"direction":"output","port":"synth:input","enabled":true}}
{"id":"midi-retain","op":"midi.backend.reconnect","args":{"direction":"output","port":"synth:input","enabled":true}}
{"id":"midi-forget","op":"midi.backend.reconnect","args":{"direction":"output","port":"synth:input","enabled":false}}
{"id":"midi-disconnect","op":"midi.backend.route","args":{"direction":"output","port":"synth:input","enabled":false}}
```

Discovery refuses inventories above 1024 returned native names or 256 KiB of
name bytes, and rejects invalid UTF-8 or names of 4096 bytes or more. Pages also
have a 512 KiB response budget. JACK allocates its native name array before these
application checks; it is released through `jack_free`, including failure paths.
Reconnect preferences have separate limits of 64 routes and 64 KiB of UTF-8
endpoint names across both directions. Missing retained names can add up to 64
entries to the native discovery inventory; the same page limits still apply.
These rules follow the [JACK port API](https://jackaudio.org/api/group__PortFunctions.html)
and [lookup API](https://jackaudio.org/api/group__PortSearching.html).

Twenty-six MIDI graph cases / 93 scenarios combine with the lifetime, MIDI output,
master-audio, track-port and client-transfer fixtures in `JackLifetimeTest`:
101 cases / 339 scenarios.
MIDI coverage includes native discovery, paging, validation, rollback,
input reset, output drain/cancellation, foreign/reentrant refusal, studio guards,
durable receipt replay, and scripted Chat/Responses agent execution. Eleven
reconnect cases / 46 scenarios cover retention bounds, shared/standalone restart,
missing peers, admission deferral and resumption, retry failures/fairness,
input reset, cleanup before and after reconnect, small native output buffers,
forgetting during outages/playback, and preference replay. The existing
optional real receiver now connects, observes and disconnects through the studio
API. The process harness also checks unsupported-backend observation, refusal,
replay and unchanged project history. All native scenarios and the process
harness remain uncompiled/unrun. The catalog now has 162 native operations and
keeps the existing 128-declaration inference limit.

`StudioBackendSelectionTest` authors nine cases / 47 executions: real native Dummy
selection/playback, twelve failure phases in two prior running states, three retained
rollback failures, eleven refused admissions, nested callbacks, native DSP joining,
rendered PCM/history/reopen, two scripted provider protocols and optional SDL
dummy-driver lifetime/playback. The executable harness adds runtime selection, busy
refusal and two receipt replay cases including process restart. SDL can skip when
that backend or its dummy driver is unavailable. All remain uncompiled/unrun;
lifecycle fixtures and scripted providers do not prove hardware or musical judgment.

`clip.edit` also accepts signed `offset` ticks and `autoResize`. A positive offset
delays the source within the clip; a negative offset consumes source content from
its beginning. Manual length or offset edits disable automatic resizing. Enable
automatic resizing in a separate request. `clip.split` takes a position relative
to the clip start and follows native non-destructive knife behavior: the right
clip shares the sample buffer or clones native note data, with an independent
playback offset. Zero-length boundaries and PatternStore splits are rejected.

`controller.list` discovers native rack controllers and their parameter handles.
`controller.add` creates an LFO using `type: "lfo"`; `controller.edit` renames it.
`controller.connect` attaches or rewires a parameter, including a controller's
own parameters, subject to native dependency and linked-model cycle checks.
`controller.disconnect` restores the parameter's stored manual value.
`controller.remove` removes rack LFOs and their connections. Peak controllers are
created and removed with their owning native effects and can be connected after
discovery. `controller.midi.connect` creates a connection-owned MIDI CC controller
for a parameter; it has no rack index. `controller.midi.learn` detects and binds
the first matching CC through an active native input. Unsupported native parameter scales are
rejected before DSP. MIDI connection and port options are described below.
`parameter.list` reports the effective and stored manual values, scale, owning
track/controller or project scope, and any controller connection. Rediscover indices and
parameter IDs after restoration.

Native linked controls share a raw-value ring. `parameter.list` reports
`linkedParametersPresent`; `parameter.inspect` returns `linkedParameterCount`,
`linkedParameters` and `linkedPage`. Use `linkedOffset` / `linkedLimit` (default
128, maximum 1024), following nested `nextOffset` / `hasMore`. Peer arrays have a
512-KiB byte budget and exclude the selected model. Each peer reports `id`,
`scale`, `writable`, `automatable` and `inspectable`. Inspect a peer's handle
separately for its full location/control metadata only when `inspectable` is
true; live non-project models can be linked without being project API handles.
Rediscover after group changes or restoration. Peer paging scans the native ring
and current project discovery; bounded payload does not imply constant work or
bound the selected parameter's existing name/path metadata.

`linkedPersistence: "checked-native-project"` identifies checked native graph
persistence for whole projects and closed track/clip settings. Native saves assign document-local `native_link_ref` values to
serialized linked models and write an ordered `nativeLinkedControls` graph,
version 1. Every member must serialize; missing/unsupported members, changed
groups or unrecognized markers refuse publication and preserve the existing
destination. This is separate from journal IDs and preserves the ring's native
controller precedence and each member's stored value.

Whole-project load, StudioRuntime checkpoints, undo/redo and reopening use the
scoped reader. It validates actual restored model references and complete groups
before installing links, rejects unsupported versions/duplicates/dangling or
unclaimed members, and rolls back ring installation if controller feedback would
result. Structured incomplete loads retain their existing native checkpoint
rollback. Clearing a project unlinks reused header/mixer models too. Subsequent
saves replace earlier graph metadata rather than copying stale definitions.

Track presets and track clone settings also use scoped references. Portable
settings must include every group member; cross-track/rack/mixer links and omitted
controls refuse preset publication or cloning rather than silently losing links.
Track/column cloning preflights each copied clip control document before creating
a replacement, then copies each closed clip group separately. Omitted or
cross-boundary clip members refuse cloning. Successful
settings groups attach to copied receivers, with independent native identities.

Modern presets write an explicit graph even when empty. Loading one removes prior
internal settings groups before restoring its declared groups; clip-owned groups
remain intact. Legacy presets without markers/graph keep prior native behavior.
Instrument replacement preserves destination mute/solo, mixer channel and track
MIDI port; removing that port does not remove an embedded MIDI controller's port.
Shared non-MIDI controller bindings retain native rack ordinal semantics and are
not made portable outside that project. Direct native/GUI load errors can leave
partial settings changes with native diagnostics; structured loads use the
existing checkpoint rollback. Native plugin/legacy execution remains unverified.

All four tracked clip types now serialize native mute/solo model controls,
including stored values and controller connections, instead of only a evaluated
mute scalar. Scalar attributes remain for older readers, and control/connection
nodes carry the native metadata flag so older MIDI-clip readers skip them rather
than treating them as notes. Solo is owned by its clip and participates in native
control discovery. Complete projects can restore cross-clip/track groups.

Tracked native `clone`, `copyTo` and `copyStateTo` entries check complete clip
control groups. Direct copies retain resident audio and native note/curve data
through the existing constructors, then restore a small native control document
onto the copied receivers. Copies keep independent IDs and stored values; source
rings remain independent. Missing or external members refuse copying before new
clip construction or old pattern-cell retirement. Native XML state copy preserves
destination identity/position and clears replaced control links before values
load; its existing asset reload behavior remains. A failed later state load can
leave partial native changes; structured copies retain checkpoint rollback.

Untracked inline curves used by note detuning retain their native constructor/DSP
path. Whole-project and whole-clip inline graphs now use the scoped metadata
described below. Standalone Note clones, other duplication gestures and plugin-only
copies remain outside portable graph scopes. Native value/topology journals have
their own in-memory formats. Native clipboard/drag selection transfers share one checked graph
across every selected clip. Missing or external members refuse publication;
groups spanning selected clips restore onto the imported receivers. The GUI uses
this same core API for copy, cut, drag and paste. A failed copy leaves the current
clipboard and cut originals intact. Paste publishes caller results after group
validation, keeps fresh identities and removes prepared clips on failure,
including a clip attached by a factory before throwing. Loading suppresses
automatic journal entries and restores the journal flag through cleanup. Existing
GUI per-object undo still lacks linked topology; the structured runtime keeps
its separate whole-project checkpoints. Native layout/type/position checks run
again at paste, including checked metadata and wide offset arithmetic.

The core transfer retains existing native XML asset reload, automation-target ID
resolution and legacy PatternStore append behavior; it does not remap arbitrary
automation targets or make shared rack bindings portable across projects. Dummy
or unregistered controller bindings refuse transfer. Prepared-clip cleanup does
not restore every playback/plugin/global callback side effect. Headless native
transfer fixtures are authored but unrun; GUI/OS clipboard/drag, plugin, legacy
and actual render compatibility remain unverified. Serializers that omit linked members are unsupported. Structured `parameter.link` / `parameter.unlink` now use native rings and the
complete project checkpoint path; serializer coverage remains required. Successful live inspection or an old receipt does not
prove serialization coverage or cross-version compatibility. Older LMMS versions
do not restore this new linked graph extension.

`parameter.link` requires two distinct current handles: `parameter` and `peer`.
It joins their existing native rings. The selected parameter and its old peers
copy the peer's stored raw value, each fitted to its own range/step; this is not
normalized mapping between different ranges. Existing direct-controller and ring
precedence remain native. Every merged member must be currently discoverable,
writable, support native automation/controller input, have a linear/log scale
and a finite stored value. One discovery pass resolves both handles, and weak
member provenance prevents callback address reuse from masquerading as a prior
receiver. Admission is rechecked after native callbacks. Controller feedback
refuses joining through the existing native dependency checks.

`parameter.unlink` requires `parameter` and detaches that writable control from
its ring. Other peers stay linked; stored values and controller bindings remain.
It can detach a writable discovered control from a ring containing external
members in a direct native call. Unsupported existing groups can still prevent
StudioRuntime from capturing its required original checkpoint. Native recovery
and per-object journal topology remain separate limitations.

```json
{"id":"link-controls","op":"parameter.link","args":{"parameter":1234,"peer":5678}}
{"id":"detach-control","op":"parameter.unlink","args":{"parameter":1234}}
```

Receipts report `parameter`, `linked`, `changed` and `manualValue`; link also
reports `peer` and `linkedPeers`. Already joined/detached calls report
`changed:false`. Use current handles from parameter inspection; rediscover after
project/history restoration. Stored receipts describe earlier execution and do
not rebind obsolete handles. `parameter.set` refuses writes when any linked peer
is disabled or an output, preserving that peer's native input contract.

Both operations use native complete-project transaction checkpoints, undo/redo,
reopening and receipt replay in StudioRuntime. Missing serializer coverage can
refuse publication even after successful live admission. Runtime rejection
restores the prior checkpoint; direct native calls can retain earlier copied
values, rings, controller or callback side effects. Every receiver has its own
queued weak identity, including `peer`. Both tool names are retained within the
128-function declaration budget and remain available through `studio_execute`.
Native user-facing value and topology journal paths are described below.
Standalone-note and plugin-only linked copies remain incomplete.
Authored native fixtures are uncompiled/unrun; no compiler/Qt/GUI/plugin/legacy,
audio, provider musical judgment or CPU/RSS acceptance is established.

Native per-object value undo now captures an entire linked ring instead of only
the initiating scalar. A saved value of 60 on a narrower receiver can coexist
with peers at 70 and 90; undo preserves those three distinct stored values.
The checkpoint records ordered full native IDs, nonreusing model lifetime stamps,
precise float values and the current range, step, scale and strict-step policy.
Restoration refuses a changed group/order/lifetime or fitting configuration before
changing any values. A scalar checkpoint made before joining a group also refuses
restoration while linked, avoiding unintended propagation into its new peers.

The complete raw-value vector is published before changed-peer notifications;
native buffers are invalidated without resetting musical clocks. Returned
callback failures, receiver retirement or detected value/group mutation restore
the surviving raw values and interpolation flags. Native ProjectJournal keeps its
existing history stacks on refusal or exception. This does not undo notifications,
derived/plugin/external callback effects, callback topology/fitting changes or
retirement, and rollback emits no compensating notifications. Native ownership
and synchronization remain required. Specialized mixer solo journals retain their
own compound behavior; unlinked models retain their existing serialization path.

These are in-memory value checkpoints. User-facing link/unlink edits use the
separate topology checkpoint below; durable session history remains separate.
Value checkpoints preserve current controller configuration, automation,
initial values and tempo modes rather than serializing those as linked value
edits. The lifetime stamp changes native model layout and construction cost;
rebuild/ABI checks are required, and exhaustion refuses construction at UINT64_MAX.
Snapshot XML and traversal use memory proportional to group size, and topology
activity during notification can require another scan. LinkedValueJournalTest
authors thirteen cases / 35 scenarios, all uncompiled/unrun. Native timing,
CPU/RSS, GUI/legacy/plugins, PCM/hardware and provider musical acceptance remain
unverified; standalone-note/plugin-only linked copies remain incomplete. StudioRuntime
continues to use its separate full-project history.

User-facing link/unlink edits now use NativeControlLinkJournal from both structured
parameter operations and GUI drop/unlink actions. A successful edit with native
journalling enabled records one prepared checkpoint for the complete affected
model set, including the two old groups when merging. Internal plugin binding,
destruction and native XML restoration keep their existing primitive paths. An
idempotent link/unlink or disabled global/receiver journalling adds no checkpoint.

The checkpoint stores desired and expected edge permutations, native IDs/lifetime
stamps, precise raw values and fitting signatures. Undo/redo resolves the same
objects and checks the expected current topology before applying the desired
partition. Its counterpart includes detached peers as well as the initiating
model, allowing redo after undo splits a group. Native controller feedback is
checked before restored values are published and again after their notifications.
Topology and value edits share the native history stack. No project graph format
or structured operation count changes.

Restoration prepares weak state before mutation, suspends event dispatch and
journalling, installs complete native rings and values before changed-peer
notifications, and invalidates group buffers without resetting musical clocks.
Returned notification failure or detected mutation restores surviving prior raw
values/interpolation and reconstructs prior rings when controller bindings remain
unchanged. If a binding changed, affected models remain detached to avoid restoring
a feedback ring. Retired objects are not recreated. Native history stacks retain
the refused entry; subsequent restoration can still refuse until state is repaired.
Callback, derived, plugin/external effects and changed bindings are not reversed,
and rollback sends no compensating notifications. Arbitrary parent/plugin deletion,
concurrent mutation and throwing cleanup remain outside these guards.

Failed initial edits or failed checkpoint publication can retain direct native
effects without a checkpoint; the structured runtime retains its complete-project
recovery path. Compound setter callbacks run with native journalling suspended,
so unrelated nested callback edits do not gain independent history entries. Native
controller dependency checks and repeated validation can be quadratic and allocate
complete resident XML/weak state; no hard deadline or CPU/RSS bound is established.
ControlLinkJournalTest authors thirteen cases / 29 scenarios, including interleaved
history, feedback rejection, retirement/binding rollback and 1050 peers. All remain
uncompiled/unrun, including GUI drop/unlink and actual plugin/legacy/audio behavior.

The shared graph transition now captures each affected old ring once, plans
remaining-peer links from immutable old edges, and installs proposed groups
without repeated predecessor searches. Unselected peers keep their order and raw
values. A cooperative check precedes mutation; weak receiver/next identities and
current edges are rechecked afterward. Controller bindings are collected after
that check and deduplicated within each proposed group before native feedback
validation. Feedback rejection restores the complete old edges, including peers
outside the proposed groups. Distinct saved references that alias one receiver
are refused before mutation.

Edge capture, planning and installation have expected linear work under hash-table
assumptions and retain memory proportional to affected controls/bindings. This is
a source-level complexity assessment, not measured native performance. Controller
dependency traversal, model loading/unlinking, constructor linking and journal
notification validation can still dominate or remain quadratic. Native owner
synchronization and valid preexisting rings remain required; weak admission guards
do not pin objects or support concurrent mutation/arbitrary callee deletion.
NativeGraphTransitionTest authors seven cases / 15 scenarios, including remaining
peers, late admission changes, feedback rollback, shared bindings and 1050/4096
controls. All are uncompiled/unrun; native CPU/RSS, latency, Qt/ABI, GUI/plugins,
PCM/hardware and full autonomous production acceptance remain unverified.

Per-note detuning helpers and existing untracked inline curves now participate
in project/checkpoint and whole-clip model discovery without creating lazy curves.
Versioned `inlinecontrol` metadata saves the helper's stored value, scale and
controller connection. Existing curves save their native points, tangents and
mute/solo controls even when empty/default; zero-length notes retain this state
too. Legacy curves still load through their native node layout. Older readers
ignore the new scalar/control metadata and cannot reproduce its new graph state.

Whole-clip clone/copy, XML state copy, direct selections and clipboard/drag transfer
retain closed inline groups, including links across copied notes or selected
clips. Native constructor copies retain points/tangents; separate control snapshots
validate note count, unique index and key/position/length before applying per-note
metadata. Full-project graphs can include outside owners; partial copies refuse
missing members. A linked detuning helper shared by multiple editable notes refuses
publication because native loading creates distinct receivers. Unlinked helper
sharing retains the existing by-value save behavior. Restored inline curves bind
only to their actual helper and clear serialized target IDs, preserving an empty
curve without inserting a point.

`note.detuning.inspect` supplies the current helper `parameter` handle and existing
`curveControls` mute/solo handles. Generic parameter discovery includes them and
reports container/track/clip/note locations. Inspection keeps lazy curves uncreated.
Ordinary shallow playback Note copies still share detuning and do not allocate
XML; deep-copy constructors now use the stored scalar rather than baking a live
controller value. Their standalone graph copying and plugin-only copying remain
outside these portable scopes; selected-note transfer has its own path below. Direct load failures
can leave partial native changes; StudioRuntime retains whole-project recovery.

LinkedInlinePersistenceTest authors fourteen cases / 25 scenarios, all
uncompiled/unrun. File size can grow from retained default/empty curve and control
metadata. Discovery/location traverses notes and constructs model lists; graph
and roster XML remain resident and native work has no hard deadline. Native
C++/Qt/MOC/ABI, DSP/GUI/legacy/plugins, PCM/hardware, CPU/RSS/latency and real
provider musical/full-system acceptance remain unverified.

NativeNoteTransfer now provides headless C++ save/load/copy for selected notes
from one live MIDI clip to one destination. It captures the complete closed
inline-control graph over the selection; omitted linked peers refuse copying.
Direct copy uses native Note constructors for resident points/tangents and XML
only for controls. Clipboard loading uses native note XML, including legacy lists
and keyboard fields, with checked key/volume/pan/type/time/endpoint admission.
Unresolved rack references and embedded non-MIDI controller factories refuse
loading before native object construction. Shared rack bindings retain their
current native ordinal meaning.

The core prepares owned notes and restores their graph before one sorted insertion
into the destination. It transfers ownership without another Note clone, preserving
links between the new notes. Source rosters are snapshotted, note lifetime tokens
are rechecked at cooperative boundaries, and destination/source clips use weak
identity guards. Native owner-thread synchronization, idle note recording and
ordinary callback contracts remain required. Full rosters/XML stay resident;
membership checks can be quadratic and allocation/sorting/native calls have no
hard deadline or measured CPU/RSS bound.

Piano-roll copy/cut/paste uses this core. Copy failure leaves the clipboard intact;
cut requires successful copying, unchanged source identities and a complete enabled
history checkpoint before deletion. Paste changes selection after success. Existing
bar-relative placement and timeline quantization remain in the GUI adapter. The
core accepts explicit source origin and destination position without GUI objects.
`note.selection.copy` exposes the same native copying path to structured callers.
Required fields are `track`, `clip`, `notes`, `targetTrack`, `targetClip` and
`position`. Optional `container` defaults to `song`; `targetContainer` defaults to
the source container. Both owners must be existing MIDI clips. `notes` contains
1..4096 distinct current integer note indices. `position` is relative to the
destination clip and places the earliest selected note there, preserving relative
timing. Source notes remain, including when copying into the source clip. Song and
pattern destinations append into their existing cell without replacing it.

For example:

```json
{"id":"repeat-phrase","op":"note.selection.copy","args":{"track":0,"clip":0,"notes":[1,0],"targetTrack":1,"targetClip":0,"position":192}}
```

The result contains destination `container`, `track`, `clip`, requested `position`,
`sourceOrigin`, `copied`, and `notes`. Each note receipt has `sourceNote`, current
destination `note` and clip-relative `position`; receipts follow request order even
when destination indices are sorted differently. Discover indices again after
insertion or sorting. Queued calls track both clip owners and each resolved source
note's lifetime token. Earlier calls may create unresolved future note indices;
same-object edits retain identity. Returned insertion callbacks that retire or
replace a copied note cause failure before a successful result roster is issued.
Direct core failure can retain committed native edits; structured execution uses
the existing whole-project rollback boundary.

Both provider protocols declare `note_selection_copy` individually and retain
`studio_execute` catalog routing under the 128-declaration budget. The catalog has
162 native operations plus ten supplemental agent operations. The new
`StudioNoteSelectionCopyTest` authors fourteen cases / 49 scenarios for receipts,
cross-container and same-clip transfer, validation, native session history/recovery,
queued identities and scripted direct/catalog agent execution. All remain
uncompiled/unrun; scripted decisions provide no real-provider musical evidence.

`note.selection.shift` moves/transposes selected notes in place through the same
`MidiClip.shiftNotes` core used by piano-roll keyboard shortcuts. Required fields
are `track`, `clip` and `notes` (1..4096 distinct current indices); optional
`container` defaults to `song`. Supply `semitones`, `ticks`, or both. Each is a
signed integer bounded by the current schema. Omitted deltas mean zero. Zero
deltas are accepted. Source notes, helpers, inline curves, controllers and graph
links retain their native identities, including links outside the selection.
Step and zero-length notes retain their native lengths and types.

```json
{"id":"shift-phrase","op":"note.selection.shift","args":{"track":0,"clip":0,"notes":[1,0],"semitones":-12,"ticks":-96}}
```

Keys clamp individually to native bounds, so intervals may collapse at an edge.
A negative time shift stops the earliest selected note at clip start, even if the
input indices are unsorted. `appliedTicks` reports this actual shared delta. An
endpoint beyond the native song range refuses the complete edit before writing.
No quantization, stretching, length or velocity change is implied.

Results include `container`, `track`, `clip`, requested `semitones` and `ticks`,
`appliedTicks`, `changed`, and request-order `notes` receipts. Each receipt has
`sourceNote`, current sorted `note`, `position` and `key`. Sorting can change note
indices; rediscover them before subsequent decisions. Queued selection tracking
uses the current note lifetime tokens and clip owners. Same-object edits preserve
lifetime identity. Calls whose resolved indices select different notes after a
shift are skipped so the agent can replan from a fresh observation.

The core validates current membership/lifetimes/scalars, prepares stable sorting
before publishing history, and uses the complete native clip checkpoint when
journalling is enabled. Open cross-clip control groups can refuse that checkpoint;
disabled history keeps their existing topology. Empty/no-op native calls publish
no clip edit/history; structured requests require a nonempty selection and follow
the runtime's ordinary mutating-operation history rules. Returned native callback
failures can retain a committed edit and its checkpoint. Whole-project structured
recovery is supplied by StudioRuntime. Post-notification retirement of a selected
note or clip refuses success. Callback scalar edits are not reversed or asserted
to equal the requested values; arbitrary callee/parent/plugin deletion, reentry,
concurrent mutation and external side effects remain outside these guarantees.

Both provider protocols declare `note_selection_shift` individually and retain
`studio_execute` routing within 128 declarations. The registered
`StudioNoteSelectionShiftTest` authors eighteen cases / 56 scenarios for native
clamping/overflow/sorting/identity and step/zero-length retention, history and
recording ownership, cancellation, invalid requests, song/pattern receipts,
durable recovery/reopening, four scripted shift/inspect routes and four queued
replan routes, 1050 notes and schema/tool bounds. All remain uncompiled/unrun.
Full rosters/maps are resident, membership checks can be quadratic and native
sorting/allocation/serialization/callbacks have no hard deadline. CPU/RSS/latency,
C++/Qt/MOC/ABI, GUI/legacy/plugin/DSP, PCM/hardware, real-provider musical judgment
and complete DAW acceptance remain unverified.

`note.selection.quantize` applies native rounding in place through the same core
used by piano-roll quantization. Required fields are `track`, `clip`, `notes`
(1..4096 distinct current indices) and `grid` (an integer tick size, 1 through the
schema's maximum song length). Optional `container` defaults to `song`; `mode` is
`both` (default), `positions`, or `lengths`.

```json
{"id":"tighten-phrase","op":"note.selection.quantize","args":{"track":0,"clip":0,"notes":[1,0],"grid":48,"mode":"both"}}
```

Quantization calls native `Note::quantized`. Positions and nonzero lengths round
using LMMS's existing float routine. Rounded zero lengths become one grid unit.
Original zero-length notes skip the entire operation, including position changes,
as in the piano roll. Negative step lengths retain native rounding/truncation
behavior and may become positive. The note type is retained. Endpoint overflow
refuses the whole edit before writing. Inline curve points are not resampled or
stretched; their native content and helper/control identities remain in place.

Results contain `container`, `track`, `clip`, `grid`, resolved `mode`, `changed`,
and request-order `notes` receipts. Each receipt includes `sourceNote`, current
sorted `note`, `position`, `key` and `length`. Notes can cross unselected notes
when positions round; rediscover indices afterward. Shared queued note/owner
provenance skips affected pending calls and allows the existing runtime to replan.
Both provider protocols declare `note_selection_quantize` individually and retain
`studio_execute` routing under the 128-declaration budget.

Piano-roll quantization previously removed/recreated each note. It now uses native
`MidiClip.quantizeNotes`, preserving note/curve/helper/controller identities and
links across or outside the selection. Shifting and quantization share private
admission/planning/history/commit/notification handling. Stable sorting is prepared
before checkpoint publication and scalar writes. The core refuses publication if
a cooperative callback changes a roster lifetime/position needed for that ordering;
full clip roster snapshots cover unselected notes too. It skips history/edit
publication for no-op/empty selections; structured calls require selected indices
and keep ordinary mutating-operation revision semantics. Enabled clip history
requires complete closed graph coverage. Returned post-commit callback failures
can retain edits; structured whole-project recovery remains in StudioRuntime.
Scalar callback edits, arbitrary callee/parent/plugin retirement, reentry,
concurrent mutation, throwing cleanup and external effects retain the native edit
limitations described above.

`StudioNoteSelectionQuantizeTest` authors sixteen cases / 67 scenarios covering
rounding/modes, graph/identity retention, malformed requests, three endpoint
refusals, native history, ownership/recording/cancellation, song/pattern sorted
receipts, session recovery/reopening, eight scripted provider/tool success/replan
routes, 1050 linked notes and schema/tool bounds. All are uncompiled/unrun. Native
float behavior and one final notification replace the prior GUI's per-note
remove/add notifications; GUI/legacy/plugin/DSP compatibility needs execution.
Full rosters/maps/checkpoint XML remain resident; membership can be quadratic and
allocation/sorting/native callbacks have no hard deadline or measured CPU/RSS/
latency. C++/Qt/MOC/ABI, actual PCM/hardware, real-provider musical reasoning/
listening/revision and complete DAW acceptance remain unverified.

`note.selection.reverse` mirrors selected note positions in place using LMMS's
native geometry: latest selected end minus (old start minus earliest selected
start) minus that note's length. Required fields are `track`, `clip` and `notes`
(1..4096 distinct current integer indices); optional `container` defaults to
`song`. Song and pattern MIDI clips use the same native operation.

```json
{"id":"reverse-phrase","op":"note.selection.reverse","args":{"track":0,"clip":0,"notes":[1,0]}}
```

Keys, lengths, note types, note/helper/controller identities and graph links remain
in place. Stored inline curve points remain as stored relative to their note;
this does not mirror curve contents or reverse audio samples. Zero and negative
step lengths retain native reversal semantics. A mixed signed-length selection
can produce positions outside native time bounds; the whole edit then refuses
before writing. Empty, single-note and otherwise unchanged core selections publish
no native edit/history. Structured calls require a nonempty explicit selection.

The result contains `container`, `track`, `clip`, `changed` and request-order
`notes` receipts with `sourceNote`, current sorted `note`, `position`, `key` and
`length`. Reversal usually changes note indices. Rediscover them before further
decisions; shared queued provenance skips calls whose resolved notes changed and
lets the runtime replan. Both provider protocols declare `note_selection_reverse`
individually and retain `studio_execute` routing under 128 declarations.

The checked `MidiClip.reverseNotes` overload shares admission/planning/history/
commit/notification handling with shifting and quantization. The existing void
entry delegates and logs refusals; the piano roll calls the checked overload and
reports failures through native song diagnostics. Enabled clip history requires
complete closed graph coverage. Structured runtime checkpoints retain the broader
project recovery boundary. Returned post-commit callbacks can leave edits applied;
the common callback/ownership/resource limitations above remain.

`StudioNoteSelectionReverseTest` authors fourteen cases / 51 scenarios for native
geometry/identity/graphs, no-op history, signed/zero-length and endpoint cases,
native history/coverage/recording/ownership/cancellation, song/pattern receipts,
session undo/redo/reopening/replay, returned notification recovery, eight scripted
provider/tool success/replan routes, 1050 linked notes and schema/tool bounds.
All are uncompiled/unrun. New owner/recording/history/time admission and stable
sorting/length notifications require GUI/plugin/legacy/DSP compatibility checks.
Full rosters/tokens/maps/checkpoint XML remain resident; membership can be quadratic
and native allocation/sorting/callbacks have no hard deadline or measured CPU/RSS/
latency. C++/Qt/MOC/ABI, actual PCM/hardware, real-provider musical production and
complete DAW acceptance remain unverified.

`note.selection.length` edits selected note lengths in place. Required fields are
`track`, `clip`, `notes` (1..4096 distinct current integer indices) and `mode`.
Optional `container` defaults to `song`. Modes are:

| Mode | Bound | Native behavior |
| --- | --- | --- |
| `maximum` | Required signed integer `length` | Shorten lengths greater than the bound. |
| `minimum` | Required signed integer `length` | Extend lengths less than the bound. |
| `trim` | Forbid `length` | Only shorten to the first strictly later start among all notes in the clip; last notes stay unchanged. |
| `fill` | Forbid `length` | Only extend to the first clip start at or after each original end; if none exists, use the selected phrase's latest end rounded up to a complete current bar. |

```json
{"id":"shorten-phrase","op":"note.selection.length","args":{"track":0,"clip":0,"notes":[1,0],"mode":"maximum","length":24}}
```

The bound uses native signed length units, within the schema's maximum song range.
Zero/negative bounds and existing step lengths retain native comparisons; a length
can change sign while its note type remains unchanged. Trim/fill references include
unselected notes across all keys. Fill skips starts before the original end,
preserving overlaps, and exact-end starts produce no extension. Zero/negative
lengths follow the same native rules. Positions and keys are retained; stored inline
curves are not stretched or resampled.

Results contain `container`, `track`, `clip`, resolved `mode`, `changed`, and
request-order `notes` receipts with `sourceNote`, current sorted `note`, `position`,
`key` and `length`. Maximum/minimum also echo the requested bound as `length`.
Existing sorted note indices normally remain stable because positions do not move;
shared queued note/owner provenance still rejects retired selections. Both provider
protocols declare `note_selection_length` individually and retain `studio_execute`
routing within 128 declarations.

Piano-roll fit and minimum/maximum commands delegate to native
`MidiClip.adjustNoteLengths`. All four modes share admission/planning/history/
commit/notification handling with the other in-place edits. Reference starts are
snapshotted and searched independently of request ordering. Fill snapshots and
rechecks current ticks per bar across cooperative work. Candidate lengths and
endpoints, including a final bar beyond maximum song length, refuse the entire
edit before writing. Note/helper/controller/curve identities and graph links remain
in place. Enabled clip history requires closed graph coverage. No-op/empty native
calls publish no edit/history; structured calls retain ordinary mutating-operation
revision semantics. Returned callbacks can leave committed native edits; structured
project recovery and the shared ownership/callback limitations above remain.

`StudioNoteSelectionLengthTest` authors seventeen cases / 75 scenarios for signed
constraints, trim/fill reference geometry, graph/curve/type/identity retention,
endpoint/final-bar refusal, meter changes, ownership/recording/cancellation, native
history, song/pattern receipts, durable session recovery/reopening, eight scripted
provider/tool success/replan routes, 1050 notes and schema/tool bounds. These remain
uncompiled/unrun. The GUI now uses explicit admission, prepared sorting and final
length/data notifications; native GUI/plugin/legacy/DSP compatibility is unverified.
Full rosters/tokens/reference vectors/maps/XML remain resident. Reference planning
uses sorting/binary search, membership can be quadratic, and native allocation/
serialization/callbacks have no hard deadline or measured CPU/RSS/latency. Native
C++/Qt/MOC/ABI, actual PCM/hardware, real-provider musical production and complete
DAW acceptance remain unverified.

`note.selection.glue` joins selected overlapping/touching notes of the same key
using native length geometry. Required fields are `track`, `clip`, `notes`
(1..4096 distinct current integer indices); optional `container` defaults to
`song`. Notes not selected cannot bridge a gap or be consumed by the operation.

```json
{"id":"join-phrase","op":"note.selection.glue","args":{"track":0,"clip":0,"notes":[1,0]}}
```

Each group's earliest note survives. Equal key/start ties retain native clip roster
order, independently of request order. It keeps its position, key, type, volume,
panning, helper/controller/curve identities and settings. Its length takes the
maximum existing group extent, using native zero/negative length comparisons.
Consumed notes retire through normal native ownership. Their own curves,
controller bindings and parameter values are discarded rather than combined with
the survivor; outside links detach when the receiver is actually destroyed.
Existing shared helper ownership can keep controls alive. Curve contents are not
resampled or stretched. The operation therefore follows native glue semantics
rather than preserving every consumed note's sound-design data in one note.

Results contain `container`, `track`, `clip`, `changed`, the number of consumed
notes as `removed`, the number of distinct selected survivors as `survivors`, and
request-order `notes` receipts. Each receipt maps original `sourceNote` to original
`survivingSourceNote` and current sorted `note`, with `merged`, `position`, `key`
and resulting `length`. Several sources can map to the same survivor. Unselected
notes are excluded from the survivor count. Rediscover indices and parameter
handles after gluing; shared queued provenance skips consumed/retargeted selections
and permits replanning. Both provider protocols declare `note_selection_glue`
individually and retain `studio_execute` routing under 128 declarations.

Piano-roll glue delegates to native `MidiClip.glueNotes`, keeping its selected-or-all
convention. The core validates owner/recording/bounds and full roster lifetime/
scalar snapshots, prepares groups and the combined sorted roster, and publishes a
complete enabled clip checkpoint before writing. It commits survivor lengths and
the roster under the native track lock, transfers consumed notes into temporary
ownership, then retires them and sends final type/length/data notifications.
Survivor lifetimes are checked before success. Empty/single/disjoint core calls
publish no edit/history, including safe empty input that previously reached an
invalid iterator calculation in the GUI loop. Structured calls require a nonempty
selection and retain ordinary mutating-operation revision semantics.

Enabled clip history requires closed graph coverage. Undo can recreate consumed
notes/controls and their graph; note identities inside history restoration can
change and XML precision remains native. Returned post-commit notification/cleanup
failures can retain edits and their checkpoint. Whole-project structured recovery
remains in StudioRuntime. Arbitrary callee/parent/plugin deletion, reentry,
concurrent mutation, throwing destruction/cleanup and external effects remain
outside this contract.

`StudioNoteSelectionGlueTest` authors sixteen cases / 56 scenarios for survivor
settings/identity and consumed control retirement, nine geometry/tie/step cases,
selection/key/chain boundaries, no-op history, admission, native graph undo/redo,
song/pattern mappings, session recovery/reopening, eight scripted provider/tool
success/replan routes, 1050 linked notes and schema/tool bounds. All are uncompiled/
unrun. Deterministic tie order and batched notifications/admission need GUI/plugin/
legacy/DSP compatibility checks. Full rosters/tokens/maps/prepared vectors/XML remain
resident; membership and native linked cleanup can be quadratic, and allocation/
sorting/serialization/destruction/callback work has no hard deadline or measured
CPU/RSS/latency. C++/Qt/MOC/ABI, actual PCM/hardware, real-provider musical production
and complete DAW acceptance remain unverified.

`note.selection.split` splits selected notes at one clip-relative tick. Required
fields are `track`, `clip`, `notes` (1..4096 distinct current integer indices) and
`position` (within the schema's song range). Optional `container` defaults to
`song`. It splits only strictly inside a positive-length note. Exact boundaries,
noncrossing notes and zero/negative lengths remain unchanged.

```json
{"id":"split-phrase","op":"note.selection.split","args":{"track":0,"clip":0,"notes":[1,0],"position":96}}
```

Original left notes and their controls retain identity. Fresh right fragments use
native Note/curve constructors and complete copied control metadata. Closed linked
groups among crossing notes are restored independently on the right; the left
group stays intact. A linked peer outside the crossing set refuses copying, even
if that peer was selected but does not cross this cut. Shared rack bindings retain
native ordinal references; owned MIDI controllers are copied independently.

Keys, types, volume/pan and selection flags retain native copy semantics. Right
curve points retain their original relative tick positions; they are not shifted
or resampled for continuity and can restart their shape at the new note start.
This matches native split copying rather than claiming continuous detuning.

Results contain `container`, `track`, `clip`, requested `position`, the number of
actual cuts as `split`, `changed`, and request-order `notes` receipts. Each receipt
has original `sourceNote`, current left/unchanged `note`, its `position`, `length`,
`key` and boolean `split`. Actual cuts also include `rightNote`, `rightPosition`
and `rightLength`. Noncrossing entries omit those right fields. Insertion/sorting
can move indices of unchanged later notes; rediscover them before subsequent
decisions. Shared queued provenance skips retargeted selections and allows existing
future-target semantics for notes created by an earlier call. Both provider
protocols declare `note_selection_split` individually and retain `studio_execute`
routing under 128 declarations.

`NativeNoteTransfer.split` and the checked `MidiClip.splitNotes` overload prepare
right fragments and their graph before publishing a complete enabled clip journal
checkpoint. Full roster lifetime/scalar/selection snapshots are revalidated across
cooperative work and preparation. Left length changes and ownership insertion use
one native track lock and prepared sorted roster, without a second graph-losing
clone. Returned source/right-fragment retirement refuses success. No-op core calls
publish no edit/history and return one null fragment slot per input. Failures clear
the output roster. The legacy vertical split entry delegates to this path and logs
refusals. Sloped knife and keep-longer edits now use the same prepared cut kernel,
through the checked line entry described below.

Enabled clip history still requires closed whole-clip serializer coverage, in
addition to crossing-set copy closure. Prepared controller factories and native
callbacks retain ordinary side-effect/cleanup limits. Returned post-commit failure
can leave edits and a checkpoint; structured runtime project recovery remains the
broader boundary. History restoration can recreate note/helper identities and uses
native XML precision. Arbitrary callee/parent/plugin deletion, reentry, concurrent
mutation, throwing cleanup and external effects remain outside the contract.

`StudioNoteSelectionSplitTest` authors eighteen cases / 67 scenarios for native
geometry/boundaries, left identity/flags and independent right graphs/controllers,
copy-closure refusal, no-op/history, ownership/cooperative changes, sorted song/
pattern receipts, session recovery/reopening, eight scripted provider/tool success/
replan routes, 1050 split notes and schema/tool bounds. All are uncompiled/unrun.
Full rosters/tokens/control XML/clones/combined vectors remain resident; membership
and repeated revalidation can be quadratic, and constructors/graph operations/
callbacks have no hard deadline or measured CPU/RSS/latency. Native C++/Qt/MOC/ABI,
GUI/plugin/legacy/DSP, actual PCM/hardware, real-provider musical production and
complete DAW acceptance remain unverified.

`note.selection.knife` applies the native piano-roll knife line. Required fields
are `track`, `clip`, `notes` (1..4096 distinct current indices), `fromPosition`,
`fromKey`, `toPosition` and `toKey`. Optional `container` defaults to `song` and
`keepLongest` defaults to `false`. Endpoint fields accept the full native signed
integer range, including coordinates outside the clip or keyboard.

```json
{"id":"knife-phrase","op":"note.selection.knife","args":{"track":0,"clip":0,"notes":[1,0],"fromPosition":72,"fromKey":59,"toPosition":168,"toKey":63,"keepLongest":true}}
```

Geometry retains the native float slope and note-centre intercept: endpoint tick
delta divided by endpoint key delta, evaluated at `key - 0.5`. The minimum endpoint
key is excluded and the maximum included, regardless of endpoint direction. Equal
endpoint keys do nothing. Intercepts truncate to integer ticks and must fall
strictly inside a bounded positive-length note. Endpoint differences use widened
arithmetic; finite/representable intercepts are checked before integer conversion.
No quantization is added. Planning uses immutable captured note fields and checks
the current roster again before editing.

With `keepLongest:false`, original notes become left halves and fresh right halves
copy complete closed control groups among crossing notes, as in vertical splitting.
With `keepLongest:true`, no new notes are created: the original note keeps the
longer interval and moves to the cut if the right half is longer. Equal lengths
keep the left interval. Its existing note/helper/controller/curve identities and
outside links remain. This deliberately replaces the old knife's remove/recreate
behavior. Whole-clip enabled history still requires complete closed coverage;
crossing-set copy closure is needed only when creating right fragments.

Results echo endpoints and `keepLongest`, with `container`, `track`, `clip`, actual
`cut` count, `changed` and request-order `notes`. Each entry has original
`sourceNote`, current retained/original `note`, `position`, `length`, `key` and
boolean `cut`. Actual cuts also have `cutPosition` and `kept` (`both`, `left` or
`right`). New right fragments add `rightNote`, `rightPosition`, `rightLength`.
Noncut entries omit cut/fragment details. Moving a retained right interval or
inserting right fragments can shift existing indices. Rediscover them afterward;
queued provenance skips retargeted selections and permits replanning. Both provider
protocols declare `note_selection_knife` individually and retain `studio_execute`
routing under 128 declarations.

Vertical split and knife share native snapshot/preparation/graph/checkpoint/commit/
notification handling. The piano-roll gesture calls checked
`MidiClip.splitNotesAlongLine` and still completes gesture cleanup after refusal;
the legacy line entry delegates and logs failures. Empty/horizontal/no-crossing
native calls publish no edit/history and return null right slots and `-1` cut
positions per input. Structured no-op calls retain ordinary mutating revisions.
Stored curves are not rebased or resampled, including when keeping a right interval.
Returned post-commit failure can retain edits/checkpoint; structured recovery and
the native ownership/callback/factory/cleanup limitations described above remain.

`StudioNoteSelectionKnifeTest` authors seventeen cases / 72 scenarios for native
line/row/truncation/extreme geometry, both and keep-longer identity/graph behavior,
tie choice, copy closure, history/ownership/cooperative changes, strict requests,
song/pattern receipts, session recovery/reopening, eight scripted provider/tool
success/replan routes, 1050 notes and schema/tool bounds. All are uncompiled/unrun.
Preserved originals, prepared stable sorting and batched notifications differ from
the prior GUI loop and need GUI/plugin/legacy/DSP compatibility evidence. Full
rosters/tokens/control XML/clones/maps remain resident, membership/revalidation can
be quadratic, and native constructors/graphs/callbacks have no hard deadline or
measured CPU/RSS/latency. C++/Qt/MOC/ABI, actual PCM/hardware, real-provider musical
production and complete DAW acceptance remain unverified.

Native note roster ordering is position ascending, then key descending. Prepared
in-place edit/cut sorts now use that full ordering, including proposed keys before
transposition commits. Exact key/position ties keep stable order. New shift and
split regressions insert another note afterward through native `addNote`, checking
that the roster still satisfies the comparator required by native insertion.
Varied-key large split/knife fixtures now use a retained link anchor rather than
assuming the first note pointer stays fixed as higher keys are inserted.

`step.clone` doubles one MIDI row's current native step count and copies the first
Step note at each original grid position. Required fields are `track` and `clip`;
optional `container` defaults to `song`. The structured result count is limited
to 4096 steps. Empty and regular-only rows still double their step count.

```json
{"id":"repeat-beat","op":"step.clone","args":{"container":"pattern","track":0,"clip":0}}
```

Sources follow ascending source-step order. Each destination uses the native
`floor(step * ticksPerBar / stepsPerBar)` grid, with widened arithmetic; this is
not assumed to be an affine offset on nonintegral grids. Extra duplicates at one
source grid point, off-grid Step notes and regular notes remain in the original
roster and are not copied. Equal-position source choices use native roster order.
An existing Step note at a mapped destination refuses the operation before editing,
instead of overwriting its scalar settings as the old GUI loop could do.

New notes retain full native content, stored curve points/timing, type, key,
length, volume/pan and independent copied control groups. Originals retain identity
and selection; copies are unselected, matching the old slot's default selection
behavior. Copied groups must be closed across the chosen first-per-grid sources;
links into uncopied duplicates, regular notes or other rows refuse copying.
Owned MIDI controllers copy independently and rack references keep native ordinal
semantics. Curve contents are not resampled.

Results contain `container`, `track`, `clip`, original `fromCount`, resulting
`count`, `copied`, and source-step-order `notes` receipts. Each receipt includes
original `sourceNote`, `sourceStep`, destination `step`, current copied `note`,
`position`, `key` and `length`. Rediscover indices afterward. The operation uses
current row content after earlier edits; `step.set` followed by `step.clone` can
share a batch while the track/clip owner remains valid. Replaced row owners are
skipped through existing queued provenance. Both provider protocols declare
`step_clone` individually and retain `studio_execute` routing within 128 tools.

The existing GUI slot delegates to checked `MidiClip.cloneSteps`, used by the clip
menu and pattern editor. Clones/graphs and the sorted roster are prepared before a
complete enabled clip checkpoint. Count, meter and full roster scalar/lifetime
snapshots are revalidated through preparation and checkpoint serialization. Count
and ownership insertion commit under one track lock, including empty rows. Song,
arithmetic-product and rounded-bar bounds refuse before writing. Returned original/
copy retirement or count changes refuse success. Native post-commit failures can
retain edits/checkpoint; structured runtime supplies broader project recovery.
The pattern editor's multiple-row loop remains sequential, without a whole-pattern
transaction. Enabled native clip history requires complete closed coverage.

`StudioStepCloneTest` authors fifteen cases / 47 scenarios for complete independent
controls/controller/curve state, first-Step selection and retained unselected
content, nonintegral grids, empty rows, bounds/collision/coverage, native history,
ownership/cooperative changes, song/pattern receipts, session recovery/reopening,
eight scripted provider/tool current-row/replan routes, 1050 copied notes and
schema/tool bounds. All are uncompiled/unrun. Full-content copy, collision refusal,
atomic count/roster publication and batched notifications need GUI/plugin/legacy/
DSP compatibility checks. Full rosters/tokens/grid maps/XML/clones remain resident;
membership/revalidation can be quadratic and native factories/serialization/
callbacks have no hard deadline or measured CPU/RSS/latency. Native C++/Qt/MOC/ABI,
actual PCM/hardware, real-provider musical production and complete DAW acceptance
remain unverified.

`step.set` now uses the same prepared native row-edit kernel as `step.resize`:

```json
{"id":"enable-cell","op":"step.set","args":{"container":"pattern","track":0,"clip":0,"step":2,"enabled":true}}
```

The cell is zero based and must be within the current positive row count; structured
rows are limited to 4096 steps. Enabling retains the first existing Step note in
native roster order, including its controls, identity, selection and duplicate
content. If none exists, it inserts one unselected native default Step note: the
existing default key/volume/pan, length `DefaultTicksPerBar / 16`, no detuning helper,
and exact widened native floor-grid position. Regular notes do not enable a cell.
Disabling removes every Step duplicate at the cell, retaining regular/off-grid notes
and leaving the step count unchanged. Normal removal detaches control links.

Receipts retain `step`/`enabled` and add `changed`/`removed`; enabled receipts include
the current `note` index and `position`. Rediscover note indices after changes.
Native equal-state no-ops publish no history or notifications; structured mutation
receipts retain ordinary session revision behavior. Row ownership is bound, while
cell contents and bounds are resolved at execution: `step.resize` then `step.set`
on a newly available cell can share a batch. Later queued note targets removed by
disable become stale and trigger the existing skip/replan behavior. `step_set` is
individually declared for both provider protocols, with catalog routing available;
22 protected native declarations fit the existing 128-tool budget.

Checked `MidiClip.setStep` snapshots full roster scalar/lifetime state, meter and
count, prepares the complete sorted insertion or retained ownership roster, and
revalidates before one complete enabled clip checkpoint. Publication uses one track
lock, followed by normal cleanup and native notifications. Enabled/retained note
lifetimes, count, meter and expected roster size are checked before success. Native
failure after publication can retain edits/checkpoint; StudioRuntime provides the
broader whole-project recovery boundary. The legacy `setStep` entry delegates, GUI
click toggling uses it without a separate manual checkpoint, and wheel creation uses
checked enable. Existing-note wheel volume editing and low-level `addStepNote`
retain their prior paths. GUI negative cells now refuse before native lookup.

`StudioStepSetTest` authors eighteen cases / 60 scenarios: native defaults and one
publication, preserved existing duplicate content, all-duplicate removal/linked
cleanup, no-op history, native undo/redo/legacy entry, nonintegral grids, twelve
bounds/history refusals, eight owner/cooperative changes, nine malformed structured
requests, song/pattern receipts, durable replay/history/reopening, four returned
failure recovery paths, twelve scripted Chat/Responses direct/catalog future-cell/
stale-row/stale-note routes, 1050 duplicate removals and schema/declaration bounds.
All are uncompiled/unrun. Idle model ownership and enabled closed whole-clip history
coverage are required. New GUI checkpoint/notification/modified behavior needs
native compatibility execution. Full rosters/tokens/prepared sorting remain resident;
linked cleanup can be quadratic, and native serialization/callbacks/destruction
have no hard deadline or CPU/RSS/latency evidence. Native C++/Qt/MOC/ABI, existing
projects/plugins/DSP, actual PCM/hardware, real-provider musical production and full
DAW acceptance remain unverified.

`step.resize` now uses one checked native row edit rather than a loop of per-bar
add/remove calls:

```json
{"id":"shorten-row","op":"step.resize","args":{"container":"pattern","track":0,"clip":0,"count":16}}
```

Structured source and target counts must align to the current native bar, and the
target must be 1..4096. Invalid zero/negative meter state refuses before modulo or
native length arithmetic. Growth adds empty cells. Shrinking clears every Step
note, including duplicate notes, at native grid cells in `[count, oldCount)`.
Regular notes, off-grid steps and steps outside the old prefix remain; retained
notes can still determine the clip's displayed/native length. Widened exact grid
inversion preserves native floor-grid behavior, including nonintegral ratios.

The receipt contains `fromCount`, `count`, `removed` (note count, including
duplicates) and `changed`. Rediscover note indices after shrinking. Queued row
ownership remains bound while contents are resolved at execution, allowing earlier
`step.set` edits in the same batch. A queued note removed by resize becomes stale;
the runtime skips it and replans through existing note lifetime provenance.
Both provider protocols declare `step_resize` individually and retain catalog
routing. 22 protected native declarations fit the existing 128-tool limit.

Checked `MidiClip.resizeSteps` accepts positive native counts and an optional target
limit. Existing GUI add/remove slots delegate to it with one-bar deltas; removing
from a row of at most one native bar retains the old no-op behavior. Complete count,
meter, roster/lifetime/scalar admission precedes an enabled complete clip checkpoint.
Count and the prepared retained roster publish under one track lock, then removed
notes destruct normally and detach control links before native notifications.
Retained note identities/content stay intact. Native equal-count no-ops publish no
checkpoint or notifications; structured mutation receipts retain ordinary session
revision behavior. Source/target integer-product, song-span and rounded length
bounds refuse before writing. Native post-commit failures can retain edits and
checkpoint; structured whole-project recovery remains the broader transaction.

`StudioStepResizeTest` authors seventeen cases / 54 scenarios: duplicate grid-cell
removal and retained content, one-publication growth/shrink, no-op history, native
undo/redo and legacy slots, nonintegral grids, bounds/history coverage, cooperative
admission, song/pattern receipts, durable replay/reopening/recovery, twelve scripted
provider/tool current-row/stale-row/stale-note routes, 1050 linked notes and schema
bounds. All remain uncompiled/unrun. Enabled history needs complete closed clip
coverage. Changed GUI checkpoint/notification behavior, existing projects, plugins
and DSP need native compatibility checks; pattern multi-row operations remain
sequential. Full rosters/snapshots stay resident and linked cleanup can be quadratic.
There is no hard deadline inside native serialization/callback/destruction and no
CPU/RSS/latency measurement, native build/audio/hardware or real-provider production
evidence. Complete DAW acceptance remains unproven.

`note.selection.remove` removes one native note selection in a prepared batch:

```json
{"id":"remove-phrase","op":"note.selection.remove","args":{"container":"song","track":0,"clip":0,"notes":[4,1,3]}}
```

`notes` must contain 1..4096 distinct current indices from one MIDI clip. Receipts
contain `removed` and a request-order `notes` array with original `sourceNote`
indices. Removed notes have no live handles; rediscover remaining indices afterward.
Existing weak clip/track and per-note lifetime provenance skips stale queued
selections when owners or indexed identities change. A missing future note can be
created by an earlier call and resolved at execution. Both provider protocols
declare `note_selection_remove` individually and retain catalog routing; 22
protected native declarations, 162 native operations and ten supplemental agent
operations fit the 128-declaration budget.

Checked `MidiClip.removeNotes` snapshots full roster scalar/lifetime state, count
and meter, rejects null/duplicate/foreign selections, prepares retained ownership
and removed-note capacity, revalidates cooperative admission and one complete
enabled clip checkpoint, and publishes the retained roster under one track lock.
Removed notes then destruct normally, detaching controls according to native shared
ownership. A helper/controller can outlive one removed note when another native
owner retains it. Surviving note identities/content and step count remain intact;
post-notification roster size, count, meter and surviving lifetimes precede success.
Returned failures clear the removal count. Native post-commit failure can retain
edits/checkpoint; StudioRuntime supplies the broader whole-project recovery boundary.

Native empty selections are no-ops without history or notification. Nonempty edits
require valid count/meter/arithmetic and retained-note length paths. Invalid selected
geometry can be deleted without retaining it; invalid surviving geometry refuses.
Enabled history requires complete closed whole-clip serializer coverage and can
refuse links outside that clip. With history disabled, ordinary subset deletion can
detach outside peers without requiring copied-group closure.

Existing `note.remove` now uses this checked path and returns `removed`.
`note.clear` uses checked native whole-row clearing, returns `removed`, and resolves
all current notes after earlier batch edits. Native `clearNotes(removed,error)`
accepts the full aliased roster, retains clip metadata and step count, and treats an
empty clip as a native no-op. Structured mutations retain ordinary session revision
behavior. The GUI clip clear command and piano-roll selected cut/delete delegate to
the same core; clipboard publication and selected lifetime validation still precede
cut removal. Low-level `removeNote`/zero-argument `clearNotes` remain for native
loading/callback lifecycles, and mouse erase gestures retain their prior paths.

`StudioNoteSelectionRemoveTest` authors seventeen cases / 53 scenarios: retained
identity/control content and outside-peer cleanup, selected malformed geometry,
aliased full clear/metadata/count, empty native history, five malformed/coverage
refusals, eight owner/cooperative changes, three native history/legacy clear paths,
ten malformed structured requests, song/pattern request-order receipts, existing
single/clear interfaces, two durable replay/history/reopening paths, three returned
failure recovery paths, twelve scripted Chat/Responses direct/catalog future-note/
current-clear/stale-index/stale-owner routes, 1050 removals and schema/tool bounds.
All remain uncompiled/unrun. GUI publication/history/modified behavior and native
project/plugin/DSP compatibility need execution. Full rosters/tokens/sets remain
resident and linked cleanup can be quadratic; serialization/callbacks/destruction
have no hard deadline or measured CPU/RSS/latency. Native C++/Qt/MOC/ABI, actual
PCM/hardware, real-provider musical production and full DAW acceptance remain
unverified.

`note.selection.edit` applies supplied absolute scalar fields uniformly to one
native note selection:

```json
{"id":"phrase-dynamics","op":"note.selection.edit","args":{"container":"song","track":0,"clip":0,"notes":[4,1,3],"volume":110,"panning":-20}}
```

The selection must contain 1..4096 distinct current indices from one MIDI clip.
Optional `position`, `length`, `key`, `volume` and `panning` replace the corresponding
field on every selected note. Omitted fields stay unchanged; this is an absolute
edit, so supplying one position can place multiple notes at that same tick. Use
`note.selection.shift` for relative time/pitch changes. Structured explicit lengths
remain positive; omitted signed/zero native lengths are retained. Volume/panning
accept bounded numbers and truncate toward zero into native integer note fields,
after the original full numeric value passes its bounds check.

Receipts contain `changed` and request-order `notes`, each with original `sourceNote`,
current `note`, `position`, `length`, `key`, `volume` and `panning`. Existing `note.edit`
uses the same path for one note and returns that receipt plus `changed`. It now
parses every supplied field before editing; a valid position followed by an invalid
key/volume/pan cannot leave that position partially changed. Both entries preserve
note identities/type/selection and inline control/curve content. Stored curve points
are not resampled or retimed. Empty/equal field sets are native no-ops with no native
checkpoint or notification; structured mutation revisions retain ordinary behavior.

Checked `MidiClip.editNotes` uses the native shared selection-edit kernel. Full
roster scalar/lifetime snapshots, source ownership, idle recording, positive count/
meter, final selected endpoints, retained geometry and rounded native length paths
precede prepared native position/descending-key sorting and one complete enabled
clip checkpoint. Supplied native fields can repair malformed selected geometry if
the final geometry is admitted. Prepared fields and roster publish under one track
lock, followed by normal native notifications. Returned selected scalar changes,
selected/unselected retirement, count/meter/roster changes refuse success. Native
post-commit failure can retain edits/checkpoint; StudioRuntime supplies broader
whole-project recovery. Enabled native history requires closed full-clip coverage.

The shared transform kernel now snapshots and checks full scalar state for all
original notes, including unselected keys. This catches an unselected same-position
key change after projected sorting before checkpoint/commit. GUI numeric velocity/
panning dialogs use the checked batch core, with pre-modal weak clip/note lifetime
capture and post-modal validation. Other drag/scribble/wheel/low-level setters retain
their existing paths. These changes require native GUI/legacy/plugin/DSP checks.

Queued owner/note identity provenance skips changed indexed targets and permits
missing future notes created earlier in the same batch. Position/key edits can
change indices, so rediscover notes before later targeted calls. Both provider
protocols individually declare `note_edit` and `note_selection_edit` and retain
catalog routing. 22 protected native declarations, 162 native operations and
ten supplemental agent operations fit the existing 128-declaration budget.

`StudioNoteSelectionEditTest` authors eighteen cases / 65 scenarios: complete fields/
one publication/identity/control retention, three signed/zero/type omission paths,
native ordering and later insertion, three no-op paths, eleven bounds/coverage/
later-note refusals, malformed selected geometry repair, nine owner/cooperative
changes including a key change after sorting, two native history paths, eleven
malformed structured calls including late single-note fields, song/pattern receipts/
fraction conversion, existing single-note behavior, two durable history/reopening
paths, four returned notification/retirement/scalar failures, twelve scripted Chat/
Responses direct/catalog future-note/stale-index/stale-owner routes, 1050 linked
notes and schema/tool bounds. All remain uncompiled/unrun. Full rosters/scalars/
tokens/maps/sort remain resident, repeated membership/revalidation can be quadratic,
and native virtual setters/serialization/callbacks have no hard deadline or measured
CPU/RSS/latency. Native C++/Qt/MOC/ABI, actual PCM/hardware, real-provider musical
production and full DAW acceptance remain unverified.

`note.batch.add` creates new native notes with one prepared insertion:

```json
{"id":"new-phrase","op":"note.batch.add","args":{"container":"song","track":0,"clip":0,"items":[{"position":192,"length":48,"key":60},{"position":240,"length":48,"key":64,"volume":110,"panning":-20}]}}
```

`items` contains 1..4096 objects. Each requires absolute tick `position`, positive
`length` and native `key` 0..127. Optional volume/panning use native defaults and
bounded integer conversion after admitting the original numeric value. Unknown
nested fields refuse, and every input is validated before native publication.
Each input creates a distinct unselected regular Note without detuning/control
content; identical inputs are permitted. Use `note.selection.copy` for existing
notes with curve/control content and step operations for structured beat cells.

Receipts contain `added` and input-order `notes`, each with `input`, current sorted
`note`, `position`, `length`, `key`, `volume` and `panning`. Input order need not
match time/key order. Existing notes keep identities, selection, scalar values and
inline controls, and the step count stays unchanged. Existing `note.add` now uses
the same checked native constructor and retains its original `{note: index}` result.
There is no GUI quantization. Both entries use current row contents after prior
edits, with weak owner provenance refusing replaced rows. Missing future notes can
be targeted after creation in a batch; existing indices still require rediscovery
when insertion reorders them.

Native `MidiClip.addNotes` accepts scalar NoteSpec values, including bounded signed/
zero lengths and Step type for native callers. It snapshots all inputs and the full
existing roster's scalar/lifetime state, validates all endpoints/count/meter/
integer-product/rounded length paths, constructs fresh native notes, and prepares
the complete native position/descending-key order before one enabled clip checkpoint
and track-locked publication. Returned old/new note retirement or scalar changes,
count/meter and roster-size changes refuse success and leave output empty. Native
post-commit failure can retain edits/checkpoint; StudioRuntime supplies the broader
whole-project recovery boundary. Native empty input is a no-op without history or
notification. Enabled history needs closed complete clip coverage.

Both provider protocols individually declare `note_add` and `note_batch_add` and
retain catalog routing. There are 22 protected native declarations, 162 native
operations and ten supplemental agent operations within the 128-declaration limit.

Current inspection also corrected a schema mismatch in `note.selection.remove`
and `note.selection.edit`: both dispatchers expected index arrays, but their schema
entries had retained the generic integer field. They now share bounded distinct
`notes` array schemas with the existing selection operations. Their native fixtures
now assert array type, minimum/maximum size and uniqueness. Earlier source-only
checks missed that mismatch; prior native execution was never established.

`StudioNoteBatchAddTest` authors seventeen cases / 62 scenarios: input-order output/
one publication/old complete controls, distinct duplicates and native tie ordering,
three signed/Step native paths, empty native history, twelve input/length/coverage
refusals, eight owner/cooperative changes, native undo/redo, twelve malformed nested
requests, song/pattern receipts and numeric conversion, original single receipt/
history, two durable replay/history/reopening paths, four returned notification/
retirement/scalar failures, twelve scripted Chat/Responses direct/catalog creation/
future-note/current-row/stale-owner routes, 1050 insertions and nested schema/tool
bounds. All remain uncompiled/unrun. Raw addNote, GUI and recording paths retain
their prior behavior. Native single-note history/publication/modified changes need
GUI/project/plugin/DSP compatibility execution. Full inputs/rosters/scalars/tokens/
allocations/sort remain resident; repeated validation can be quadratic, and native
construction/serialization/callbacks have no hard deadline or CPU/RSS/latency evidence.
Native C++/Qt/MOC/ABI, actual PCM/hardware, real-provider production and full DAW
acceptance remain unverified.

`StudioCapabilities.validateArguments` exposes the same argument preflight without
accessing native models. It implements the current catalog's primitive, object,
array, enum, inclusive numeric-bound, string length/pattern, required/additional-key,
item-count and uniqueness keywords. Unsupported schema types refuse. Number values
must be finite; integer fields must also be integral. String length counts Unicode
code points. Additional plugin attribute values must be strings. Declared index
array uniqueness uses numeric identity, including equivalent signed zero.

Preflight checks cancellation before catalog construction, periodically while
visiting values/comparisons, and at completion, with event pumping suspended. It
adds no global graph/note limit beyond published bounds. It does not replace native
state-dependent validation or provide transaction/rollback for callbacks or native
effects. Empty/equal native mutation behavior and durable receipt handling retain
their existing operation-specific semantics. Diagnostics may arrive earlier and
now include property/index paths.

The schemas now publish actual song tempo/master gain/pitch bounds, native owner
enums, track-creation types, nonempty names for operations whose native name parser
requires them, and string-valued plugin attributes. `song.set` also parses every
provided field before invoking any setter. A valid tempo followed by an invalid
master volume/pitch cannot partially change tempo in a direct call. Invalid empty
track/clip creation names and late invalid clip names likewise refuse before model
creation/movement. Controller/mixer name behavior retains its prior admission with
earlier schema diagnostics.

`StudioCapabilitySchemaTest` authors ten cases / 46 scenarios, including a metadata
representative-argument loop over all 162 catalog entries without engine startup;
top-level/nested type/required/unknown checks, bounds/enums, index arrays, specialized
native shapes, no default insertion, digest patterns and cancellable traversal.
`StudioCapabilityPreflightTest` authors seven cases / 15 scenarios for late song
fields, invalid creation/clip names, nested attributes before plugin lookup, durable
failure replay, four scripted Chat/Responses direct/catalog correction routes and
cancelled note creation. All remain uncompiled/unrun. Representative catalog inputs
exercise schema consistency, not native reachability or musical acceptance. Native
regressions, GUI/plugin/project compatibility, CPU/RSS/latency and actual provider/
audio/hardware production remain unverified. Catalog creation, QJson copies, long
strings/regular expressions and native callbacks are not preempted.

`clip.edit` now delegates to checked native `Clip.edit` rather than applying fields
directly in the dispatcher. It retains position/length/offset/autoResize receipts
and adds `changed`. Optional name and stored mute retain native controller
precedence. Every supplied field is parsed before native admission. The clip and
its track must remain registered on the owner thread; MIDI recording, armed/active/
pending sample capture and automation recording must be idle.

Manual position/end/offset bounds are admitted before writing. Explicit manual
lengths remain positive, while existing zero-length clips can retain their length
for metadata-only edits. PatternStore owns cell positions, including a request to
set the same cell position. Manual length/offset disables automatic sizing.
Explicit `autoResize:true` must be separate from manual length/offset and invokes
the native subtype's automatic sizing/offset behavior. It is an applied refresh
(`changed:true`) even when visible values happen to match. Empty/equal edits without
that refresh are native no-ops without checkpoint/notification. Automatic sizing
delegates to native routines; final range checks occur afterward and do not validate
every nested note, sample or automation state before those routines execute.

The core snapshots relevant original properties and revalidates owner/recording
state, geometry/name/stored mute and clock/meter dependencies across cooperative
admission and complete enabled clip checkpoint serialization. Native virtual move,
length, offset, automatic sizing, name and mute setters remain authoritative. Owner
and recording state are rechecked after each native setter, with sample frame-clock
and automatic meter changes refused. Subtype sample offset conversion stays native,
including its behavior after a later tempo change.

One complete enabled clip checkpoint owns the compound edit; nested native journal
publication is temporarily disabled during setter application and restored on
ordinary exit/exception. Closed whole-clip control coverage is required when history
is enabled, and native undo may recreate nested note/helper identities. Native
intermediate setter/global signals remain observable. This is not atomic publication
across arbitrary callbacks: direct failure may leave partial fields/checkpoint, and
callback effects/history retain existing limits. StudioRuntime remains the broader
whole-project recovery boundary. Final properties and bounds are checked before and
after final data/modified notifications.

`StudioClipEditTest` authors sixteen cases / 58 scenarios across four clip types,
native sample offset/tempo behavior, empty/equal edits, zero-length pattern metadata,
automatic refresh, complete count/linked-control undo/redo, ten native bounds/
coverage refusals, nine recording/owner/cooperative changes, nine malformed structured
calls, song/PatternStore receipts, durable history/reopening, four returned setter/
notification failures, restored journal scope on direct failure, eight scripted
Chat/Responses direct/catalog future/stale clip routes and schema/tool bounds. All
remain uncompiled/unrun. Existing raw setters and GUI drag paths retain their previous
behavior. Native serialization/virtual callbacks/sizing are not preempted and no
CPU/RSS/latency, native build, GUI/project/plugin/DSP compatibility, actual PCM/
hardware or real-provider complete production evidence is established.

Native clip history now wraps complete clip state and its closed control graph in
`nativeClipJournal` metadata. Undo/redo retains the Clip object while note/helper
objects can be recreated. Enabled-history transfer refuses a destination whose
complete clip graph cannot be checkpointed, including links outside that clip;
disabled history bypasses that checkpoint. Legacy journal state keeps its existing
loader. Malformed-body/graph or callback restoration failure can leave partial
native state; outside automation bindings and plugin/external effects retain native
loader limits. Initial preparation failures clean owned notes, but a notification
failure after insertion can leave the complete edit committed with a native undo
checkpoint. Broader rollback still belongs to StudioRuntime project transactions.

NativeNoteTransferTest authors twelve cases / 35 scenarios, all uncompiled/unrun.
They cover direct/clipboard/legacy transfer, invalid input, native graph undo/redo,
checkpoint refusal, post-insertion failure, cancellation/retirement, foreign-thread
admission and 1050 notes. Native C++/Qt/MOC/ABI, GUI/DSP/legacy/plugin, PCM/hardware,
provider reasoning and full autonomous DAW acceptance remain unverified.

Native value propagation now guards the initiating model and linked peers across
returned synchronous notifications. A separate ring topology generation tracks
joining, detachment and native graph installation/rollback. After a topology
change, propagation resumes at live, currently linked peers not yet visited by
this outer write. A retired sender is not dereferenced afterward, a detached
future peer is omitted, and a newly joined peer remains eligible. Weak visited
identities distinguish new objects at reused addresses. Normal raw-value fitting,
per-model signal behavior, current-ring ordering and nested setter value order
remain native. This does not reset musical clocks or unrelated buffer generations.

`parameter.set` also guards its receiver before constructing a result. Retirement
refuses a successful receipt; StudioRuntime uses its existing whole-project
rollback. Direct setter exceptions/notifications can retain earlier value and
callback side effects. Narrow setter traversal guards do not make other callers,
parent/plugin self deletion, journal serializers, reentrant project replacement
or concurrent mutation safe. Native owner synchronization is still required.
Linked writes allocate weak visited entries; changed topology can rescan the
ring, including changes in another ring. Churning callbacks have no hard deadline,
and the uint64 topology generation has a finite wrap limit. Native allocation,
latency, CPU/RSS, Qt/platform and actual audio/production execution remain unverified.

Native initialization and range changes now recheck their weak receiver after
emitting setters before continuing. XML model loading guards the value/scale
transition and legacy automation curve lifetime. It prepares a replacement
controller connection before attachment, transfers ownership to the model, then
releases the previous binding before checking whether the model survived. A
returned attachment exception still releases that old binding; direct calls can
retain the replacement and other earlier effects.

Tempo recalculation restores the surviving model's prior journalling flag even
when a returned value callback throws. It checks receiver lifetime before mode
bookkeeping; tempo loading, scale changes and meter loading/reset likewise stop
before their next receiver access. `parameter.sync` refuses success if either
custom-meter write or synchronization retires the tempo model. StudioRuntime
uses the existing full-project checkpoint recovery. These guards cover specific
caller continuations, require native owner synchronization, and do not establish
safe arbitrary deletion inside every callee, parent/plugin loader or destructor.
Controller preparation changes attachment notification timing; native compatibility
and exclusive binding ownership need execution checks. ModelSetterOwnershipTest
authors twelve cases / 27 scenarios, including returned exception recovery and
native/runtime LFO retirement. None are compiled or run; C++/Qt/MOC/ABI, actual
PCM, GUI/legacy/plugins, provider reasoning and CPU/RSS remain unverified.

Native joining/splitting, controller rewiring/retirement and ownership changes
now invalidate affected group buffers immediately, preserving unrelated cached
interpolation and musical clocks. Link admission checks controller feedback
before value copying and again after callbacks. A retired other model is skipped;
an already completed nested merge is not repeated. The native C++ link method
now returns success/failure, requiring rebuild/API review. GUI drop/unlink paths
acquire model ownership. Callback rejection does not roll back earlier value or
signal effects, and arbitrary self deletion/reentry or concurrent raw mutation
remain outside these changes.

LinkedParameterTest authors thirteen cases / 21 scenarios for same-period
linear/log inheritance, group split/rewire/retirement/ownership, feedback and
callback changes, unrelated cache preservation, 1050-peer paging/receipt/history,
invalid selectors, external peers, cancellation/event exclusion and both native
agent protocols. All remain uncompiled and unrun. Other native link lifecycle paths,
C++/Qt/MOC/ABI, GUI/audio/plugin/legacy behavior, resource costs and full autonomous
production remain unverified or incomplete.

LinkedProjectPersistenceTest authors eleven cases / 20 scenarios for cross-owner
project groups and stored values, scoped identity remapping, ordered controller
precedence, eight malformed graphs, feedback rejection, native checkpoint
undo/redo/reopening and receipt replay, legacy clearing, structured load rollback,
unsupported-member publication refusal, repeated save/unlink and cancellation.
All remain uncompiled and unrun. Graph safety does not roll back all plugin/project
side effects, and discovery/DOM traversal costs and native compatibility remain
unmeasured.

`controller.waveform.set` addresses an LFO rack `controller` and `path`. Empty
clears its user waveform; `select: true` activates the native user-wave selector.
Native decoding preserves the prior waveform on failure. `controller.list` now
reports the waveform path, frame count and sample rate. Existing controller XML
persists the path and selector; clearing also invalidates the native cached value
buffer. The LFO selector's maximum excludes the non-waveform enum sentinel;
sample-and-hold noise starts with a finite zero sample before its first cycle.

Native LFO catch-up now wraps accumulated skipped-period phase in double
precision before converting it to the existing float waveform loop. Native
playback/seek phase updates likewise wrap absolute-frame phase before storing
it. Previously a large phase could swallow the per-sample increment and produce
a constant buffer. The skipped-period difference also retains its native width
instead of narrowing to int. Existing waves, multipliers, base/amount controls,
parameter mapping, cache behavior and project XML remain native.

LfoCatchUpTest authors five cases / 11 scenarios covering delayed reads at three
native speed multipliers, a wider-than-int period difference where native long
permits it, late sine/saw seeks, same-period caching and next-period advancement,
linear/log parameter sample data, and native sample/Mixer marker rendering at
cycle-aligned early/late positions with and without project save/load. The render
fixture requires decoded, varying PCM to match within its tolerance; it is not
the commission's musical deliverable. All remain uncompiled and unrun. Native
signed-long clock lifetime/reset limits and ordinary float rounding remain;
double precision is finite, sample-and-hold noise stays stochastic, and native
audio/GUI/plugin/legacy compatibility and CPU/RSS behavior remain unverified.

Native Controller construction now initializes its normalized cached value to
the native midpoint and its sample-accuracy flag to false. The LFO, MIDI and peak
implementations retain their explicit sample-accurate selection. This prevents
first nonzero-offset/default reads and native parameter buffer classification
from depending on uninitialized storage. Per-period notification traversal uses
weak controller snapshots, skips retired controllers and processes newly
registered ones starting with the next pass. Native model ownership is still
required; this is not a concurrent-registry or arbitrary callback-deletion guard.

Controller::runningFrames now returns the native f_cnt_t width instead of
narrowing to unsigned int. Controller::runningTime preserves fractional seconds
by dividing in double before returning its existing float type. Public C++ frame
return-width expectations require a native rebuild and review of external
consumers. Native signed-long clock lifetime/reset limits and float time
resolution remain; no long-duration hardware timing is proven.

ControllerStateTest authors seven cases / 17 scenarios for initialized defaults
under three prior-storage patterns, native scalar Dummy mapping, LFO/MIDI first
offsets, fractional/wide frame helpers, notification retirement/creation and
exception return, plus native AudioEngine periods that retire another rack
controller after mixing. All remain uncompiled and unrun. Native API/ABI, GUI/
plugin/legacy compatibility, snapshot allocation/latency/CPU/RSS, actual DSP and
full autonomous production remain unverified.

`parameter.sync` operates on native TempoSyncKnobModel parameters. Modes are
0 off, 1 double whole, 2 whole, 3 half, 4 quarter, 5 eighth, 6 sixteenth,
7 thirty-second and 8 custom. Custom mode accepts positive `customNumerator` and
`customDenominator` within the native meter's bounds. Sync state and custom meter
are serialized by the existing model. All controller and curve behavior requires
native execution.

`automation.inspect` returns a native arrangement curve's targets, points,
interpolation and tension. Optional `position` evaluates a source tick. Use
`automation.connect` and `automation.disconnect` with a discovered `parameter`;
disconnect retains the raw native values. `automation.clear` removes points while
retaining target bindings. `automation.removePoint` removes an exact source tick.
All arrangement operations address `track`, `clip` and optional `container`.

`automation.point` accepts exact `position` and `value`, optional `outValue`,
`progression`, `tension`, `inTangent`, `outTangent` and `lockedTangents`. A point's
`value` is used at its exact tick; `outValue` applies immediately afterward and
starts interpolation toward the next point. The default out value equals value.
Progression is 0 discrete, 1 linear or 2 cubic Hermite; tension is 0..1. Tangents
use native value-per-tick units. Providing a tangent locks that node by default;
`lockedTangents: false` regenerates native tangents and rejects manual tangent
arguments. Replacing a point regenerates its tangents unless provided again.
Native interpolation can overshoot point bounds. An unconnected curve uses
LMMS's default 0..1 range. `automation.edit` changes progression and tension.

`automation.global.inspect`, `.point`, `.edit`, `.removePoint` and `.clear` operate
on the existing hidden global track, addressed by `parameter` alone. Positions
are song ticks. Inspecting an absent curve does not allocate one. Empty curves
evaluate to the parameter's stored manual value through this inspection method.
Active curves follow native playback semantics. This source saves/restores the
native hidden track in project XML and retains model IDs for empty connections.
Project clearing removes hidden targets from the preceding project. Existing
legacy format upgrades remain enabled; compatibility across project versions and
unmodified LMMS builds has not been established.

`note.detuning.inspect`, `.point`, `.edit`, `.removePoint` and `.clear` address
`track`, `clip`, `note` and optional `container`. They use the native note's
DetuningHelper, not another pitch engine. Positions are ticks relative to note
start and values are semitones in -60..60. Point settings match arrangement
curves. New native detuning uses linear interpolation. Inspection avoids creating
a lazy curve; interpolation editing requires an existing audible detuning change.
Clear returns to zero detuning. Native note cloning copies its detuning curve
independently. Full `project.inspect` snapshots include active note detuning and
arrangement/global curves. Routine agent observations provide bounded track headers
and counts; clip and note pages expose curve metadata without expanding points.

QtTest and process checks were authored for native interpolation, target lifetime,
serialization, recovery/undo, oscillator frequencies and independently decoded
automation renders. Additional QtTests cover owned instrument controls, named
choices, filtered discovery, discrete-value rejection, microtuning recovery,
actual decoded LFO values, failed decoding and waveform recovery. The process
harness also compares actual TripleOscillator waveform/arpeggio audio and
AudioFileProcessor gain/clearing, then checks resource restoration after restart.
Six additional native cases cover Scala parsing/exchange, signed/formal mappings,
unmapped automatic-length notes, active-note changes, tuning recovery/undo and
project replacement. The process harness measures rendered oscillator pitch
ratios, unmapped silence, Scala exchange, failure preservation and restart state.
They remain unrun without a compiled full LMMS executable.

## Session operations and durability

Session operations are handled by StudioRuntime itself:

| Operation | Arguments | Purpose |
| --- | --- | --- |
| `session.inspect` | `{}` | Observe revision, history cursor, stored memory, and configured providers |
| `session.memory.set` | `{"memory":{...}}` | Replace stored intent/preferences/context explicitly |
| `session.provider.set` | `{"role":"reasoning" or "audio","configuration":{...}}` | Save normalized, nonsensitive inference settings |
| `session.chat` | `{"text":"...","maxRounds":64,"maxOperations":512,"maximumWallTimeMs":1800000,"maximumCpuTimeMs":0,"maximumResidentMiB":0}` | Persist an intent and queue an autonomous job; budgets are optional and zero disables each resource budget |
| `conversation.history` | `{"offset":0,"limit":64}` | Read archived and recent dialogue in order; available to the agent, with page size 1â€“256 |
| `agent.inspect` | `{}` | Observe job status, plan, remaining decisions and counters |
| `agent.plan.set` | `{"goal":"...","steps":[...],"decisions":[...]}` | Persist a musical production plan |
| `agent.context.compact` | `{"summary":"original task, choices, unfinished work and useful evidence IDs"}` | Archive a completed inference exchange and continue the same job from its working summary |
| `agent.history.list` | `{"job":"optional committed job ID","offset":0,"limit":16}` | Discover jobs, or frozen contexts within a selected job |
| `agent.history.read` | `{"job":"committed job ID","context":0,"record":0,"offset":0,"limit":8192,"sha256":"optional digest from first chunk"}` | Retrieve a tool record; omit `record` for a frozen context, and omit both selectors for a whole archived job |
| `agent.cancel` | `{}` | Persist cancellation and abort cooperative inference/audio work |
| `agent.resume` | `{"maxRounds":128,"maxOperations":1024,"maximumCpuTimeMs":600000,"maximumResidentMiB":4096}` | Resume a paused/cancelled job; optional bounded budgets retain consumed counters, and zero disables a resource budget |
| `agent.finish` | `{"status":"complete" or "needs_input","summary":"...","evidence":["successful-call-id"]}` | Record a provider-declared task outcome with validated native result references |
| `session.undo` | Optional `summary` boolean | Restore the previous complete native project checkpoint |
| `session.redo` | Optional `summary` boolean | Restore the next complete native project checkpoint |
| `session.shutdown` | `{}` | Pause active work, acknowledge, then close the native session |

Each job transitions through queued/running and complete/paused/cancelled states.
Unsolicited `event: "agent"` messages report transitions alongside request-ID
responses; clients must handle both. Successful native operations commit their
checkpoint and the job's tool result/cursor in the same manifest. The round budget
is reserved before external inference. Defaults are 64 rounds, 512 operations and
30 minutes of accumulated execution; maxima are 1,000 rounds, 100,000 operations
and 24 hours. Budget exhaustion pauses work without erasing committed edits.
These remain cooperative execution limits. Optional CPU and resident-memory budgets
use the measurements below; they do not impose operating-system resource quotas.

`studio.resources.inspect` accepts `{}` and samples the running LMMS process
without changing the project or revision. It is also available to the model as
`studio_resources_inspect`. A fresh request ID obtains a fresh sample; replaying an
acknowledged ID returns that request's original sample. Each sample identifies
`scope: "lmms-process"`, process ID, a process-instance UUID, UTC observation time
and monotonic milliseconds since the sampler's initialization. It reports cumulative
`cpuUserMs` and `cpuSystemMs`, current `residentBytes`, process-lifetime
`peakResidentBytes`, measurement sources and errors. Unavailable counters are JSON
`null`, so missing measurements cannot appear as zero resource use.

Windows CPU totals use
[GetProcessTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes).
Current and peak working sets use dynamically resolved
[K32GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo),
without adding a PSAPI import-library requirement. Unix CPU totals use
[getrusage(RUSAGE_SELF)](https://man7.org/linux/man-pages/man2/getrusage.2.html).
Linux peak RSS converts `ru_maxrss` from KiB to bytes; current RSS uses
[/proc/self/statm](https://man7.org/linux/man-pages/man5/proc_pid_statm.5.html)
resident pages and is explicitly an estimate because that kernel counter can lag.
macOS resident sizes use Apple's
[MACH_TASK_BASIC_INFO fields](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/mach/task_info.h).
Other Unix platforms currently report CPU time with resident-memory counters
unavailable. Measurements cover all LMMS threads and exclude child plugin processes,
remote inference, system-wide consumption and GPU memory. Resident memory can
include shared pages; it is not private heap allocation.

Every reasoning request receives a fresh `processResources` observation and the
job's prior `jobResourceUsage`. Native tool transitions, completed inference rounds
and persisted inference failures accumulate optional `job.resourceUsage` in their
existing manifest transaction. It records transition count, CPU-measured transition
count, accumulated interval CPU and wall milliseconds, the maximum resident bytes
seen at interval endpoints, and the last sample/interval. The interval's
`cpuPercentOfOneCore` is CPU milliseconds divided by wall milliseconds times 100;
it can exceed 100 when multiple threads execute on multiple cores. Zero-duration
intervals have no CPU percentage. Different process identities, a restarted sample
clock, counter regression or missing CPU counters produce unavailable deltas.
Previously committed totals survive recovery, while new transitions measure only
their own before/after samples; they do not subtract counters from the old process.
Legacy jobs without this field remain supported, and malformed stored metrics
reject startup before native project replacement.

Job accounting covers the measured transition up to its final bookkeeping step.
It excludes idle gaps, final manifest/receipt publication, subsequent file cleanup,
and work interrupted before a safe accounting commit. CPU use from concurrent
playback or nested control work is part of the process interval. The process-lifetime
RSS peak can predate a job; the job's sampled maximum can miss short-lived peaks.
Counters are sampled sequentially, with operating-system granularity and collection
overhead. Numeric totals saturate at JSON's exact-integer limit and transition
counts at 2,147,483,646. This does not establish a speed/memory benchmark, enforce
allocation limits, contain an out-of-memory plugin or prove sufficient resources.

`StudioResourcesTest` authors six cases/24 executions covering real native CPU work,
missing/reset counters, multi-core percentages, restart-safe accumulation, original
receipt replay, provider observations, committed edits and rollback/failure/cancel
paths, and invalid stored measurements. The process harness captures resource
samples around its native conversational render/listen sequence and verifies
accounting in the resulting job and inference observations. No native metrics,
benchmarks or tests have been executed in the current workspace.

`session.chat` and `agent.resume` now accept `maximumCpuTimeMs` (integer 0â€“86,400,000)
and `maximumResidentMiB` (integer 0â€“1,048,576). Both default to zero, meaning disabled;
legacy jobs with absent fields also have those budgets disabled. A positive CPU
limit covers accumulated measured job CPU time plus the current transition's delta.
A positive resident-memory limit compares the current whole-process resident bytes
with the specified MiB threshold. It uses current sampled memory rather than the
process-lifetime peak. A value equal to the limit pauses execution. The agent sees
both limits as `resourceBudgets` in each reasoning observation; the model has no
tool to raise them itself.

The runtime checks enabled budgets before dispatching an inference or native action,
then through its existing cancellation predicates, sampling no more often than every
50 ms during cooperative work. A forced check at the native transaction boundary and
after provider generation catches short operations too. Once a violation is observed
it stays latched for that transition: late inference decisions are discarded, failed
native mutations restore the checkpoint, dependent calls are discarded after native
rollback, and the job becomes paused with a reason in `lastError`. Pausing before
dispatch retains untouched pending decisions. Session/agent inspection, fresh
`studio.resources.inspect` requests, explicit cancellation and shutdown remain
available while a job is executing; direct user requests are outside autonomous-job budgets.
The interruption bridge and mandatory rollback continue to use their existing paths.

Resume keeps measured consumption. Raising a CPU limit permits more measured work;
resuming with the same exhausted CPU limit pauses again before inference. Lower
resident use can permit a memory-paused job to continue, or the user can raise or
disable that budget. Unavailable measurements pause an enabled budget. An enabled
CPU budget also requires complete CPU accounting for all previously reserved rounds
and executed operations; legacy unmeasured work, missing prior counters or a round
interrupted before its accounting commit cannot be treated as free CPU time. Such
a job requires that CPU budget to be disabled or a new job started; its project and
history remain available. Invalid limits in input or persisted state are rejected.

Limits are cooperative thresholds and can overshoot between samples. In-process
plugin hangs, blocked kernel/driver calls and allocations that fail before the next
probe remain outside their control. Current Linux RSS remains an estimate. Final
manifest/receipt publication, subsequent cleanup, child plugin processes and remote
inference are excluded from the measured job budget. A completed external export
can remain on disk if the boundary check subsequently rejects its result; native
checkpoint rollback does not undo filesystem exports. These limits do not provide
out-of-memory containment, strict process deadlines or crash-proof CPU accounting.

`StudioResourceBudgetTest` authors eight cases/24 executions for input validation,
missing measurements, periodic/boundary cancellation, late decisions, native rollback
and dependent calls, nested cancellation, incomplete accounting, restart/resume and
invalid persisted budgets. Scripted samples exercise policy while the actual native
project is edited and restored. The process harness separately authors a real
resident-memory admission, explicit resume and restart scenario. These tests remain
uncompiled/unrun; no native budget enforcement or threshold accuracy is yet verified.

The global dialogue no longer needs to remain entirely inline. When it exceeds
128 recent messages or 256 KiB of serialized message data, commit archives older
messages into `conversations/<uuid>.json` chunks of at most 64 messages. The recent
window retains at most 64 messages after archiving and targets 256 KiB, while
keeping the newest message intact even if it alone exceeds that byte target.
Each reference records a contiguous absolute offset, count and SHA-256; each
archive is capped at 16 MiB. All message fields, roles, text, timestamps and job
associations are retained. Files publish before the manifest references them;
failed publication cannot acknowledge a new message. Orphan files are ignored.
Legacy inline transcripts migrate before native project replacement on startup.

`conversation.history` keeps absolute message offsets across the archived and
recent stores. Results include `messages`, `total`, `offset`, `nextOffset` and
`complete`. Limits are 1â€“256 messages, defaulting to 64. Pages target at most 1 MiB
of serialized messages and can be shorter than the requested count; one larger
message is returned intact. Follow `nextOffset` rather than assuming the requested
page size was returned. Existing archive and receipt size limits still apply to
very large individual messages. Offsets beyond the end return an empty, complete
page. Fresh reads verify each selected archive; startup verifies all references
and message structure before loading native state. Missing, changed, inconsistent
or cancelled reads return no partial page. Receipt replay retains its original
page and total, so use fresh request IDs to observe appended dialogue.

The provider receives the recent dialogue window and a notice when earlier
messages are archived. Its `conversation_history` tool can retrieve the older
dialogue without filesystem access. Compacted active tasks still start from their
original intent and working summary. `agent.inspect` reports the total conversation
message count and `archivedConversationMessages`. Provider context size and archive
disk usage remain separate concerns; reference lists, the active exchange, memory
and other metadata can still exhaust the manifest budget.

StudioTranscriptTest authors seven cases/18 executions covering exact ordered
retrieval across both stores and restart, byte/count retention, corrupted or
inconsistent archives, fresh reads versus receipt replay, failed migration and
publication, orphan archives after failed manifest commit, cancellation and
scripted chat/Responses retrieval of an old preference followed by a native tempo
edit. These tests remain uncompiled/unrun. Scripted retrieval does not establish
real provider recall or full musical production continuity.

The model has no tools for arbitrary code execution, provider changes or shutdown.
It can invoke native checkpoint undo/redo through the declared history tools. Plans, tool outputs and raw provider continuation are
persisted. Context replaced during explicit replanning is retained in `pastInputs`;
previous jobs are atomically archived under `jobs/` with SHA-256 references.
When `agent.resume` detects a changed project revision or reasoning provider,
each queued call receives a failed output in the old provider's protocol before
that exchange is archived. Original call IDs and arguments remain in history;
the same job cannot reuse these IDs or cite the skipped calls as successful
completion evidence. Completed calls retain their actual results and operation
counts. Resuming with the same revision and provider retains valid queued work.

Starting a new conversation while the previous job is paused or cancelled also
closes its pending calls with explicit skipped outcomes in the archived job.
Its previous status, plan, intent and completed results remain available, and its
native edits remain in the project. Use `agent.history.read` with the old job ID
and call ID to recover the original decision and reason it was not executed.
A new job has its own call-ID scope.

These outcomes become authoritative with the manifest referencing their context
or job archive. If archive creation fails, the acknowledged job retains its
pending decisions. If manifest publication fails, the session stops with the
last acknowledged manifest intact; unreferenced archive files may remain.
Job archives are validated and limited to the existing 16 MiB readable-file
budget before publication. Missing or altered archive files reject startup. Restart restores the native checkpoint,
increments the revision and invalidates pending decisions; their skipped outcomes
go back to reasoning, while committed edits are not executed again. Native failures
also skip the remainder of a dependent call batch for replanning.

Successful `session.undo`, `session.redo`, `project.new`, `project.load` and
`project.import` now end execution of that inference batch as well. A restored
track index can refer to a different native track, and an imported layout must be
observed before continuation. The actual successful operation result is retained.
Every remaining call, including planned reads, rendering, memory changes or
`agent.finish`, receives a failed tool output explaining that rediscovery is
required. These outputs preserve the original provider call IDs and order and are
committed atomically with the successful operation's checkpoint/history change.
The skipped calls are never dispatched, do not consume operation budget, and
cannot become successful completion evidence.

The same job continues automatically with a fresh bounded native observation.
Inspect the restored/imported contents and issue revised decisions with new call
IDs. Previously skipped IDs remain recorded and cannot be reused. Put project
creation/loading/import and history restoration last in a batch when other work
must run first. This rule applies after canonical operation resolution, including
native operations reached through `studio_execute`. Explicit client requests sent
later still use their normal revision and replay rules. Ordinary composition
batches remain sequential; this rule does not invalidate every batch after every
native edit.

The successful result and skipped outcomes survive restart and large-result
archiving together. If their manifest publication fails, the existing transaction
failure path rolls back the unacknowledged native project and stops the session;
the last acknowledged manifest keeps the original pending calls. Recovery handles
those calls under the existing restart invalidation rule.

Within a project, each queued call's existing native targets are also captured
before the preceding call executes and compared after it succeeds. If a selected
track, clip, note, mixer channel, effect, controller, parameter, plugin owner,
global automation clip, playback pattern or exact mixer route has been deleted or
now resolves to a different object, the remaining batch receives failed skipped
outputs in that same transaction. Both direct declarations and `studio_execute`
use this check. For example, removing track 0 cannot silently make a queued edit
of track 0 rename the former track 1. Inserting a note before note 0 likewise
invalidates an already queued edit of that old note index.

These are transient references to native lifetimes. QObject targets use QPointer;
notes use a lazy process-only identity whose lifetime survives comparison even
when a deleted note's address is reused. Native Note copies and clones get fresh
identities; assignment keeps the destination object's identity. Track/container
and destination selectors for clip copying and slice insertion are resolved by
their native meanings. Mixer move destinations are final slot numbers; route
`from`/`to` selectors identify channels. A route replaced by channel reset also
invalidates a queued operation selecting that exact existing route.

Previously unresolved targets may be created by preceding calls. Adding a track,
then its clip and notes can remain one batch; editing an unchanged target after
an unrelated removal can also proceed. Existing targets are checked again around
each dispatched call. Successful hosted-plugin removal or swapping ends the
batch unconditionally because PluginHost supplies no per-slot lifetime identity.

This does not make indices persistent identifiers or validate every plugin's
internal layout. Pagination offsets, native program/bank slots, tuning slots,
graph/expression keys and hosted parameter/patchbay internals still require
correct discovery. Selection checks do not refresh plugin controls or replace
the native operation's normal validation. Explicit later client requests retain
their revision/replay semantics. The checks and fixtures are source changes;
native execution and performance remain unverified.

A plain assistant text reply pauses the job because it supplies no explicit task
outcome. `agent.finish` validates cited successful call IDs, but the runtime cannot
prove semantic fulfillment or musical quality from those IDs. A job's `complete`
status is a provider declaration about that task, not completion of this commission.
There must still be independent native, musical and compatibility evidence.

`agent.context.compact` requires an active job, a nonempty current exchange and a
nonempty summary of at most 64 KiB. An agent must call it last in a batch; a direct
request cannot compact while native calls are pending. The runtime appends the
tool's successful output before archiving the exchange, so the archive retains
its complete call/result pair. The new context starts with the original intent,
the working summary in the native observation, current project state, the plan
and persistent memory. Job identity, successful evidence, operation/round
budgets and native project revision/history are preserved. Compaction consumes a
tool operation when invoked by the agent and does not declare task completion.

Prior exchanges are stored in `contexts/<uuid>.json`, with protocol, item count,
tool-record count and SHA-256 references in `pastInputs`. Provider/revision replanning also archives
the old exchange. Existing inline `pastInputs` remain readable. Opening a session
verifies referenced contexts in both the current job and completed job archives
before replacing native state. Failed archive publication leaves the previously
committed exchange intact and stops the runtime if job bookkeeping cannot commit;
unreferenced archive files cannot become current implicitly.

Agent observations report `inferenceHistoryBytes`, `retainedRecordBytes`,
`unarchivedToolRecordBytes` and a `recommendedCompactionBytes`
target of 256 KiB. This is an advisory JSON byte count, not a model token limit.
The compaction advice uses active input and unarchived tool records; already
compacted indexes and inference metadata are not repeatedly treated as removable
payload just because the total retained-record size remains large.
`agent.inspect` includes the current history/record sizes, past-context count and working
summary. Five native cases/15 executions cover chat/Responses continuation, restart,
old evidence, corrupt/missing/inconsistent archives, provider changes, legacy inline
contexts, replay, failed writes and pending-call protection. All are uncompiled/unrun.
Summary quality and real provider behavior are unverified. Active tool records,
compact evidence indexes, inference metadata, the chat transcript, retained
request receipts and archive references still occupy the
16 MiB manifest budget; disk quotas and archive cleanup remain unfinished. This does not provide unlimited
conversation storage or automatic context-window sizing.

`agent.history.list` with no `job` returns the current job first, followed by
archived jobs from newest to oldest. With a committed job ID, it lists frozen
contexts in order. Lists return `total`, `offset`, `nextOffset` and `complete`;
the default page is 16 items, with a permitted limit of 1â€“32. Intent and summary
previews contain at most 1,024 UTF-8 bytes. The current job exposes its past
contexts only. After a job is archived, its final nonempty input is also available
as the last context. A whole current job cannot be read, because a history tool
result would itself change that job. Metadata listing uses committed references;
selecting an archived job also verifies its archive.

With both `job` and `context`, `agent.history.list` pages the context's tool-record
metadata. Each entry has `record` (the native archive record index), `call` (the
original call ID), `name` (the original function name), recorded `ok`, and
`responseStorage` (`inline` or `toolResultArchive`). A recorded native revision,
a bounded `errorPreview`, or the stored payload's byte count may also be present.
Arguments and full responses are omitted. The default page is 16 records, maximum
32, with a 64 KiB limit on the returned records array. Follow `nextOffset` until
`complete`; escaping can make a page shorter than its requested limit. Error
previews contain at most 1,024 UTF-8 bytes and preserve character boundaries.

Use a discovered `call`, or `context` plus `record`, with `agent.history.read` to
retrieve exact arguments and results. This also works for the final exchange of
an archived job; context listing now reports its actual tool-record count.
Whole final-context reads retain their existing protocol/input-only bytes and
digest. Its records live in the job archive and are selected separately. Legacy
contexts without record arrays list zero records; no missing history is invented.

Record listing verifies the selected context/job archive and its committed
record index. It reports stored outcomes without opening separate result payloads;
reading a selected record verifies that payload too. Historical handles and
indices describe the old execution and must be rediscovered before new edits.
Metadata paging bounds the response, but reading/parsing a referenced JSON archive
still uses its complete file, up to 16 MiB. Index scans check cancellation and
resource budgets cooperatively. Cancelled listing returns no partial record page.

Compaction and provider/revision replanning now move all still-inline tool
records into the context archive's `records` array. These retain the exact
structured call arguments and responses, including failures and skipped calls.
Large responses may already have immutable payload references, described below;
record retrieval resolves those references to the original full record.
The manifest replaces each full tool record with
`{"kind":"archivedTool","id":"call-id","name":"function_name","ok":true,"context":0,"record":0}`.
Inference metadata remains inline. Call IDs stay reserved across compaction and
restart. `agent.finish` re-reads referenced archives and checks successful evidence
against these indexes; an unavailable, changed or mismatched archive cannot support
a newly acknowledged outcome. Multiple citations from one context share a verified
read within that request. Full context payloads are released after verification;
the runtime retains only a set of verified context indices across citations.
Startup verifies indexes in both current and archived
jobs before replacing native state. Failed archive writes preserve the previously
committed full records and pending call.

Context discovery includes `recordItems`. Supply both `context` and `record` to
`agent.history.read` to obtain one archived tool record without returning the
rest of the exchange. The record is serialized as compact JSON, with its own
content digest and the same paging rules. The whole parent context is verified
first. Existing contexts without records and jobs with full inline records remain
readable; their context entries report zero archived records. Compaction does not
truncate payloads or replace them with an agent-generated summary. It also does
not automatically bound the active exchange, evidence-index count or archive disk
usage. Retained request receipts preserve their own exact copy of results; large
copies are stored outside the manifest as described below.

Large native tool outputs are also archived during normal session commits,
including while a provider batch still has pending calls. A response larger than
256 KiB moves into an immutable `tool-results/<uuid>.json` file containing the
complete original tool record: call ID, name, exact arguments and native response.
If active inference input plus tool records exceed 4 MiB, older responses above
8 KiB also move. The latest response stays inline when it is at most 256 KiB, so a
fresh history chunk remains directly usable. These thresholds measure encoded
JSON bytes; they do not impose a native parameter or project-size limit.

The retained tool record adds an `archive` reference with path, byte length and
SHA-256, and replaces its response with a small projection. The corresponding
provider tool-output message receives the same projection, preserving its call
ID and position. It contains the native `ok`, request ID and revision where
present, an optional error preview of at most 1,024 UTF-8 bytes, and:

```json
{"storedResult":{"job":"committed-job-id","call":"provider-call-id","sha256":"payload-digest","bytes":300000,"readWith":"agent_history_read"}}
```

Preview construction caps UTF-16 input before UTF-8 conversion. One extra code
unit preserves a surrogate pair crossing that cap before the byte prefix is cut
at a complete character. Oversized UTF-16 preview fields are rejected before
encoding during stored-record validation. This bounds the temporary conversion;
the full original error and archive still use the existing result storage limits.
Seven added native fixture rows cover ASCII, large errors and Unicode cutoff
boundaries, including retained archive identity and malformed previews. These
fixtures have not been compiled or run.

`storedResult` identifies an exact stored result; it does not contain the full
operation result. `agent.history.read` accepts `job` and `call` to read that
original tool record, including while its job is current. The `call` selector is
mutually exclusive with `context` and `record`. Existing chunk limits, UTF-8
boundaries, digest continuations and `nextOffset` apply. Call selection continues
to work after explicit compaction, provider replanning and job archiving. Selecting
a stored record by context/record also resolves its original payload. Whole
context/job reads preserve their stored reference structure.

Startup verifies referenced tool payloads in the current job and frozen contexts
of current or archived jobs before replacing native project state. New history
reads and completion-evidence checks verify the payload's complete bytes, digest,
call arguments, outcome and retained projection. Nested payload references are
refused. Previously acknowledged receipt replays retain their existing semantics;
they are not a fresh validation of an external file. Legacy inline records remain
readable and may migrate before continued work. Semantically equivalent JSON
formatting in an old provider output is accepted during migration.

Inline tool records and all their matching provider outputs are checked before
any payload file is published. Both provider message formats use an index of
output locations, including duplicate outputs. A late mismatch or malformed
output therefore refuses the batch before archive creation and native startup
replacement. Previously accepted small inline mismatches are also rejected.

New tool-result filenames use the first 128 bits of the serialized record's
SHA-256 in the existing UUID-shaped path format. The full SHA-256 stays in the
reference. An identical record can reuse that file after checked reading,
full-digest verification and exact byte/object comparison. A conflicting,
unreadable or symbolic-link target is refused without overwrite. Existing random
UUID references keep their read behavior. This reuses identical publication;
it does not delete old orphans or reclaim any acknowledged history. Mandatory
publication and reuse verification suspend operation callbacks and Qt event
polling. These synchronous operations still have no hard I/O deadline.

The new batch/refusal fixtures cover both protocols, changed values, invalid JSON
and conflicting duplicate replies before publication. Collision fixtures retain
the target file, previous manifest and current local project. Existing legacy
migration fixtures now require the same archive path, unchanged modification time
and unchanged file count when reusing identical bytes. StudioToolResultTest now
authors 14 cases / 50 scenarios; all remain uncompiled and unrun.

Publication and reads for `conversations`, `tool-results`, `receipts`, `jobs` and
`contexts` now share an owned-path check. Each path must be an exact family plus
lowercase UUID and `.json`, under the canonical session root captured after
locking. The current root must still resolve to that same canonical location.
An explicitly selected root symlink is accepted while that location stays the
same. Archive subdirectories must be real immediate directories under the root;
symlinks and, with Qt 5.15 or later, junctions are refused before creating or
opening payloads. Existing payload targets must be regular files at their owned
canonical path, without symlink/junction redirection. A linked directory is
refused even if its external payload has exactly the expected bytes and digest.
Previously followed archive-directory redirects now fail explicitly. Ordinary
random UUID references and inline records retain their existing behavior.

Startup validates referenced archives before native replacement. Fresh history
reads and archived receipt replays recheck ownership before returning payloads.
A refused job publication during request preparation may durably acknowledge an
error receipt while preserving the previous job/project. Receipt, transcript or
tool publication failure at commit, and context publication failure during agent
bookkeeping, retain the acknowledged manifest and stop the runtime through their
existing failure paths. The guard neither scans directories nor derives ownership
from their contents.

StudioArchivePathTest authors six cases / 44 POSIX scenarios, with four cases /
16 scenarios available on Windows. All five families cover replaced targets,
startup preservation, fresh-read refusal and obstructed publication. POSIX rows
also cover parent/file links, selected and retargeted root links, and deterministic
tool migration against a directory or linked target. All remain uncompiled and
unrun; the Windows junction branch has no executed or authored junction fixture.

Canonical text checks do not pin handles/inodes or prevent concurrent external
replacement, hard-link aliases or adversarial filesystem races. This shared guard
covers these five archive families; manifest, checkpoint, media and lock I/O have
their own existing paths and checks. It does not establish universal filesystem
containment or change buffering, hard deadlines, disk quotas or orphan cleanup.

Payload publication precedes the session manifest commit. A publication failure
at the commit boundary leaves the last acknowledged manifest intact; an uncommitted native mutation
uses the existing mandatory rollback and stops the failed runtime. Neither
archiving nor result retrieval executes the original operation. Pending work,
job identity, evidence IDs, budgets and native history remain under the same
session transaction. No task summary or completion is inferred from archiving.

This bounds individual active response copies and relieves accumulated result
payloads. Each stored record is still limited to 16 MiB and is fully buffered for
writing and verified reads. Calls/arguments, project observations, reasoning,
small outputs, metadata/index counts and other session fields can still grow.
Explicit context compaction remains necessary; automatic model-context sizing,
disk quotas and orphan-result cleanup remain unimplemented.

`agent.history.read` selects only committed job IDs and context indices, never a
caller-supplied file path. A fresh request verifies the selected file's complete
SHA-256, UTF-8 and JSON structure before returning content, along with job identity
and context protocol/item count where applicable. A context within an archived
job verifies both the parent job and selected context. Whole-job reads validate
child references but do not re-read every child file; startup verifies all of them.
Legacy inline contexts and final job input are serialized as compact JSON. File
archives retain their exact stored bytes. Reads scan and buffer each selected
file, capped at 16 MiB; paging bounds returned content, not total I/O per request.

The result includes `job`, `context` (`-1` for a whole job), `record` (`-1` when no
tool record was selected), `protocol` (empty for a
whole job), `encoding: "UTF-8 JSON"`, `sha256`, `totalBytes`, `offset`, `nextOffset`,
`complete` and `text`. Offset and limit count UTF-8 bytes, not characters. Limits
range from 4 to 65,536 bytes, defaulting to 8,192. Offsets inside a character are
rejected; the chunk end is shortened to a character boundary. Concatenate `text`
chunks, preserving the returned digest and following `nextOffset`. Passing that
digest as `sha256` binds subsequent chunks to the same content. An offset at the
end returns empty text and `complete: true`. The entire enclosing JSON response
can be larger than the text limit because it includes metadata and JSON escaping.

History reads preserve native project revision/history and remain subject to
normal agent operation/time budgets and request receipts. Replaying an acknowledged
request returns its cached result; a fresh ID revalidates the archive. Missing,
changed, inconsistent or cancelled reads return no partial content. Cancellation
is cooperative between 64 KiB file transfers. The agent is instructed to treat
historical content as earlier evidence and inspect current native state before
reusing it. Retrieval quality still depends on provider reasoning.

Five additional native cases/12 executions cover exact Unicode-safe reconstruction,
legacy inline contexts, final input, restart/replay, changed/missing parent and
context archives, invalid selectors, cancellation, and scripted chat/Responses
retrieval followed by a native tempo edit. Five further cases/18 executions cover
large tool-payload preservation, evidence after restart, inconsistent indexes in
current and old jobs, missing/changed evidence at finish, reused call IDs and
legacy contexts without record archives. The existing failed-write case now also
checks retention of a previously successful native edit and its full record.
StudioContextTest now has fifteen cases/45 executions; none are compiled or run.
The scripted provider does not establish
real model reasoning, musical judgment or complete long-horizon continuity.

Each project mutation saves an immutable native `.mmp`, checks its SHA-256 digest,
then atomically publishes `session.json`. Responses are acknowledged only after
the session commit. Failed native edits are rolled back to the prior checkpoint.
An atomic commit failure stops further requests. Recovery follows the committed
manifest, so an orphan checkpoint never becomes the current project implicitly.
Corrupt/unsupported manifests, invalid cached request records or checkpoint references,
and altered active checkpoints are rejected without
silently replacing the existing session. QLockFile enforces a single writer.

Within one process, only one StudioRuntime may own LMMS's global engine. Ownership
is reserved before session initialization and retained until the runtime object
is destroyed, including after a shutdown request. Another session cannot open
against a different directory and replace that owner's project. Failed opens
release engine ownership; destroying a failed runtime cannot release a later
owner's reservation. The directory lock still protects against other processes.
Opening sessions, processing requests, advancing agents and running the protocol
loop require the application thread. Rejected foreign-thread requests do not read
session revision or write receipts. Callers must keep runtime and engine lifetimes
valid for every active call; these checks do not make concurrent destruction safe.

Checkpoint restoration converts standard and nonstandard returned C++ exceptions
into explicit failure. A failed request discards tentative manifest changes; when
rollback succeeds, its failure receipt and any agent cursor changes commit with
the prior history/cursor and an increased revision. A rollback failure stops the
session and retains the last acknowledged manifest. Commit/bookkeeping failures
stop requests before attempting rollback, including nested requests from native
callbacks. Both the operation and recovery errors are retained when available.
Startup failures preserve an existing manifest and leave the session unopened.

Inference exceptions persist elapsed time and pause the active job, preserving an
explicit cancellation if one occurred during inference. Resume can continue from
that durable state. Failed native agent edits discard dependent pending calls and
require replanning against the restored revision. Eight native cases/25 executions
cover these paths, including direct synchronous callback exceptions and failed
manifest writes and subsequent native undo after failed model initialization;
they are authored but uncompiled/unrun. They do not establish
recovery from hung processors, process faults, corrupt memory, throwing destructors,
arbitrary Qt event-loop exceptions, allocation failure while reporting an error,
or private plugin state absent from native project serialization.

The manifest retains 256 project checkpoints in revision history and 128 request
receipts or references to them. Receipts include exact inputs and responses.
Reusing a retained request ID with the
same content replays its response without another execution; different content
is rejected. IDs of executing requests are also reserved until their calls unwind.
A nested allowed control request cannot take the same ID before the outer receipt
exists; that rejection is transient and is not stored as the pending request's
result. Distinct-ID observation, cancellation and shutdown remain available while
native work yields to callbacks. Five native cases/15 executions cover nested ID
collisions, cancellation/shutdown through rollback and restart, foreign-thread
entry and exclusive engine ownership. All remain uncompiled/unrun.

Receipts whose compact JSON exceeds 8 KiB are published atomically under
`receipts/<uuid>.json` before the manifest references them. Each reference stores
the request ID, request digest, archive path, archive SHA-256 and exact byte count.
Smaller receipts remain inline. With at most 128 retained receipts, their inline
payload contribution is bounded to roughly 1 MiB plus reference/array overhead;
large responses no longer consume the full manifest budget just through replay
storage. One archived receipt may contain at most 16 MiB. The original response
is returned intact, and native project or agent state is acknowledged only after
the manifest commits. A receipt-publication or manifest-commit failure stops the
session and rolls back tentative native edits. Published but unreferenced files
cannot acknowledge an operation or become current after recovery.

Startup verifies every referenced receipt's bytes, UTF-8/JSON, digest, request and
response IDs, request hash and response revision before replacing the project.
Older large inline receipts migrate before native replacement; a migration write
failure preserves the current native project and existing manifest. Every replay
also verifies its receipt archive. Missing, altered, inconsistent or cancelled
reads return no cached content and never fall through to re-executing that ID.
After a failed read, a retry can succeed once the receipt is readable again.
Replay checks its own receipt rather than rerunning the original tool or
revalidating resources that tool previously observed.

Request IDs are reserved before receipt reads, so nested controls cannot claim a
replaying ID. Reads cooperate with cancellation and shutdown between 64 KiB chunks;
publication is synchronous and does not poll the event loop between receipt and
manifest writes. Each read buffers at most one receipt archive. The 128-ID replay
window is unchanged: expired IDs can execute again, and orphan or expired files
are not consulted. Committed receipts leaving that window now enter the native
file retirement queue described below. Unacknowledged orphan cleanup and total
disk quotas remain unfinished.

StudioReceiptTest authors nine cases/22 executions for large inputs and responses,
restart/replay without repeating native edits, malformed/missing/altered archives,
rejected fresh replay, native rollback on publication failure, unacknowledged
orphan receipts after manifest failure, legacy migration and its failure before
native replacement, nested cancellation/shutdown, interrupted replay and bounded
receipt retention. These native tests remain uncompiled/unrun.

This deduplication window is bounded, and a replayed response carries
the original revision rather than the current revision. Use fresh IDs for new
observations. Revision numbers increase on successful project mutations and on
rollback/restores and session reopening, since restored native parameter objects need rediscovery.
An edit after undo truncates the redo branch. Checkpoints leaving that branch or
the 256-entry history window now enter the same native file retirement queue as
expired request receipts.

The manifest's optional `retiredFiles` array contains `{path, sha256}` records.
Only references removed from the previously committed history or receipt ledger
are added. The new manifest publishes the updated references and retirement queue
together before any file is removed. Failed checkpointing, rollback or manifest
publication therefore cannot delete the old undo/redo branch. Existing sessions
without a queue remain supported. Startup rejects malformed paths, duplicate
records, invalid digests, overlap with retained files, and queues above 4096 entries
before replacing the native project. Reaching that queue limit rejects further
retirement through the existing failed-commit path instead of dropping records.

After a successful commit, cleanup examines up to sixteen queued files, rotating
through deferred entries across commits. It only accepts the exact native
`checkpoints/<uuid>.mmp` and `receipts/<uuid>.json` namespaces. It pins the session's
canonical directory when acquiring its lock, verifies the immediate parent remains
inside it, rejects links/non-files, hashes the contents in cancellable 64 KiB chunks,
and checks containment, size and modification time again before removing the file.
Windows junctions receive an explicit check where Qt exposes `isJunction`.
Canonical paths and link metadata follow
[QFileInfo's documented behavior](https://doc.qt.io/archives/qt-5.15/qfileinfo.html).
Manifest publication uses the existing
[QSaveFile commit path](https://doc.qt.io/archives/qt-5.15/qsavefile.html).

Changed, inaccessible or redirected files stay queued. Missing files count as
already collected. Successfully collected records are pruned by the next manifest
commit. A restart between publication and removal retries cleanup; a restart after
removal but before pruning recognizes the missing file. Cleanup does not dispatch
Qt events or inherit the enclosing operation's time budget, and its failure cannot
reverse an already published operation. Process interruption can defer cleanup.
`session.inspect` and `ready` include `storage.retiredFileReferences`,
`storage.retirementLimit` and `storage.lastCleanup` (attempts, removed bytes/files,
already-absent files, pending count and individual errors). The report describes
the preceding cleanup pass; a receipt replay returns its original report.

This queue manages only known, previously committed checkpoint and receipt files.
It does not scan directories to infer ownership. Source media, checkpoint assets,
exports, jobs, contexts, conversations, files orphaned before a successful manifest
commit and old untracked files remain retained. There is no disk-byte quota or hard
I/O deadline. Cleanup hashing is synchronous, so slow storage can delay a response.
The exclusive runtime/session lock prevents competing studio writers; the path
checks are not descriptor-relative protection against concurrent external directory
replacement. Filesystem race resistance, power-loss durability, Windows junction
behavior and storage/latency measurements remain unverified.

`StudioStorageTest` authors eleven cases (22 POSIX or 20 Windows executions) for
redo-branch and history expiry, receipt replay/expiry, failed publication,
interruption/restart, invalid queues, altered files, bounded rotating retries,
path/link rejection, queue exhaustion and cancellation without nested Qt dispatch.
`StudioProcessTest` adds an executable-level retirement/restart/undo/redo case.
These tests have not been compiled or run in this workspace.

Known native resource attributes (sample clips, AFP, SlicerT, SF2, GIG, Patman,
TripleOscillator and LFO wave files) using `local:` paths are resolved against the original
project before writing a checkpoint. New checkpoints copy declared media into
`checkpoints/assets/<sha256>.<extension>` and use native `local:assets/` references.
Their manifest entries include an `assets` array of relative paths, SHA-256 values
and exact byte sizes. GIG groups instead use `assets/<group-sha256>/<root-name>`;
their root entry also contains `members`, an ordered array of `{name, sha256,
bytes}` objects. The root comes first and companions follow in case-sensitive
filename order. The group digest includes root identity, each UTF-8 filename,
byte count and SHA-256; root equality alone does not deduplicate a bank. Equal
single-file bytes with equal extensions share a file across
revisions. Native project references must exactly match the declared asset set;
file digests and sizes are checked before restore and before native mutation.
Invalid assets are rejected before replacing the current Song. Legacy checkpoints
without an assets array remain readable and retain external dependencies.
Checkpointing does not prove every plugin's resource handling or preserve missing
inputs when its serializer has no accepted-source adapter.

`project.save` with `withResources: true` uses LMMS's native project bundle writer
and returns the actual project file inside the bundle directory. Bundle copying
skips empty resource attributes, reuses identical source paths, resolves basename
collisions including extensionless files, and rebases XML only after all copies
succeed. A DataFile loaded from disk resolves its own `local:` resources even when
another project is open. Native serializers can bind accepted source bytes to
individual resource nodes within a resource-save scope. SF2 binds its private
snapshot. SampleBuffer captures a private copy before the existing native decoder
runs and keeps that copy with the immutable decoded buffer. Sample clips, AFP,
SlicerT, TripleOscillator and LFO user waves bind these accepted inputs during
serialization. Thus bundles and checkpoints preserve each loaded version even
when different consumers loaded different bytes from the same original filename,
or that original changes or disappears. Explicit bundles retain the original
basename; checkpoints use content hashes. Plain project saving
retains original path references. This is source behavior with native tests
authored but unrun. Patman also binds its accepted patch input. GIG and other file-backed plugins still need accepted-input adapters,
and quotas, full accepted-input coverage and complete failed-bundle
publication/cleanup remain required. Shared checkpoint storage and integrity
records are implemented in source but native verification remains unrun. Checkpoint
assets are not garbage-collected yet; failed commits can leave orphan files, and
hashing/staging still adds synchronous disk work. Power-loss durability and
resource/cancellation limits require further work and actual execution.

Snapshot capture streams through 64 KiB storage and closes the actual file
descriptor before native loading. It records SHA-256 and byte size from that stream
and verifies the completed private file before returning it to the loader.
SampleBuffer, SF2 and Patman serializers retain a snapshot lease with the resource
node. Checkpoint staging compares the actual copied stream with the capture-time
fingerprint before publishing; bundles verify copied files against it. Reused
copies are also checked. A same-size change to the private snapshot therefore
cannot silently become the accepted input of a new saved revision. Failed copies
leave native XML references unchanged; whole-bundle cleanup remains incomplete.
SF2 verifies its snapshot before preparing a replacement synth after a sample-rate
change. These paths and corruption/rollback tests are implemented but native
execution remains unrun. Worker-thread releases defer temporary-file
cleanup to the application thread while its event loop runs. Deferred cleanup
is also held in an intrusive queue. After native audio workers/voices stop,
Engine.destroy drains released snapshots on the application thread, including
when its event loop has exited. Active snapshot owners remain intact. Actual
shutdown, late worker releases and crash cleanup still need native verification
and further lifecycle handling as applicable. Capture adds synchronous disk work and a temporary copy per loaded
input; disk quotas, hard I/O cancellation and repeated-load costs remain unfinished.

Native asset I/O now uses shared 64 KiB transfer/hash loops with cooperative
checks before operations, between chunks and before finalization/publication.
Studio requests provide their cancellation/job-time callback. Application-thread
event polling is throttled and excludes desktop input events, allowing queued
studio inspection/cancellation/shutdown while the existing busy-request guard
rejects concurrent edits. Nested scopes inherit earlier stop conditions; render
startup adds its own deadline. Ordinary GUI calls without a scope retain their
existing execution policy. Partial copies created by the helper are removed on
failure; existing destination files are never overwritten. Mandatory native
rollback and audio-device restoration suspend the cancelled I/O scope, so a stop
request cannot prevent required restoration. These paths have authored native
tests but remain unrun. Checks cannot interrupt blocked OS calls, native decoder/
plugin work or XML parsing, and Qt callback duration is not a hard timing bound.
Disk/memory quotas, actual responsiveness/resource measurements, filesystem races
and complete failed-operation cleanup remain required.

GIG source now captures the root before inspecting its `xfil` extension filenames
and `doxf` declaration for the root-stem `.gx99` companion. The accepted private
directory preserves the original root name and all declared relative member
names. Companion `dlid` identifiers must match the root declaration before
libgig opens the group; this also prevents a missing ID from shifting libgig's
extension-table parsing onto uncaptured names. Native libgig/RIFF readers close
before their snapshot directory owner is released. Serializers bind that owner
for bundles and checkpoints; a standalone DataFile without a binding discovers
the same dependency group. Group directories are staged, verified and renamed
into their digest location, and restore verifies every member before mutation.
Bundles keep the same layout under `resources/<group-sha256>/`.

This source path accepts complete RIFF/RIFX roots and companions, choosing each
file's 32/64-bit chunk-length width at 4 GiB to match libgig's automatic mode.
ID fields are compared across each member's own byte order. It allows up to 4096
`xfil` declarations, an optional `.gx99`, and 100000 top
level chunks per file. Absolute paths, traversal, backslashes, invalid UTF-8,
portable filename hazards, duplicate names including case aliases, missing or
mismatched IDs and incomplete RIFF layouts fail before publication. Non-automatic
offset layouts and non-UTF-8 legacy filenames remain unsupported by this capture
path. Seven new native cases/23 executions use structural metadata fixtures;
they are unrun and do not contain playable instruments. Three additional native
runtime cases/11 executions now use libgig-written 16/24-bit mono/stereo PCM,
single-file banks and a small split wave pool. They check accepted-bank playback
after original deletion, native bundle reload, distinct companion versions,
undo/redo, relocated-session reopening and failed replacement. The split fixture
moves the native wave pool unchanged and points its native wave table at the
declared companion; it does not simulate loading or synthesis. Those cases are
conditional on libgig and the built native plugin and remain unrun.

GIG playback source now uses an aligned, fixed, zeroed scratch block, bounded
loop reads, scoped synth/note locks and a per-instance libgig decompression
buffer. This avoids using libgig's shared default decompression buffer across
different instrument workers. PCM decoding preserves the existing half-scale
gain, reads native-order 16-bit words without alignment assumptions and extends
24-bit little-endian values with unsigned arithmetic. A stalled resampler no
longer clears other voices already mixed into the period. Nonempty referenced samples with
unsupported channel/depth/frame shapes or zero sample rate, and empty/out-of-range
first loops, are rejected before replacing the instrument. Null dimension-region
sample selection is handled without dereferencing it. Three decoder cases/13
executions cover signed endpoints, mono/stereo independence, unaligned input,
short buffers and invalid shapes/gain; all are unrun.

Valid GIG loading, PCM,
multi-file bank relocation, full session recovery and format compatibility still
need native verification, including the 64-bit large-file branches, compressed
parallel streams and loop modes. Existing backward/bidirectional loop behavior,
multiple-loop support, sampler dimensions/layers, streaming failures and hard
real-time/resource bounds remain incomplete or unverified. Native read exceptions
now release the local locks; complete engine/render fault recovery still needs
work. Disk quotas, multi-file source consistency under
external edits and filesystem races also remain unresolved. Declaration behavior
was checked against [upstream libgig source](https://svn.linuxsampler.org/cgi-bin/viewvc.cgi/libgig/trunk/src/gig.cpp)
and [RIFF source](https://download.linuxsampler.org/doc/libgig/api/RIFF_8cpp_source.html).

Input is limited to 1 MiB per request and session metadata to 16 MiB. Native stdin
polling keeps Qt events running while idle and reads bounded frames from pipes,
files or consoles. Queued execution keeps polling available during nested render
and network event loops. During an operation only inspection, cancellation and
shutdown requests can execute. New intent requires the active job to finish or
be cancelled. A manifest-size failure stops further work; active context sizing,
archive cleanup and storage quotas remain needed for long sessions despite automatic
archiving of older dialogue and large request receipts.
Studio mode now installs scoped handlers for POSIX `SIGINT`/`SIGTERM` and Windows
console Ctrl+C/Ctrl+Break before engine initialization. The handler only sets a
lock-free atomic flag. The application thread observes it in the existing input
and agent timers, stops dispatching new work, and requests the same durable
`session.shutdown` transition used by the protocol. An active job becomes paused;
the runtime emits one JSON object with `event: "shutdown"`,
`reason: "process interrupt"` and the shutdown response's `ok`, `id` and `revision`.
The event acknowledges the metadata transition; exit waits for active cooperative
work to unwind and for native engine teardown. Inference, listening, rendering and
scoped resource reads see the interrupt through their cancellation callbacks.
Mandatory native rollback suspends cancellation so an interrupted edit can restore
its last checkpoint. Late provider decisions cannot execute after cancellation.

Successful shutdown returns zero. A failed shutdown commit stops the runtime and
returns nonzero without acknowledging persistence; its last acknowledged manifest
remains authoritative. An interrupt already present before session startup returns
nonzero without opening the session. A startup interrupt during cancellable file
work follows that operation's existing error/recovery path. Repeated interrupts
coalesce into the same flag; they do not escalate to forced termination. The scoped
handler survives engine teardown and restores the previous POSIX handlers (or
removes the Windows callback) when the studio branch ends.

This follows the [C++ signal-safety rules](https://eel.is/c++draft/support.signal)
for plain lock-free atomic operations. Windows console callbacks run through
[SetConsoleCtrlHandler](https://learn.microsoft.com/en-us/windows/console/setconsolectrlhandler);
the callback handles only Ctrl+C and Ctrl+Break. Console close, logoff, OS shutdown,
forced process termination and crashes have no graceful-shutdown guarantee.
An inherited Ctrl+C-ignore setting can prevent Ctrl+C delivery. The process harness
uses a separate child process group and Ctrl+Break because
[GenerateConsoleCtrlEvent](https://learn.microsoft.com/en-us/windows/console/generateconsolectrlevent)
cannot target Ctrl+C to one process group; Windows signal tests require a shared
console and never broadcast to the test runner. Blocked initialization, kernel I/O,
in-process plugin calls, driver teardown and external replacement of the handlers
can still prevent timely exit. This is cooperative shutdown, with no hard deadline.

`StudioShutdownTest` has six cases (ten POSIX or nine Windows executions) authored
for handler lifetime/restoration, early interruption, idle/queued shutdown, late
chat/Responses decisions, interrupted edit rollback including nested shutdown, and
failed metadata publication. `StudioProcessTest` adds actual signal delivery while
idle and during held inference, verifies exit before releasing the provider, and
reopens the native project to check committed tempo and paused work. Those are four
POSIX or two Windows process cases. All remain uncompiled/unrun in this workspace;
source diagnostics do not prove signal delivery, cancellation or teardown behavior.

## Native MIDI file import

`project.import` selects installed native import filters and defaults to
`container: "song"`. For MIDI import into the PatternStore, pass
`container: "pattern"` and optionally `pattern`, an existing column index. Omit
the index to use the current column; an empty store accepts column 0 and creates
its first native Song pattern. Explicit destinations retain the current selection.
Supplying `pattern` with a song destination or an out-of-range index is rejected.
Pattern dispatch requires a filter implementing that destination: the existing
Hydrogen song importer is not invoked for a PatternStore request. The application
and import plugins must be rebuilt for the extended native filter interface.

```json
{"id":"import-pattern","op":"project.import","args":{"path":"phrase.mid","container":"pattern","pattern":2,"summary":true}}
```

The MIDI
filter accepts plain SMF and RIFF/RMID. It now reads in 64 KiB chunks, checks
container and event framing before creating models, and passes only the embedded
SMF payload to PortSMF. The old RIFF path found its data chunk and then restarted
reading at the outer RIFF header. PortSMF remains the musical decoder; the new
reader does not replace its note, tempo or channel interpretation.

Admission checks cover declared chunk/track extents, header/division fields,
four-byte variable-length quantities, running status, channel data bytes, complete
end-of-track framing and fixed-length meta events. RIFF padding and unrelated
neighbor chunks are honored. Unknown SMF chunks/meta events remain available to
the parser. Unrelated file formats return control to other filters. Recognized
malformed data, zero tempo, unrepresentable denominator exponents and incomplete
input fail before native track creation. This intentionally rejects malformed
files the previous path could report as successfully imported.

Native import requires the model owner thread. It holds the engine model guard
while constructing tracks, suspends FileResourceIO event polling, and no longer
pumps events from channel/controller construction. The progress dialog is
nonmodal so setValue does not dispatch events inside partial model state. The
parser sequence has RAII ownership and its read error is checked. Cooperative
budget/cancellation checks run during reading, framing, conversion, note copying
and before success. If construction fails, newly appended tracks in the destination
and Song are removed; existing tracks and their notes are retained. Import can
then be retried with the same filter object. PatternStore rollback retires new
rows before an automatically created Song pattern column.

Each source track/logical channel now owns its instrument and automation state.
Controller curves no longer redirect a second channel's values into the first
channel's model, and program curves no longer share an object across source
tracks. Bank MSB/LSB events update one combined native bank curve. The SF2 path
verifies the loaded plugin and its bank/program models before treating a fallback
instrument as a SoundFont player. Notes round absolute endpoints to native ticks,
with a one-tick minimum for collapsed short notes; velocities round to native
volume. Finite values and native timeline headroom are checked. Tempo and meter
points bypass editor quantization. Pitch bend uses the native semitone range.
Clip splitting retains the longest outstanding note end, preserving sustained
polyphony across shorter overlapping notes. Unmapped events produce one aggregate
diagnostic rather than unbounded per-event logging.

Pattern imports use the cell supplied by native track creation in the selected
column. Each imported channel receives one instrument row containing the entire
local sequence; song clip splitting does not run in this destination. Controller,
program, bank, tempo and meter curves use corresponding selected automation cells
and pattern-local ticks, including time-zero controller/program resets. Other
columns remain unconnected and existing rows retain their content. Existing sparse
rows are rejected before construction because native MIDI-cell initialization can
otherwise fill their missing cells. The destination's native identity is retained
across the GUI SoundFont advisory and checked again under the engine guard.

Imported MIDI cells retain a fixed whole-bar length derived from the decoder's
sequence duration, note ends and event/automation positions. Decoder duration is
used to preserve end-of-track trailing silence; behavior against the pinned parser
remains unverified. A metadata-only sequence creates an empty instrument row named
`MIDI sequence length`, carrying its period through the native PatternStore length
calculation. No instrument is loaded for this row. Imported automation geometry
follows the final shared period, including longer existing instrument cells.
Representability checks include the destination column position. Pattern selector
identity participates in queued-call validation, and successful imports retain
the runtime's durable history, replay and agent-batch invalidation behavior.

`MidiImportTest` compiles the production importer, framing reader and pinned
PortSMF sources. Twenty-seven cases / 91 authored scenarios cover exact plain/RIFF
payloads, odd padding, running status, SysEx/escape framing, extended/unknown
chunks, SMPTE header admission, malformed inputs preserving existing notes,
independent channel/program/bank automation, pitch bend, timing/velocity rounding,
unquantized tempo/meter, sustained polyphony, overflow rollback, cancellation
before and during construction, retries, owner-thread admission and event
reentry. Pattern fixtures cover empty/current/explicit destinations, index 256,
7/8 columns, unchanged existing cells, local automation, separated note material,
trailing silence, automation-only periods, a longer existing period, cancellation
and retry, save/reload, selector identity and structured undo/replay/recovery.
Three SoundFont cases skip if the native plugin is unavailable; structured import
also requires the built filter. Four added StudioAgentRestoreTest rows exercise
pattern import through both provider protocols and both native tool routes, with
stale calls discarded and subsequent edits based on fresh observation. The entire
suite is uncompiled and unrun here; the PortSMF submodule remains uninitialized.

Generic imported CCs now activate the instrument track's native MIDI CC rack.
Volume, pan, bank and pitch bend retain their existing native model mapping and
do not activate that rack by themselves. A used generic controller is registered
even when its initial value is zero. Controllers with a later first event receive
SoundFont-compatible initial values for balance, expression, sound controllers
and RPN/NRPN null selections. Unused initial values do not activate knobs. These
values follow the [FluidSynth controller initialization source](https://github.com/FluidSynth/fluidsynth/blob/master/src/synth/fluid_chan.c);
they do not establish identical behavior in every possible instrument. Added
import fixtures cover both destinations, explicit zero, the pre-event initial
value, mapped-control isolation and project reload.

The CC rack's enable boolean and knobs remain discoverable through native
`instrument.inspect` / `parameter.list` and editable through `parameter.set`.
Used controls replay as Internal MIDI input to the loaded instrument. Playback
uses the current native tick and frame offset before scheduling notes; controller
edits while playing wait for that tick. Stopped edits apply immediately. Bytes
round to 0..127, and equivalent bytes are not sent repeatedly. Native silence,
instrument replacement, channel changes and disable/enable transitions invalidate
the previous delivery state so unchanged controller values can take effect again.
Only used controllers replay: sending all 128 zero-valued knobs would also send
unused channel-mode commands. Rack-generated channel-mode cleanup does not cause
the entire rack to flush again on every tick.

CC input, native track output and rack replay now share instrument dispatch
bookkeeping. A completed same-channel attempt remembers its byte value; a changed
byte or an attempt on another channel makes the affected active rack control
eligible for reassertion at the next native tick. Live input does not write rack
models or activate unused knobs. A disabled rack leaves live input in control.
MidiPort's existing enable/channel filters still run before input dispatch.
The rack describes native model automation, so enabled active values intentionally
take effect again after a competing live input rather than recording that input
as a project edit.

Reset All Controllers (CC 121, identified in the
[MIDI Association's controller table](https://midi.org/midi-1-0-control-change-messages))
invalidates all prior CC attempts and releases native sustained input notes through
the existing note-completion path. Sustain on/off bookkeeping is also shared with
native output. The rack can reassert its active values after reset; an explicitly
used reset knob is remembered and does not resend the same command every tick.
The track still has global sustain state rather than independent state for every
MIDI channel, so these paths do not establish complete multichannel pedal semantics.

A native invalidation generation prevents in-flight CC completion from concealing
nested input/output, rack transitions or native silence. Only the exact currently
emitted rack channel-mode event preserves completed channel-mode attempts during
cleanup; reentrant external commands and direct native silence invalidate them.
Value-controller attempts are invalidated even for the exact rack command, since
cleanup clears native pedal state. A later tick reasserts those values once while
retaining completed channel-mode attempts.
Existing cleanup still retains the InstrumentPlayHandle unless the caller requested its
removal. Invalid CC controller/value/channel ranges are refused before instrument
dispatch and native sustain changes. Cache entries describe normally completed
attempts, including a plugin's false return; unchanged ignored controllers remain
suppressed until another model/state trigger. This avoids repeated warnings but
does not prove that a particular plugin accepted the controller.

Instrument settings now persist used controller indices in the `activecc`
attribute, including zero-valued controls. Loading suppresses rack events until
native playback and validates this bounded list; malformed lists disable the
rack and report an error. Legacy projects without the list infer use from nonzero
values and explicit CC model elements. They cannot recover an untouched explicit
zero stored only as an attribute. Older LMMS versions ignore the new list.
`InstrumentMidiCCTest` now authors 24 cases / 59 scenarios using production track,
model, playback and serialization paths with a deterministic native instrument
receiver. Cases include initial replay, byte equivalence, lifecycle changes,
timing, channel-mode commands, restoration, legacy/malformed lists, delivery
failure and retry, callback edits, parameter discovery, undo/replay and session
recovery. Delivery cases cover shared input/output delivery,
same/changed/other-channel values, input filtering, disabled rack control, reset
and one-time sustained native note completion, exception retry, false-return
suppression, nested delivery/state invalidation and malformed CC rejection.
Channel-mode rows also require one-time value/pedal restoration while preserving
the command's completed attempt.
This fixture is also uncompiled and unrun; it provides no actual DSP,
SoundFont, hardware, GUI or audio proof.

Raw event validation now uses signed `MidiEvent::param` values rather than
convenience accessors, which can mask controller numbers or narrow values.
The initial CC refusal implementation used those accessors and did not actually
refuse every malformed raw field; source diagnostics passed without establishing
that behavior. Six additional CC rows cover high-byte, negative-alias and signed
maximum parameters. They are authored and unrun; the corrected guard is source
evidence rather than executed refusal proof.

Native instrument input and output validate meaningful raw fields before note
array access, pitch/model updates, piano state or receiver dispatch:

| Event | Native field bounds |
| --- | --- |
| Note on/off, key pressure | Key 0..127; velocity/pressure 0..127 |
| Control change | Controller and value 0..127 |
| Program change, channel pressure | First parameter 0..127 |
| Pitch bend | First parameter 0..16383 |
| LMMS note-panning meta event | Key 0..127; native signed panning -128..127 |

Known channel messages require channel 0..15. Native preview input notes, key
pressure and note-panning retain the existing `-1` channel sentinel; the native
NotePlayHandle selects the track output channel for preview notes. This preserves
piano and piano-roll entry points without requiring a window. The signed panning
range belongs to LMMS's native meta extension, rather than wire CC panning.
Other system, SysEx and unknown meta/plugin-specific messages retain their existing
dispatch and are not universally validated by this guard.

Zero-velocity native output now follows the note-off counter/piano/signal path,
matching the existing MIDI wire encoder's release normalization. The equivalence
is also described in [RFC 6295, Appendix A.6](https://www.rfc-editor.org/rfc/rfc6295.html#appendix-A.6).
Unmatched note-offs retain their cleanup delivery behavior without making overlap
counts negative; positive onsets saturate before signed integer overflow.
Native output still uses its existing zero receiver release velocity and output
channel policy. Engine changes and note mutexes use scopes, so a thrown receiver
callback does not retain those locks. This does not roll back already changed
counters, piano state, released handles or plugin effects.

InstrumentMidiEventTest authors eight cases / 100 scenarios with actual native
track, Piano, model and NotePlayHandle paths and a deterministic receiver. Two
39-row malformed matrices check preserved held notes/models/piano/receiver state;
fourteen endpoint rows retain valid bytes and timing. Other cases cover preview
sentinels/signed panning, extreme keys, zero-velocity release, overlap accounting
after stray releases, and native cleanup/further output following receiver throws.
Its process timeout bounds lock-leak failures. All remain uncompiled and unrun;
these fixtures do not establish real driver, plugin/DSP, GUI, sanitizer, project
compatibility, resource or full autonomous DAW acceptance.

This is not a claim of full MIDI interoperability or transactional reversal of
all application side effects. Failed construction can leave the project marked
modified and retain errors/journal/allocation history; MIDI auto-assignment and
external instrument effects are not undone. Arbitrary synchronous model deletion
from callbacks remains outside the cleanup guarantee. Input/parser/event/model
storage remains resident, bounded only by representable sizes and available
resources. Checks cannot interrupt PortSMF parsing, instrument loads, allocation,
sorting or blocked native calls. GUI cancellation cannot interrupt event-suspended
construction. Files exceeding INT_MAX bytes and events leaving insufficient
native bar-rounding headroom are refused. Format-2 independent sequences, SMPTE
musical interpretation, exact CC command ordering/RPN/NRPN behavior, complete controller
mapping, actual PatternStore playback/round trips, native audio, GUI, project compatibility,
CPU/RSS and full autonomous workflow acceptance remain unverified or unfinished.
The rack remains value automation rather than a complete ordered MIDI event
stream: repeated identical commands, same-tick controller ordering and 14-bit
pairing are not preserved by a held model value. Instruments may ignore CC input,
and internal input delivery does not confirm any physical MIDI output. Arbitrary
synchronous track destruction or instrument replacement under playback locks is
outside the callback guarantee. Native application/plugins require rebuilding
for the extended instrument model interface.

## Native MIDI file export

`project.exportMidi` takes a destination `path` and calls the same native exporter
as Song's MIDI export action. The exporter now reads InstrumentTrack, MidiClip,
Note and PatternTrack models directly, without serializing plugin state. Unloaded
instruments retain their editable notes with the default MIDI patch. An empty
song produces a conductor track containing tempo, name and end-of-track events.

Current track gain, base key and enabled master pitch are applied to note values;
pitch and velocity are clamped before conversion to MIDI bytes. Zero-velocity
and disabled notes are omitted. Native track/clip mute and solo-derived mute are
honored. Ordinary clips apply signed start offsets, restart notes overlapping the
leading trim boundary and crop notes at the trailing boundary. Pattern cells use
their native local onsets and trim-boundary restart rule. Pattern indices remain
integers beyond 255, and missing cells are read without creating native clips.
Repeat periods use the maximum instrument-row length rounded to the current
native bar, including muted rows, as PatternStore does.

Pattern placements preserve signed loop phase. Overlaps choose the latest start,
then stored clip order for ties; an earlier long placement resumes when a later
one ends. Placement intervals are half-open tick ranges. Already triggered regular
notes may extend beyond a placement's end. This describes the editable arrangement;
it does not reproduce frame-block and inclusive-end boundary artifacts in live
PatternTrack playback. Step-note durations remain approximate: the historical
1,500-tick upper bound is shortened at the next audible onset or clip/segment end.
It does not measure the instrument's sample or envelope duration.

The writer retains every event instead of silently stopping at 50 KiB per track.
Files use native 48-tick quarter notes, keeping integer note endpoints exact.
Header, MIDI data, tempo, channel, chunk and endpoint bounds are checked. A bank
selection precedes its program change; releases precede new note-ons at the same
tick. Stable ordering and event-local channels survive repeated encoding. Long
gaps use empty text events to keep each variable-length delta within four bytes.
The format reference is the MIDI Association's
[Standard MIDI Files specification](https://midi.org/standard-midi-files-specification).

Native model reads require the owner thread and hold the engine model guard.
Event pumping is suspended during export. Inherited cancellation/budget checks
run during traversal, repeat expansion, event encoding, each 64 KiB output chunk
and immediately before QSaveFile commit. Export errors return failure and an
existing destination remains intact until publication succeeds. These cooperative
checks cannot interrupt a blocked library call, allocation or sort. Event and
encoded-track buffers remain resident; no constant-memory or CPU/RSS bound is
claimed. Reusing an exporter starts fresh channel and pattern state each time.

The export contains editable note arrangement and static tempo/pitch/gain. It does
not render samples/effects, tempo or parameter automation, arpeggiator/chord output,
per-note expression, microtuning/key-range synthesis, native MIDI-port processing,
or meter/key-signature metadata. After fifteen melodic channels, channels wrap
and share patch state; same-key overlapping voices also depend on the receiver.
Use native audio rendering and listening for audible production evidence.

`MidiExportTest` directly compiles the production exporter and independently
decodes its actual output. Nineteen authored cases / 65 scenarios cover a 20,000-note
track, complete releases/EOT, ordering, repeatable encoding, long gaps, invalid
values, empty/unloaded models, gain/pitch bounds, trim/offsets, mute, high pattern
indices, multirow odd-meter periods, overlap/phase, step approximation, Unicode,
reused exporters, cancellation preserving an old destination, owner-thread/event
admission, sparse cells and invalid paths. All are uncompiled and unrun. The
existing process harness still covers the public export/import operations, also
unrun. File interoperability, GUI behavior, native DSP equivalence, resource use
and full autonomous studio acceptance remain unverified.

## Rendering, evaluation and remaining coverage

`audio.render` uses ProjectRenderer and LMMS's enabled encoders. The output suffix
must select an available WAV/FLAC/OGG/MP3 encoder. It supports native sample rates,
16/24/32-bit WAV, 16/24-bit FLAC and a cooperative render timeout (default five minutes).
Explicit bit depth is rejected for lossy formats. Output reports requested settings;
`audio.analyze` reports the actual decoded sample rate. Rendering checks native encoder
readiness, short writes and codec finalization before atomically publishing the file.
The result's `sha256` and `bytes` describe the encoded bytes streamed into that
atomic publication. Pass that `sha256` as `sourceSha256` to subsequent analysis
and listening requests to require the same audio, including when reusing an
output pathname after revising the project.
An unwritable output reports an error and keeps the session alive. A plugin blocked
inside in-process DSP can also block cooperative cancellation; process isolation and stronger
resource supervision remain necessary. Individual native stems use the optional
track selection below. Live playback and capture operations are documented
separately; hardware and complete native workflow verification remain outstanding.

Rendering accepts the native export controls below. Defaults are explicit for each
request; an earlier GUI or CLI export does not supply hidden repeat/range defaults.

| Argument | Native behavior |
| --- | --- |
| `track` | Optional nonnegative native track index. Selects one currently unmuted Instrument or Sample track; omit for the full mix. Automation tracks and song PatternTrack wrappers are not audio stem targets. |
| `container` | `song` (default) or `pattern`; requires `track`. A PatternStore row renders its occurrences across the song arrangement. |
| `exportLoop` | Boolean, default false. True omits the one-bar tail padding on whole-song exports. It does not itself repeat the song. |
| `betweenMarkers` | Boolean, default false. True selects the song timeline's loop begin/end, regardless of the playback loop-enabled flag. Set markers through `timeline.set`. |
| `loopCount` | Integer 1..2147483647, default 1. Passes through the marked section. Whole-song mode includes the surrounding arrangement and extends to the loop end when necessary for repetition, then adds its tail unless `exportLoop` is true. |
| `bitRate` | OGG/MP3 only; integer 64..384, default 160, matching the native CLI admission range. The codec can reject or adjust the request. MP3 uses the native LAME CBR setting; OGG maps this value to Vorbis VBR quality as `bitRate / 320`, not a target kbit/s. |
| `stereoMode` | MP3 only: `mono`, `stereo` (default), or `jointStereo`. |
| `compressionLevel` | FLAC only: number 0..1, default 0.625 where native compression control is compiled. Supplying this argument in a build without that control fails explicitly. |

Codec-specific arguments on other formats fail instead of being silently ignored.
The FLAC device requires the compression command's success acknowledgment, following
the [libsndfile contract](https://libsndfile.github.io/libsndfile/command.html#sfc_set_compression_level).
The result reports `requestedBitRate`, `requestedStereoMode` and
`requestedCompressionLevel` (null where inapplicable/unavailable), plus
`bitRateSemantics`. These are requested native encoder settings, not independently
measured output bitrate or sample rate.

For stems, discover tracks with `project.inspect` in each container and call
`audio.render` once per eligible source, using distinct output paths and request
IDs. Each file has its own atomic publication, encoded digest and durable receipt;
there is no all-or-nothing multi-file transaction. For example, after setting the
song markers:

```json
{"id":"stem-bass-1","op":"audio.render","args":{"path":"exports/bass.wav","container":"song","track":0,"betweenMarkers":true,"bitDepth":24}}
```

The result's `stem` is `{ "container": "song", "track": 0, "type": "instrument" }`
or the corresponding sample/pattern selection; it is null for the full mix.
These indices identify the selection for that request. Queued agent calls bind
native target identity, so a preceding track removal/move invalidates a stale
render selection and requires rediscovery. Replaying a committed ID returns its
historical receipt without rendering again or checking the current output path.

Isolation is a temporary native export filter shared with GUI/CLI individual-track
export. It excludes other instrument/sample tracks and their own audio buses,
including their effect tails, without toggling mute/solo models. The selected
source keeps its mute automation, track effects, mixer sends and master processing;
automation tracks and song pattern wrappers keep their native behavior. Automation
can still change native model values during rendering and retain its final values,
as in ordinary native export. A fresh headless selection rejects a currently muted
track; use an explicit native edit if its mute state needs to change.

The GUI/CLI manager snapshots initially eligible tracks before the first stem.
Earlier stems' automation cannot invalidate that admission or erase a later
selected track's arranged extent; that track's mute automation still controls its
audio. Selection flags are released after processing stops, on success or failure,
and are neither serialized nor journalled. Solo restoration reads the model's mute
value so it cannot persist the temporary export filter as a user mute state.

Whole-song stem lengths use the selected source's arranged extent and the native
remaining automation/pattern extent. Use the same `betweenMarkers` range and render
settings when aligned files are needed. Shared/nonlinear mixer or master effects
can make summed stems differ from the full mix. Stems use the current native render
graph; they are not pre-effect or pre-master taps.

`renderRange` records `exportLoop`, `betweenMarkers`, `loopCount`, `begin`, `end`,
`loopBegin`, `loopEnd` and `effectiveTicks`. Positions are native ticks;
`effectiveTicks` is a decimal string to preserve large repeated lengths exactly.
The range comes from `Song::startExport` after device setup and native exclusion of
muted tracks/clips, rather than from the earlier preflight. Zero-length final ranges
fail. Invalid active markers and tick-bound overflow fail before rendering; repeated
length and progress arithmetic use 64-bit storage. This is a musical range, not a
sample-accurate trim promise: native tick/buffer rounding and codec delay remain.

The prior export-loop flag, marker-selection flag and repeat count are restored on
success and failure; timeline markers are not changed by rendering. Repetition is
gated to active export so those stored settings cannot repeat ordinary playback.
Native timeline-jump forwarding is synchronous, allowing sample/LFO/VST consumers
to handle offline loop discontinuities before the next tick. A render still stops
playback through the native exporter; it does not resume an earlier transport state.

`StudioExportSettingsTest` authors ten cases / 44 executions for meter/range math,
native rendered PCM order/duration, repeat and tail selection, cancellation/timeout,
setup and publication failure, empty effective ranges, codec controls (including
CLI bitrate 96), invalid arguments, ordinary playback, processing-thread jump
delivery, completed-pass/wide-length progress, device-setup changes and muted extent exclusion, session history,
undo/redo, restart and receipt replay. Codec rows can skip unavailable encoders or
independent MP3 decoding. All cases remain uncompiled/unrun.

`StudioStemTest` authors ten cases / 21 executions for song/PatternStore selection,
sample/instrument filter behavior, solo restoration, independently decoded sample
stems, selected effects/routing/automation/master processing, excluded bus tails,
invalid/nested/foreign-thread admission, cancellation/setup/publication failures,
GUI/CLI bulk eligibility after mute automation, a later full mix, replay/reopen,
undo/redo and scripted agent rediscovery in both provider protocols. None has been
compiled or run. The instrument rows check native selection state, not a rendered
instrument plugin; the provider rows are scripted orchestration fixtures, not
evidence of actual reasoning or musical judgment.

`audio.analyze` decodes through libsndfile and reports duration, channel peak/RMS,
full-scale samples, and nonfinite samples. It rejects truncated decodes and limits
duration to the requested budget (default two hours). These are technical facts;
the interface explicitly reports `perceptualListening: false`.

Analysis and listening first capture a private source copy, then decode that
copy. Both report its `sourceSha256` and `sourceBytes`; analysis also returns the
absolute requested `path`. Optional `sourceSha256` must be exactly 64 lowercase
hexadecimal characters. A different accepted digest fails before sample measurement or
audio-provider submission. Omitting it accepts the bytes copied for this request.
Changes to the original after capture cannot change the decoded excerpt or
measurements. Capture is a streamed copy, not an atomic filesystem snapshot of a
concurrently edited source: its digest identifies the exact accepted copy, and
an expected digest detects differences from a known render. The copy is temporary
and does not preserve an export as a durable session asset.

Capture and verification cooperate with cancellation and native execution budgets
between file chunks. Scratch disk use is proportional to the source file. The
listening source limit is enforced before copying; analysis retains its existing
duration limit without a new source-byte cap. Analysis checks the original header
before copying, then rechecks the accepted copy's header before measurement; a
change after preflight cannot bypass the duration limit. An operating-system I/O
call or native decoder can still block cooperative
cancellation. Digest validation and numeric argument validation precede capture.
Replaying a committed request ID returns the original receipt, even if the path
has since changed or disappeared. Use a fresh request ID to evaluate current bytes.

`audio.listen` decodes a bounded mono/stereo excerpt through libsndfile, transcodes
it to PCM16 WAV and submits it to the configured audio provider. Defaults are the
first 30 seconds; `offsetSeconds` and `durationSeconds` select other segments, up
to 120 seconds and 20 MiB. Input audio must be 8â€“192 kHz and the source at most
1 GiB. Results identify the provider/model, accepted source and excerpt SHA-256,
actual decoded scope, and attributable provider judgment. PCM16 transcoding can
clip floating-point values outside full scale; technical source measurements are
reported independently, and `transcodingClippingSamples` counts affected excerpt
samples. The evaluation prompt supplies this scope and transcoding metadata.
Multiple excerpts are needed to assess longer works.
Fixture tests assert real waveform submission and revision orchestration, and do
not establish perceptual quality or a functioning real musical evaluation loop.
`StudioAudioIdentityTest` adds six native cases / 21 executions covering published
WAV digests at three bit depths, overwrite/replacement/deletion during decoding,
invalid and mismatching digests, duration bounds, cancellation during capture and
decoding, receipt replay versus fresh checks before/after reopen, and a scripted
agent that renders, measures, submits excerpts, revises native sample gain and
preserves editable work using both reasoning protocols with/without restart.
Provider declaration checks also cover the optional digest on both evaluation
tools. These fixtures are authored but uncompiled/unrun.

Other outstanding coverage includes MIDI timing/controller compatibility, complete sample
and curve verification, arbitrary controller creation, full arpeggiator/chord and
tuning compatibility, richer arrangement edits, detailed effect observations,
plugin failure diagnostics, plugin recovery/isolation, conversation steering during
active work, provider context-window management and stronger resource supervision. Existing GUI
functionality and the entire native plugin set remain in the build. Full native
regression, compatibility-corpus and perceptual-workflow evidence are still required.

Encoder error handling follows the native library contracts:
[Vorbis block processing](https://xiph.org/vorbis/doc/libvorbis/vorbis_analysis_blockout.html),
[Vorbis packet processing](https://xiph.org/vorbis/doc/libvorbis/vorbis_bitrate_flushpacket.html),
and [libsndfile file descriptors, frame writes and close results](https://libsndfile.github.io/libsndfile/api.html).
Those contracts inform the source changes; they do not substitute for executing the tests.

## Native remote-host failure handling

The existing LMMS remote plugin parent, watcher, shared-memory FIFO and Unix socket
transport now contain bounded waiting and failure propagation in source. They still
host the native VST/Zyn implementations; this adds no external supervisor or GUI
automation. Rebuild the application and every remote host together because the
native C++ interfaces changed. Real hosted-plugin compatibility is unverified.

Process startup and Unix connection waits have 30000 ms bounds. Each parent send or expected-reply wait defaults to
120000 ms; nested work retains the earliest active deadline, and one processing
callback shares a deadline across its send and reply. Set
`LMMS_REMOTE_PLUGIN_TIMEOUT_MS` before starting LMMS to an integer from 1000 to
600000. Invalid values emit a diagnostic and use the default. The native watcher
checks monotonic deadlines every 25 ms and requests child termination on expiry;
actual scheduling/termination latency must be measured. GUI event processing and
unrelated replies do not renew the active deadline. Idle hosts have no active
request deadline.

The native renderer acquires a process-wide export context before device setup.
Its monotonic execution budget and explicit cancellation are visible to the
remote watcher independently of the main event loop. Startup checks cancellation
between bounded waits, and active host requests inherit the render context. A
request already in flight when device setup begins can adopt that context too.
Cancellation invalidates interrupted transport and requests child termination;
it does not wait for the longer host request timeout. Actual latency is unverified.
Interrupted hosts need fresh initialization; automatic project/plugin recovery
has not been implemented.

Context IDs increase monotonically, so cancellation of an older export does not
cancel a later one. ID/deadline snapshots are synchronized. Idle hosts have no
active request to interrupt. After the render worker finishes, new remote requests
are detached from render execution, allowing healthy hosts to receive device
restoration messages even after cancellation. The export reservation remains
until the renderer is destroyed. Studio and the shared manager also reject export
entry while an audio-device backup is pending, before opening another output.
These entry points follow the existing single Song/AudioEngine ownership model;
this does not support simultaneous independent projects.

`ProjectRenderer::startProcessing` now reports whether the worker actually started.
Studio starts its cancellation timers before device setup and avoids entering a
completion loop after a failed start. Codecs opened before setup retain explicit
ownership until the engine installs them, including cancellation and setup-error
paths. Finalization checks cancellation, and studio publication checks the elapsed
budget before copying/committing the temporary file. The optional export dialog
retains its manager while a nested startup call unwinds before destroying it.
Native GUI behavior and all file/device ownership paths still need execution.

FIFO lock/message waits periodically observe invalidation. Complete FIFO messages
are published together and release the data lock while waiting for capacity, so
the reader can drain it. Unix invalidation shuts down the socket to wake blocked
I/O. Message fields, aggregate lengths, shared pointers and audio channel counts
are checked before allocation/copying. The FIFO retains its native 512 KiB
capacity; socket messages have a 64 MiB aggregate limit, and shared audio storage
has a 64 MiB limit. Oversized messages fail explicitly. These limits require
compatibility-corpus and resource measurements.

Processing requires a matching completion reply. Failures clear the output and
return failure; using an already failed host also records a new failed processing
attempt. The renderer observes a process-wide host failure counter, stops export
and refuses finalization. This conservatively includes a concurrent failure from
another native remote host. Studio rendering rejects the temporary output before
atomic publication. The shared export manager reports failure, closes the file
device and removes partial output; CLI export returns failure and the optional GUI
reports the same error. Completed earlier stems remain available if a later stem
fails. Legacy export still uses its existing direct destination writes.

`RemotePluginTest` contains twenty-two authored native QtTest cases, including nine
processing-fault rows, two studio cancellation/deadline rows and two export
reservation rows. They exercise real native IPC with fault fixtures for
crashes, hangs, malformed/partial messages, reply floods, bad channel counts,
abandoned FIFO locks, backpressure, binary round trips, host restart, export
failure and child diagnostic routing. Added cases cover interrupted processing
without main-thread events, requests predating export setup, cancelled startup,
pre-worker cancellation, unattached codec closure, manager ownership during setup,
reservation after worker completion, preservation of an existing destination and
fresh-host recovery followed by a native WAV render. The fixtures are test instrumentation and
do not prove VST/Zyn DSP or third-party compatibility. All these native tests are
unrun. CPU/heap benchmarks, the complete platform/host matrix, automatic plugin
recovery and supervision of in-process DSP remain required. In-process DSP, codec
finalization, disk I/O and device restoration still require cooperative unwinding.
A blocked diagnostic sink can also delay watcher execution. These source changes
do not establish a hard termination guarantee for the complete application.

## Native mixer routing and processing

`mixer.route` now delegates to a native route API that also validates GUI and
other direct callers. Endpoints must belong to that mixer, sends cannot leave
master or return to their sender, and new edges must preserve an acyclic graph.
Send amounts passed to this API must be finite and between zero and one.
Updating an existing send preserves its model and avoids duplicate dependencies.
Creation reserves all route lists before publishing their shared pointer. Cycle
checks visit reachable channels iteratively instead of enumerating every path.

Native mixer XML checks channel/send indices and its complete routing graph
before replacing the mixer. Forward references remain valid; comments and
unrelated elements do not become channel zero. Negative, malformed or
unrepresentable indices, sends from master, self-sends and feedback cycles become
native load errors. Index admission preserves the native 16-bit count/index
representation: at most 65,535 channels, numbered 0 through 65,534. This is not a
memory or render-capacity guarantee; the existing job queue's 8,192-entry bound
still applies. Studio project loading treats these errors as incomplete loads
and restores its prior checkpoint, advancing revision to invalidate replaced
object handles while preserving musical history.

Each period snapshots every channel's mute state before any dependency is
published. Muted channels clear their audio and notify receivers using that
snapshot. Ready channels are admitted once. Dispatch drains the full graph even
when master is muted or buses have no output route, allowing their native effect
state to advance. Unfinished dependencies after a successful queue drain latch
`mixer-dependency` through the existing DSP inspector/retry path. Returned job
exceptions and queue overflow retain their existing failure causes.

Nine native cases/23 executions are authored for exact buffer mixing, mute-state
transitions, effect call counts, converging graphs, direct route validation,
malformed XML, native model round trips, dependency failure/retry and transactional
project-load rollback. They remain uncompiled/unrun. Large-project
resource bounds, real plugin tails, sample-exact automation, physical playback,
GUI regression and project-corpus compatibility remain unverified.

## Native mixer solo and mute restoration

`mixer.edit` with `solo` now operates through core-owned model connections, with
no MixerView required. Direct native solo-model writes use the same path. Only
one channel is selected. Selecting another replaces it; writing the same solo
value again preserves current state. `project.inspect` includes `soloChannel`
(-1 when inactive) and per-channel `solo` and `muteBeforeSolo`, alongside the
existing effective `muted` value. When inactive, `muteBeforeSolo` equals current
mute state.

Solo retains the native routing rule: the selected channel, its upstream
senders and every downstream output of those senders become audible. Traversal
stops at master, so unrelated inputs to master remain muted. Soloing master only
unmutes master. An initially muted master is restored when solo is released.
Both traversals visit each channel once, including in converging graphs.

The first selection snapshots mute states. Switching selections keeps that
snapshot; release restores it. Explicit mute edits during solo update the state
to restore, including same-value edits on an already masked channel. They also
remain effective until another selection or routing change reapplies the mask.
Adding/removing routes or adding/removing/resetting channels updates the mask.
Deleting or resetting the selected channel releases solo; moving it preserves
selection by channel identity. The optional GUI observes these core transitions.

While solo is active, native mixer XML adds `mutedBeforeSolo="0|1"` to each
channel. Existing `muted` values retain effective overrides, and `soloed` retains
selection. Loading this state restores both layers without overwriting overrides.
Malformed restoration values reject mixer load before replacement. Older projects
without the new field use their saved mute states as the release baseline and
apply solo after routes have loaded. Their original pre-solo mutes cannot be
recovered if the file never recorded them. Multiple saved solo flags resolve to
the highest channel index, matching the previous selection scan.

The native journal groups mute/solo transitions into one compact record containing
the selected model identity, effective mutes and release baselines. Derived mute
writes do not add separate records. Same-value mute edits that change the release
baseline also receive a record. Restoration updates controls on existing channel
objects, preserving volumes, effects and route objects. Records follow channels
when moved; changed channel membership or malformed state rejects restoration
before mutation. If the initiating model has been deleted, the existing native
journal policy skips its dead record. This is separate from studio checkpoint
undo/redo, which restores complete native project state.

Native journal undo/redo retain a pending record when serialization or checked
restoration fails, and restore the prior journal-enabled flag after returned
C++ exceptions. Recursive undo/redo and clearing history during restoration are
rejected. Generic legacy restoration still has a void load contract; arbitrary
callbacks may partially mutate state before failing. These guards do not provide
transactional rollback of every native object or callback.

Sixteen native cases/35 executions plus process checks are authored for routing,
master and channel selection, explicit mute edits, channel lifecycle, new and
legacy XML, exact stereo output, session undo/redo, rollback, restart and receipt
replay, grouped native journal transitions, malformed records, rejected or throwing
restoration, and journal reentry. All are uncompiled/unrun. GUI behavior, actual
journal execution, linked controls and controller-driven solo/mute, automation
timing, old-version file handling, large-graph resources and concurrent callback
reentry still need verification. Native components require rebuilding after the
journal virtual interface and mixer control type changes.

## Native DSP failures and explicit retry

`audio.processing.inspect` and `audio.processing.reset` take no arguments and do
not change project revision/history. Inspection also appears at
`transport.backend.dsp` in native project/transport observations. It reports
`failed`, a named first `failure`, decimal-string `failureCount`, `resetEligible`
and `counterExhausted`. The count belongs to the current engine lifetime and
counts failure reports; it is not a hardware-health result or a durable cross-
process identifier. Causes distinguish setup, instrument/effect/mixer jobs,
queue overflow, mixing, nonfinite output and corrupted effects.

Returned C++ processing exceptions now leave ThreadableJob terminal and reach a
queue failure latch. Remaining queued jobs are marked done and queue completion
counts advance even after a throw. An overflow ends the rejected job and fails
the period. AudioEngine observes stage failures, discards mixer period state,
invalidates capture continuity and returns silence for both pipeline buffers.
Later periods remain silent without running DSP or advancing native song setup
until explicit retry. The native Song can still report playing while faulted;
stop it before repair/retry. Nonfinite play-handle, bus and effect audio is
observed before silence detection or optional sanitization can hide it, and
master pipeline output is also checked.

Retry requires the application/engine owner thread, stopped transport, disarmed
sample and automation recording, no unfinished/reserved take, no export and no
stored/file audio-device reservation. It preserves the report count and clears
the first-cause latch so a subsequent period may try again. Healthy stopped reset
is idempotent; saturation refuses reset. Clearing does not repair a plugin or
assert recovery. A still-faulty project can immediately latch another failure.
Saved retry receipts replay their prior result without clearing a newer fault;
use inspection for current state. Playback/resume, export and recording reject
a latched fault. Recording also checks during capture and discards the failed
take while preserving prior PCM.

A successful reset of an actual DSP failure also lazily invalidates native
Controller and AutomatableModel value-buffer caches. The next buffer read must
recompute even if its native period number is unchanged, so repaired manual
values or newly received MIDI values do not reuse failed-period data. Both
musical clocks, stored parameter values and the report count stay unchanged.
Cache generations are stamped only after successful computation. Healthy reset,
refused reset and receipt replay do not repeat invalidation. Native interpolation
continues approaching its endpoint within the buffer and settles on a following
period; invalidation does not change that convention.

This is cache refresh, not rollback of partial oscillator, envelope, interpolation,
transport or plugin state. Stateful controllers can advance when recomputed.
Private plugin caches and custom override behavior remain outside the guarantee.
The new transient native cache fields require full rebuilding and external
C++ API/ABI review. Generations and clocks still have finite ranges; native
model synchronization remains required.

DspCacheRecoveryTest authors eight cases / 15 scenarios for failed native play
jobs using owned MIDI controllers and linear/log parameter buffers, manual null/
interpolation and linked caches, healthy/refused resets, returned recomputation
failure, native runtime receipt replay and LFO refresh without clock rewind.
All remain uncompiled and unrun; physical/native output, private plugin recovery,
GUI/legacy compatibility, allocation/latency/CPU/RSS and autonomous production
remain unverified.

ProjectRenderer compares engine failure count/latch before start, between native
periods and before/after output finalization. It catches returned exceptions in
its processing body and records cleanup exceptions as failed export execution.
Studio export does not publish the temporary artifact on those failures, so an
existing destination survives. Legacy direct export and external plugin private
locks/destructors still need separate lifecycle verification.

Nine new native cases/22 executions are authored for standard/nonstandard nested
exceptions, concurrent queue claims, overflow, real native periods, bus/master
effect faults with sanitization on/off, stopped/owner/capture/export retry guards,
target preservation, transient receipt replay and capture rollback. They remain
unrun. Source checks pass for 195 changed C++ files and 99 operations; no native
build, Qt test, process trace, render or resource measurement has run. Hangs,
deadlocks, signals/SEH, crashes and corrupt memory cannot be contained by these
C++ catch boundaries. Complete plugin repair, setup/cleanup failure recovery,
hard termination and resource bounds remain required for the full DAW.

## Native audio worker dispatch

The `workers` object inside `audio.processing.inspect` (also at
`transport.backend.dsp.workers`) reports `registered`, `ready`, `queued`,
`scanning` and `batchActive`. These integer counts and boolean form a snapshot
under the scheduler mutex. Registered workers include starting/quitting objects;
ready counts workers accepting new dispatches. Queued and scanning are disjoint
states of admitted workers. The coordinating render thread also processes jobs
and is not counted. When `batchActive` is false, queued and scanning are zero.
The snapshot does not measure processor health, elapsed work or latency, and can
change between requests without a project revision.

Workers now wait on a shared mutex and dispatch predicate. A batch finishes only
after job completion and return of every admitted worker's queue scan. Reset,
overlapping dispatch and worker construction reject an active batch. Quit cancels
an unstarted admission or waits for an active scan to return; it does not reset
the shared queue. Engine/worker destruction joins workers before destroying them.
The obsolete unstarted worker object is removed; the coordinator remains usable
with zero background workers. Dynamic child publication is supported from jobs
already outstanding in the batch, as used by native mixer dependencies. An
independent producer cannot publish after a batch has completed.

Nine native scheduler cases/15 executions are authored for startup, repeated
static/dynamic batches, dynamic job trees, returned exceptions and retry, active
reset/reentry refusal, quit/admission races, active destruction and pool reuse.
They use real Qt workers and remain uncompiled/unrun. A processor that never
returns can still block dispatch and shutdown indefinitely. Deadlock recovery,
safe hard termination, platform behavior, sanitizer results and measured CPU/
latency/memory bounds remain unfinished or unverified.

## Native audio-device restoration

`audio.device.restore` takes no arguments and retries an engine-owned device
reservation after failed restoration. It is transient: it does not create a
project revision. Live status appears at `transport.backend.restoration` with
`pending`, `restoring`, `inProgress`, `failed`, named `failure` and `retryEligible`.
Failures distinguish wrong-thread/busy requests, returned stop/notification/start
exceptions and a backend restart that reports failure. Saved responses are
receipts; replay does not perform another restoration. Inspect current status
before deciding whether the studio recovered.

The engine now retains the prior backend and its original running state during
export. Restoration is nonthrowing for returned C++ failures. Failed stop keeps
the current device owned; failed notifications/restart retain the restored prior
device and reservation for retry. Completed notifications are remembered across
restart retries. A stopped prior backend remains stopped. A cancelled reservation
that never switched is released without notifications/restart; same-pointer
switching does not delete that device. Owner-thread and transition guards reject
reentrant switching. Device stop/join occurs before the model lock is taken.
The restore capability bypasses the generic model-lock wrapper for the same
reason: a callback being joined may need that lock to render.

Running render execution prevents restoration. External restore also refuses a
still-owned export context before native finalization, unless restoration already
failed and is pending. A renderer refuses finalization through a retired file
device. Studio export checks restoration before publishing its temporary output
and observes new DSP reports during restoration/publication. Legacy RenderManager
keeps and reports its failed reservation and can reuse the manager after recovery;
legacy direct output writes retain their existing publication behavior.

Eight native cases/16 executions are authored for prior running/stopped state,
same-pointer/cancelled reservation, joining actual native rendering, standard/
nonstandard callback exceptions, stop failure before/after adoption, restart
status/exception failure, execution/owner/finalization guards and studio target
preservation. Lifecycle adapters do not represent physical drivers. All cases
and process-harness no-op/replay checks are unrun. Source checks pass across 196
changed C++ files and 100 operations. Compiler/CMake/qmake, initialized pinned
dependencies and a native LMMS executable remain unavailable in the workspace.
Native shutdown, opaque callbacks/destructors, blocked driver calls, port/routing
restoration, concurrent lifetime races and hard recovery/resource bounds still
require implementation or verification. The complete DAW goal remains active.

## Native instrument MIDI routing

Five native MIDI operations are implemented in source and remain unverified.
They address an instrument `track`, with optional `container` (`song` default,
or `pattern`). Inspection, configuration, subscription and input recovery also
accept a `parameter` with a direct MIDI controller connection. A parameter
selector cannot be combined with `track` or `container`. Project inspection
includes each instrument's MIDI state.

| Operation | Native behaviour |
| --- | --- |
| `midi.inspect` | Select `track` or `parameter`. Reports mode/readable/writable, channels, controller selections, velocity/note overrides, program, base velocity, raw-client capability and available/requested endpoints. |
| `midi.record` | Required `track`, `clip`, `durationMs`. Append a bounded native note-input take; options and limits below. |
| `midi.input.reset` | Select `track` or `parameter`. Explicitly retry failed local input delivery after observing backend availability; see the recovery section below. |
| `midi.configure` | Select `track` or `parameter`; optional settings below. A parameter supports only `readable`, `inputChannel`, and `inputController`. Prevalidates supplied values before changing a native model. |
| `midi.subscribe` | Select `track` or `parameter`; required `direction` (`input`/`output`), exact `port` name and boolean `enabled`. Parameters support input only. Enabling requires a reported available non-raw endpoint; disabling can remove retained unavailable intent. |

`readable`/`writable` enable native I/O. `inputChannel`/`outputChannel` use `0..16`:
input zero accepts all channels, output zero preserves event channels, and a
specific output channel retains native filter/generated-event semantics.
`inputController`/`outputController` use `-1..127`, with -1 selecting none.
`fixedInputVelocity`, `fixedOutputVelocity` and `fixedOutputNote` use `-1..127`,
with -1 disabling an override; native zero release velocity remains zero.
`outputProgram` is `1..128` and produces a zero-based program byte, unlike native
instrument bank/program indices. `baseVelocity` is `1..127` and preserves LMMS's
mapping to native note volume.

`outputSource` chooses `track` (the default, including older projects) or
`instrument`. Track mode forwards LMMS track events to the output port. Instrument
mode sends MIDI generated by the instrument and suppresses the track's direct
copy, preventing doubled notes when a plugin echoes or transforms its input.
`instrument.inspect` reports `midiOutputSupported`; selecting instrument output
through `midi.configure` requires support from the current instrument. The choice
is also available in the graphical MIDI panel and is saved as `outputsource` in
the native MIDI port state. Older LMMS binaries do not understand this new choice.

Carla Rack and Patchbay can provide instrument MIDI output through their existing
native processing callbacks. Complete channel messages are supported: note on/off,
polyphonic pressure, controller, program, channel pressure and 14-bit pitch bend.
The callback accepts port zero, events within the current audio block and at most
512 events per block. It copies bytes immediately, keeps equal-time event order,
and sends them through the existing MidiPort after Carla processing releases its
instrument mutex. Generated events do not reenter that instrument's MIDI input.
Callbacks outside the processing thread or block are rejected. Invalid selected
output or overflow rejects the whole buffered block and raises a native processing
error; a failed processing call cannot publish a partial MIDI block or replay
consumed input. Callback success means buffered acceptance, not device delivery.

`midi.inspect` includes `trackedOutputNotes` and `outputNoteLimit` (4096). Tracking
uses the actual output channel and transformed key, including overlapping notes.
Source changes, channel/key retargeting, disabling output, track silence,
instrument replacement and port retirement attempt releases for the port's
submitted notes and sustain/sostenuto controls. A failed source-change cleanup
keeps the previous source; failed channel/key retargeting restores its model value.
New track and instrument MIDI is suppressed during
offline export; cleanup can still release notes submitted before export.

Native output timing remains backend-dependent. Carla sample offsets are reduced
to whole MIDI ticks; backends may schedule those ticks or send immediately.
ALSA now cancels pending events for the retiring output port before immediate
cleanup, as described below; this has not been executed against the native kernel.
Sample-accurate hardware delivery and recall of already-dispatched events are not
established. SysEx/system messages and additional output ports,
native routing between LMMS instrument tracks, and recording generated events
into editable MIDI clips remain unfinished. Backend calls provide no delivery
acknowledgment. Cleanup can release another track's voices or pedals when they
share an external destination and MIDI channel.

Endpoint `available` and `selected` are distinct; selection stores routing
intent. `connectionVerified` is false because the native subscription APIs return
no acknowledgment. Model changes can send physical MIDI immediately; undo or
rollback restores settings and cannot retract an already sent message.

Structured `midisubscriptions` entries retain comma-containing/Unicode names and
unavailable requests. Legacy comma attributes remain supported; older readers
retain their comma ambiguity. Missing requests survive and reapply when their
exact endpoint name is reported again. Disabling a direction clears its requests.
Reload deselects prior subscriptions explicitly. Raw parser state is initialized,
SysEx/system-common status cancels running status, and native channel-voice output
uses the existing checked serializer. Raw complete messages have an output mutex;
WinMM uses the same serializer, including correct 14-bit pitch bend. ALSA endpoint
list copies/publication have a dedicated snapshot mutex.

Forty-six authored native cases have 111 executions when WinMM is compiled (105
portable executions and six conditional WinMM cases), and the executable harness
adds model/serialization/replay and default-dummy refusal checks. All are unrun.
They use diagnostic native boundary fixtures and do not prove physical MIDI
operation. Raw/WinMM/ALSA/CoreMIDI input uses the lifetime-aware queue described below.
ALSA has eighteen additional cases/66 executions described below; all are unrun.
CoreMIDI has twelve additional cases/19 executions described below; all are unrun.
Shutdown/hotplug acknowledgment, output delivery and device verification remain unfinished.

Nine new MIDI port cases/37 executions cover scalar decoding, source persistence,
duplicate prevention, note/pedal retirement, offline suppression, tracking limits,
cleanup failures, mode/model agreement and configuration prevalidation. Six Carla cases/22 executions
cover Rack/Patchbay output, ordered native routing, state restoration, malformed
events and overflow, processing exceptions, worker callbacks, mutex reentry and
input acceptance. All are authored but uncompiled and unrun. Graphical behaviour,
real MIDI-producing plugins, backend latency and hardware cleanup need native
execution evidence.
The byte-stream output used by ALSA raw MIDI and sndio still ignores timestamps
and lacks arbitrary SysEx/system-message output. JACK has its own scheduled
output path described above. The [MIDI Association summary](https://midi.org/summary-of-midi-1-0-messages)
supports message semantics, not native execution evidence.

`midi.inspect` and project instrument rows include transient `inputDelivery`:
`queuedDelivery`, `registered`, `pendingEvents`, `queueLimit`, `failed`,
`failureCode`, decimal-string `droppedEvents` and `restartRequired`.
`queuedDelivery` is true for raw clients (including dummy), WinMM, ALSA and CoreMIDI.
A registration or healthy empty queue does not verify a physical
connection. The failure codes are 0 none, 1 queue overflow, 2 enqueue failure,
3 processor cleanup failure/exception, 4 backend callback exception, 5 native backend input
failure, 6 intentional client transfer, and 7 client retirement with detachment
to Dummy. `inputDelivery.backend` and `transport.inspect.midiBackend.inputBackend`
report `observationAvailable`, `active`, `nativeErrorCode`,
`overrunObservationAvailable`, decimal-string `reportedOverrunBytes` and `overrunScope`.
The overrun fields describe native byte-loss observations separately from
application `droppedEvents`; an unavailable or zero counter does not certify
loss-free capture. ALSA sequencer supplies an
observed reader state and the first native error across startup/routing/output/
input operations. WinMM reports observed activity for successfully started native
inputs and its first input MMRESULT/API error; output errors are observed separately.
CoreMIDI reports an observed native input-port registration
and the first native error across its operations. ALSA raw MIDI and sndio report
their reader availability and terminal error; JACK reports native availability
and shared-audio input activity as described above. Dummy and unobserved clients report an unavailable observation;
their false `active` field does not establish that they failed. An active reader
does not certify MIDI connectivity. `midiBackend.startupFailures` retains failed
factory attempts with backend name, `nativeCodeAvailable` and `nativeErrorCode`;
unknown native codes remain explicitly unavailable.

ALSA raw MIDI and sndio now open their native duplex streams in nonblocking mode.
ALSA validates poll descriptor counts and return values; sndio allocates room for
the descriptor count required by its API instead of assuming a single descriptor.
ALSA accepts 1 through 64 descriptors per direction (128 combined); sndio accepts
1 through 64 for its duplex handle. Both add one owned wake descriptor and
serialize native handle calls against input, output and retirement. A failed open
or descriptor setup closes any acquired handles. Shutdown signals a nonblocking,
close-on-exec wake pipe, joins the reader without holding the native I/O lock,
then closes handles even when the reader has already exited. Neither driver uses
forced thread termination. Native/library calls and close/join still lack a forced
deadline; this is not proof that arbitrary drivers cannot block.

Input polls retry `EINTR`, and empty nonblocking reads leave the reader running.
Each read is bounded to 256 bytes. Fatal read/poll/descriptor errors stop native
delivery, expose unavailable input health and latch enabled model deliveries until
backend replacement. ALSA reports the negative API/system error; sndio reports
normalized errno-style categories because its API exposes EOF without a detailed
native error code.
The contracts are documented by [ALSA's raw MIDI interface](https://www.alsa-project.org/alsa-doc/alsa-lib/rawmidi.html)
and [sndio's MIDI manual](https://man.openbsd.org/mio_open.3).

ALSA raw input now samples `snd_rawmidi_status` and `snd_rawmidi_status_get_xruns`
before starting its worker, after an idle poll timeout, before a ready input read
and after a nonempty read, before decoding it. The [ALSA status API](https://www.alsa-project.org/alsa-doc/alsa-lib/group___raw_midi.html)
returns the native counter; the [Linux raw MIDI implementation](https://github.com/torvalds/linux/blob/master/sound/core/rawmidi.c)
reports input overruns in bytes since the preceding status sample and clears the
counter when sampled. LMMS retains the reported sample without subtracting an
older one. Studio inspection reads cached observations and does not consume the
native counter itself.

A positive count stops the duplex client, records the observed discontinuity as
`-EOVERFLOW`, and faults enabled model deliveries through the existing native-input
failure path. It discards the uncertain chunk before parsing; old running status
cannot silently reinterpret later bytes. Pending model input is revoked and local
note cleanup runs on the model owner. Existing `midi.record` health/failure-version
checks reject a take when this path faults. An interrupted status query gets at
most four attempts; other errors, invalid returns, exceptions or exhausted retries
invalidate the loss observation and stop the client. No lost-byte count is invented
for an unavailable query. Initial status failure closes acquired handles before
publishing a running client.

Recovery requires native backend replacement and the usual explicit input reset.
Unconfirmed output may require the explicit abandonment option described above.
The sampled count is bounded by native reporting and counter width; it is not an
event count or a complete physical-loss measurement. Data delivered before a later
fault cannot be revoked. Other backends, including sndio, do not gain this ALSA
counter observation. Native calls still have no forced deadline, and the extra
status-query cost, hardware behavior and recording integration remain unverified.

Raw output now queues complete channel-voice messages in a client-owned queue of
at most 1024 music packets. Only the native worker writes the stream, after output
readiness is observed. Each service cycle makes at most 64 nonblocking writes.
Short writes retain the exact byte suffix; `EAGAIN`, `EINTR` and zero progress
retain it for a later cycle, with a 1 ms backoff to avoid spinning on stale
writability. Other sources cannot interleave a partially submitted message. These
queues remain untimestamped: the supplied MIDI time is ignored, and SysEx/system
output is unsupported.

Cancellation removes a source's unsubmitted packets. Any partially written packet
finishes first, then reserved channel cleanup sends zero-valued controllers 120,
64, 66 and 123 before new music. Cleanup survives port retirement and does not
consume the 1024 music slots; repeated local note/pedal releases coalesce into
that reservation. Reusing a retired port's address does not inherit the old partial
packet's channel ownership. Overflow discards pending music, reserves cleanup for
submitted or uncertain channels, throws an explicit enqueue error and retains
`-ENOSPC` as the last native error. New music is refused while overflow recovery
remains pending. Fatal native failures invalidate the duplex client and retain
uncertain cleanup for inspection; they do not automatically replay or resume it.

`midi.inspect.outputBackend` and `transport.inspect.midiBackend.outputBackend`
report the same client-wide state: `observed`, `active`, `pendingEvents`,
`queueLimit`, `cleanupPendingChannels`, `nativeErrorCode`, `lastNativeErrorCode`,
`abandonmentSupported` and `abandonmentEligible`.
The transport view remains available after the last model port retires. Ordinary
backpressure can leave pending events while `active` is true; pending cleanup,
overflow recovery or a terminal failure makes it false. These fields observe
software queues and native-library acceptance, not physical delivery.
Only terminal raw output with a stopped worker is eligible for the explicit
backend-replacement abandonment described above; inactive output alone is insufficient.

Engine handoff waits cooperatively up to 250 ms for old output and cleanup to be
submitted to the native library. Cancellation, timeout or terminal failure normally refuses
the handoff while keeping the old client bindings; a fresh attempt can succeed
after cleanup progresses. Destruction also attempts cleanup, reports incomplete
cleanup, then requests stop, joins and closes. The deadline does not preempt a
native call or join. Queue locks cover individual nonblocking writes. Completing
a cancelled partial note-on may briefly activate it before cleanup, and channel
cleanup also affects other sources using that channel. Already submitted bytes
cannot be revoked; kernel drain and device receipt are not acknowledged. Discarded
music is not replayed.

`RawMidiOutputTest` adds 12 portable source cases / 26 scenarios covering packet
framing, transient writes, bounded service, cancellation, retired identities,
reserved cleanup, overflow, fatal errors and concurrent producers.
`RawMidiIoTest` now has 21 cases: 65 ALSA and 40 sndio scenarios when both platform
libraries are compiled. These exercise actual constructors/workers with private
native API substitutions, including partial construction, descriptor bounds,
fragmented input on the model owner, transient/fatal errors, short writes, packet
serialization, cancellation/retirement cleanup, output observation after port
retirement, startup/idle and pre/post-read native byte loss, status-query failures,
bounded interruptions, owner-thread note cleanup, handoff refusal/retry, explicit terminal abandonment with partial
packets or retired ports, shutdown and cancelled candidate cleanup. They
remain uncompiled/unrun. No physical MIDI, native GUI, latency, CPU/RSS or complete
project/provider acceptance follows from them.

Raw/WinMM callbacks copy scalar voice/reset events; ALSA copies seven native
channel-voice types. Each captures the source name before
returning. They enqueue on the port's QObject thread, with at most 1024 waiting
events, one scheduled callback and 64 deliveries per event-loop turn. A running
Qt event loop is required. Native input models are evaluated at delivery time
under the engine model guard. Borrowed SysEx/meta payloads are rejected by this
dispatcher; its supported message set is unchanged. Backend, registration and
queue locks are released before native processor/DSP work.

Port/client retirement cancels pending delivery; instrument/controller owners
detach their processors before their own members are destroyed. Disabling input
discards its dispatcher backlog, and removing a subscription discards that source's dispatcher backlog
while retaining other sources. Failure suspends further input for that port;
recreating the native port through project reload/track recreation clears the
fault; eligible local faults can also use `midi.input.reset` below. Toggling its
modes does not. On queue/processor failure, the owning thread
attempts native all-sound-off on all channels, bypassing input filtering to avoid
held voices after a lost release. This uses LMMS's broad native silence behavior
and can interrupt other voices on that instrument. Enqueue/allocation failure
can prevent that cleanup callback; `failed` must be handled even if cleanup
cannot run. Other registered ports remain independent.

ALSA guards its sequencer handle and destination map through registration of
owned delivery. Source names include external peers, with an address fallback if
the peer's metadata disappeared. Native channel pressure now uses the control
value; full-width control/program/bend values are checked before narrowing.
Tick note timestamps must fit the native signed tick range. Realtime note stamps
currently map to zero; their union storage is no longer interpreted as ticks.
The output queue now uses the native project's fixed 48 ticks per quarter note;
its configuration and remaining timing limitations are described below.

ALSA startup checks native setup results and releases partial/nonstarted handles,
queues and pipe descriptors during destruction. Its wake pipe is nonblocking and
closed on exec. Shutdown sets an atomic stop flag, wakes the reader and joins
before closing its handle, without holding the sequencer mutex. Reads are
nonblocking, limited to 64 events per lock acquisition, and preserve library
buffer readiness between polls. Native FIFO overrun (`-ENOSPC`) faults existing
port delivery while the reader can continue; fatal read/poll errors stop it and
queue endpoint-list removal. Port recreation can recover a delivery fault while
the reader remains active; a stopped reader requires recreating the backend.
`midi.backend.select` can recreate or replace the native MIDI client with stopped,
disarmed transport; its factory and transfer must succeed before model publication.
`audio.backend.select` changes only audio and refuses replacement of an audio
connection shared by JACK MIDI until an independent MIDI client is selected.

ALSA uses nonblocking output too. Channel messages now use the direct-write API
without a userspace output buffer. Positive delays still enter the kernel queue;
zero-delay events bypass that queue. Native send failures raise an error and
retain possible activation for cleanup. `TicksPerQuarterNote` is shared with
Engine's frames-per-tick conversion. Meter changes affect bar length, not this
quarter-note resolution. The previous 16 PPQ queue made a nonzero LMMS tick delay
three times longer than the corresponding musical duration.

Startup sets 48 PPQ before starting the queue. Tempo changes now synchronously
read, update and read back the native tempo, PPQ and unit clock skew instead of
buffering a tempo event. A failed call or mismatched readback invalidates the
clock configuration. Positive-delay sends retry synchronization against the
current song tempo and fail before sending if it remains invalid. Immediate
messages and cleanup do not depend on clock verification. Events already in the
queue can continue after a failed tempo update; stop remains the explicit cleanup
operation.

`midi.inspect.outputClock` reports `observed`, `configurationVerified`,
`ticksPerQuarterNote`, `microsecondsPerQuarterNote`, `nativeErrorCode` and `scope`.
These describe the last verified queue configuration and its current error,
not hardware latency or clock phase. Values are zero until a configuration has
been verified. A later failure retains the last verified values with
`configurationVerified: false`. Other native backends currently report
`observed: false`. Successful scheduled-send retry clears the current clock error;
the backend's historical first-error field remains latched.

Whole-tick rounding still loses fractional sample offsets. Integer MIDI tempo
introduces less than one microsecond of rounding per quarter note. ALSA delays are
relative to its own running queue; audio-device latency, clock phase and external
changes to that queue are not continuously measured or compensated. This fixes
the tick scale in source and does not establish sample-accurate hardware timing.

Every output message carries its native source port ID as an ALSA tag. Before
source changes, channel/key changes, disable, unsubscription, silence or retirement,
the backend removes pending events matching the queue, subscriber destination and
tag. Other ports' queued events remain. The check runs even with zero locally
tracked notes: a submitted note-off may still be queued behind a delivered note-on.
The backend retains a bounded mask of channels with possible note/pedal activation
until cancellation and immediate sustain-off, sostenuto-off, all-notes-off and
all-sound-off have succeeded for each channel. Cleanup does not depend on a running
queue. Failed cancellation keeps the prior source, output mode, subscription,
channel or fixed-key model; the cleanup state remains available for retry.

Song stop and pause explicitly invoke output cleanup for both song and pattern
store instruments under the native engine guard. This does not depend on the
audio engine's deferred play-handle clearing or on a track having active notes.
One failing port does not skip the remaining tracks. Playback still stops/pauses;
the structured command reports failure and `transport.inspect` exposes
`midiOutputCleanupError`. Repeating stop, or pause with `paused: true`, retries
cleanup. This status is transient, not saved in the project.

Native output IDs are cleared of old tagged events before reuse. A failed clear
leaves that native output unavailable and records the error. Backend-first
shutdown attempts cancellation and cleanup before freeing native resources.
Destructor cleanup logs failures and remains best effort. Immediate backends use
the existing port note releases; they have no LMMS-owned schedule to remove.
ALSA cancellation does not fence events already dispatched by the kernel, retract
device writes, or acknowledge receipt. Channel-mode cleanup can affect another
track sharing a destination/channel. These effects, native timing, error recovery
and physical hardware behavior require execution evidence.
Subscription requests query existing connections to make refresh/repeated removal
idempotent. A peer can disappear between query and mutation. Incoming kernel
backlog and externally established connections retain native sequencer semantics;
dispatcher cancellation does not establish that upstream backlog is empty.

The original eight ALSA cases have 27 authored executions: seven voice rows; twelve malformed/
foreign/payload rows; port/client retirement; concurrent destination-map changes;
input-loss/first-error reporting; three timestamp rows; native failed startup;
and a kernel subscription/input/shutdown case. The kernel case requires
`/dev/snd/seq`, skips if it is absent, and needs no physical MIDI device. Boundary
fixtures do not create a hardware connection. Six output cases add 22 executions
for native tags/delay modes, queued releases, scoped cancellation, native errors,
retry, route validation and seven kernel lifecycle rows. The new kernel rows use
two LMMS ports and a virtual peer with a stopped queue to exercise immediate
cleanup, isolation, source changes, disable, unsubscription, port destruction,
ID reuse and backend-first shutdown. Four portable port cases add 14 executions
for cancellation ordering, empty ledgers, routing failure/retry and destruction.
Two more portable cases/five executions exercise actual Song/PatternStore
instruments through transport stop/pause, failure reporting, continued cleanup of
other tracks and retry while already stopped or paused.
Four further ALSA clock cases add 17 executions: six tempo/sample-rate/meter rows,
eight read/write/readback failure rows, a synchronous-change/cached-send/retry
case, and two native kernel tempo/delivery rows. There are three kernel cases/ten
executions in total, all requiring `/dev/snd/seq`.
Every execution remains uncompiled and unrun.
The native kernel case, even when run, will not establish physical-device or
whole-DAW compatibility. Native driver/ioctl/lock/DSP shutdown deadlines and
CPU/memory/latency bounds have not been established.

CoreMIDI now uses opaque client/source generation tokens as callback refCons.
The callback registry retains only live LMMS clients; lookup and callback access
finish before client retirement can proceed. Tokens are never dereferenced or
reused. Source removal erases its token before native disconnect, so late
callbacks are ignored; a changed unique ID at the same endpoint reference creates
a new source generation. A rename preserves that source's identity and faults
subscribers to its old name. Native calls run outside callback/table locks, and
endpoint refresh runs on the backend's QObject thread after a coalesced
notification. Source loss faults affected ports and attempts native all-sound-off;
other ports continue. Device-specific I/O errors are limited to its subscribed
inputs; errors without a device identity are handled conservatively. Recovery of
faulted delivery still requires native port recreation.

Input uses LMMS's raw byte parser with separate state per CoreMIDI source. Empty,
short, split and variable-length packets use available bytes rather than reading
two parameters unconditionally. Running status survives packet boundaries and
realtime interleave; SysEx/system-common payloads are skipped without allocating
a message buffer. Names are owned Unicode strings. Enumeration includes virtual
sources/destinations as well as physical endpoints. Unique physical names retain
the legacy `Device:Endpoint` label; duplicate labels gain UID/ref suffixes.
Ambiguous saved names remain missing requests until explicitly selected; a label
can change when a formerly unique name becomes duplicated.

Output uses a native output port and the checked LMMS serializer, with correct
two-byte program/pressure packets and 14-bit pitch bend. Native creation/connect/
send/disconnect/dispose errors become first-error evidence. The native factory
falls back when neither input nor output registration is available. CoreMIDI's
legacy MIDI 1 APIs remain in use; MIDI 2/UMP and SysEx output are not implemented.
Input host timestamps still map to the default native position, and output
ignores the provided musical timestamp. Timing calibration remains required.

Individual input/output ports are disconnected/disposed on backend retirement.
One process-owned native MIDI client and its registry remain until application
termination, allowing backend reconstruction without explicitly disposing the
last client. Apple documents a risk that later client creation fails after the
last client is disposed. Callback/table mutexes, Qt posting and parser/event
allocation remain on CoreMIDI's high-priority receive thread; no hard real-time
deadline is established. Opaque uintptr refCon round-tripping, client/server
restart recovery, failed native disposal/resource retention, startup with partial
I/O, notification snapshots and physical MIDI all need platform verification.
Registry-token exhaustion fails closed instead of reusing a retired token.

Twelve conditional macOS cases have 19 authored executions: eight output rows;
empty/short/split framing; a variable-length list including ignored SysEx; separate
source parser state; Unicode/duplicate names; retired source/client tokens;
callback races with port/source and client deletion; coalesced notifications and
I/O errors; isolated source loss; per-device I/O failures; and a native virtual
source roundtrip/backend reconstruction case. Boundary fixtures avoid native
device creation. The virtual-source case requires a CoreMIDI service, skips if
it is unavailable, and does not need physical hardware. Every execution remains
unrun; packet-builder tests do not establish actual MIDISend or physical output.

`midi.input.reset` takes `track` and optional `container`, or `parameter` for a
direct MIDI controller connection. It addresses the same native port as
`midi.inspect` and is transient (`mutatesProject: false`). It requires stopped
transport, disarmed recording/automation, no pending
or reserved native takes, and no export/device reservation. A registered readable
port with a healthy local queue returns `recoveryChanged:false`. A failed queue
suspends intake, clears its pending dispatcher events, executes native
all-sound-off on all channels for voice processors, and verifies the port and backend again before
clearing the fault. The result includes normal MIDI inspection plus
`recoveryChanged`. This broad native silence can affect other voices on the
instrument. It does not reconnect hardware, restart a reader, reset shared byte
framing, or establish that upstream kernel/library buffers are empty.
Parameter-only MIDI controllers retain their last received value during failure
and recovery. They do not consume synthetic all-sound-off as CC 120 input.

Reentrant delivery/recovery, a retired processor/port, unsupported queued delivery,
disabled input and known stopped backends are rejected. A backend/callback fault
at any point in the current failure cycle requires an observed active backend;
an earlier local overflow cannot hide a later backend fault. Local queue/processor
faults may reset when backend observation is unavailable, with connectivity still
unverified. Intake during cleanup is discarded and counted. A new failure version,
backend stop or cleanup exception leaves input suspended. Cleanup/output/DSP and
lock acquisition have no hard deadline.

`inputDelivery` additionally reports decimal-string `failureVersion`,
`recoveryCount`, `lastFailureCode`, and boolean `processing`, `recovering`,
`requiresBackendObservation`, `retirementCleanupPending` and `resetEligible`. The active `failureCode` clears
after successful reset; failure history and dropped counts remain. `resetEligible`
is a snapshot of port/backend admission, while transport admission is checked
when the operation runs. `restartRequired` remains true for known stopped,
unregistered/unsupported or exhausted delivery state, rather than every local
fault. Mode toggling retains faults. Counters are transient and reset when ports
are recreated; native MIDI settings/subscriptions remain project data.

If a client is destroyed while its MidiPorts survive, the base lifetime path
first discards its queued packets and detaches all still-owned ports. It adopts
their shared delivery state into a process-lifetime Dummy fallback, preserving
drop/recovery counters and registering failure 7 (`ClientRetired`). A discarded
replacement candidate does not detach ports still bound to their actual owner.
This fallback has no native input health observation; retirement supersedes the
old backend fault for local recovery and clears its health-observation requirement.
The failure version advances, and earlier native backend health is not evidence
about a reopened device.

Positive note-on and sustain/sostenuto activation taken from the input queue mark
possible local voices. Retirement cleans those processors on their model owner
thread, after any delivery/recovery already in progress unwinds. It bypasses
later input-mode/channel filtering and enters native all-sound-off without holding
client registry locks. Undelivered queued notes are discarded without inventing
an activation. Cleanup removes matching native notes from both pending and active
audio handle lists, so a note cannot first enter the render after its cleanup.
Track/type filtering and reserved recording completion remain respected.

`retirementCleanupPending` remains true when local cleanup throws, cannot enter
the processor or is deferred during export. An attempted cleanup can leave
`lastFailureCode` 3 while the active reason remains 7. Cleanup is not retried in a
tight loop: stop/disarm, enable input if needed, then use a fresh `midi.input.reset`
request after export ends. Successful automatic cleanup clears the pending flag
but keeps failure 7 until explicit acknowledgment. Reset on Dummy is local only;
it does not reopen hardware or attach the detached port to the engine's selected
backend. Native project settings and requested endpoint names remain unchanged.

The activation marker is conservative, including after ordinary note releases;
all-sound-off can silence other voices sharing that native instrument. It is not
a count of sounding notes or proof of external-device silence. Derived clients
must revoke/join their native sources before base destruction. Worker retirement
posts model work and needs the owner's event loop; overlapping destruction of a
port itself or calls into the retiring client from another thread are not
established as supported. Fallback
registry allocation failure is warned and leaves the port safely detached but
without guaranteed registered recovery/cleanup. The fallback registry is kept
alive until process exit; ports unregister their individual entries. These paths
and their native fixtures remain uncompiled and unrun.

Reset adds no project revision/history checkpoint. Request replay returns its
saved response without repeating reset; a later fault needs a new request ID.
Failures can occur after partial all-sound-off, and no project rollback can undo
that external/live effect. The nine new native cases have 15 authored executions
for parser resumption, model persistence, reentrancy, cleanup-time failures,
seven admission rows, cleanup exceptions, mixed local/backend faults, backend
stop, revision/history/replay/restart, and transport/argument refusal. The full
executable harness adds healthy no-op reset, replay and durable-history checks.
Every new native/executable check remains unrun.

WinMM subscription/device tables have a mutex; output handles have a separate
lifetime mutex. Device reset/close runs outside the table mutex so callbacks can
finish, and output sends permit concurrent/loopback input. Failed capability or
input-start queries no longer publish an unusable input device. Physical API
close/output errors are recorded. Reconnection acknowledgment remains unimplemented.

Owned source names add storage to the public `MidiEvent` type. LMMS, native
plugins and helper hosts must be rebuilt together; compatibility with existing
prebuilt native LMMS plugins is not established. Queuing also changes input
latency. Neither the bounded queue nor its batch size is a hard deadline for
native DSP, allocation, mutex acquisition or physical output. No compile, Qt
execution, native/plugin ABI check, latency/CPU/memory trace or physical MIDI test
has run in this environment.

WinMM's driver instance data now contains a monotonic per-input token, with a
synchronized process-lifetime lookup of live client/handle registrations. Tokens
are never pointers to clients and are not reused. Callback access completes before
retirement removes its registration; reset/close APIs then run outside registry/
table locks. An old callback cannot enter a newer registration when a native
handle value is reused. Failed close may retain a driver handle, but its retired
token cannot access the destroyed application object. Failed native closure now
retains ownership for explicit retry; see the native-retirement section below.

Input capability/open/start/reset/close results retain their first error. Inputs
publish active state after successful start; unexpected MIM_CLOSE removes active
availability and faults its subscribers, retaining their project subscription
intent. MIM_ERROR/MIM_LONGERROR fault those subscribers without inventing an
API return code. A malformed-input fault can use local reset while an observed
input remains active. That registration does not prove physical health.
MIDI_IO_STATUS enables MIM_MOREDATA; its complete channel-voice event is
delivered rather than discarded. Callback timestamps, automatic hotplug
enumeration, SysEx input/output and hard callback/driver deadlines remain
unfinished. Native factory selection falls back when no input/output registration
is usable; partial output-only availability is separate from input activity.

WinMM now reports independent `outputBackend` health through `midi.inspect` and
`transport.inspect`. Every successfully opened output retains its latest send
result. `active` requires at least one owned output, no current send failure
and no retained channel cleanup on any owned output. A successful retry clears that output's current error;
success on another output does not. `lastNativeErrorCode` retains the most recent
output capability/open/send/close error after recovery. A failed open is not an
owned endpoint: other usable outputs can remain active while that historical
error stays visible. With no owned outputs, output activity is false and its
native error is the last output error, or zero if none was reported. Output
errors do not change input health. A successful native open with a null handle
is rejected as `MMSYSERR_INVALHANDLE` in either direction.

Each send attempts every subscribed destination even if an earlier destination
fails. Any failed result or exception is reported to the caller after those
attempts, so a failed note/pedal release retains the native port's uncertain
activation ledger. Cleanup can be retried; already successful destinations may
receive the release again. The
[WinMM short-message API](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutshortmsg)
defines these native return codes and can block while sending. Success means
library submission, without an acknowledgment from the physical device.
`pendingEvents` and `queueLimit` are zero because this backend has no software
music output queue. `cleanupPendingChannels` counts the union of channel numbers
with retained cleanup across owned outputs (0..16), described below. Neither a
zero queue nor successful submission certifies external note cleanup. Forced
output abandonment remains unsupported.

If a port is destroyed after release fails, its remaining note/pedal channel bits
are copied to subscribed native outputs before the port's ledger is cleared.
This includes channels whose releases were never attempted after an earlier
failure. The output retains no pointer to the retired source. An unrelated
successful send cannot erase that cleanup state, and reusing a port address does
not replace it. Before sending new music to an affected output, WinMM submits
all-sound-off, sustain-off, sostenuto-off and all-notes-off on each pending
channel. There are at most 64 native cleanup calls per output per pass. A channel
clears only when all four submissions succeed; a failed channel remains pending
even if a later controller succeeds. Complete channels stay cleared across
cooperative cancellation, while incomplete channels retry the full sequence.
Ordinary events for an output are withheld if its cleanup fails. Other subscribed
outputs are still attempted, and the caller receives the native failure.

Backend replacement attempts retained cleanup on every native output even after
the last port has retired, and refuses the handoff if any cleanup fails. The
old client and pending state remain available for retry. Shutdown makes a final
attempt after revoking input callback tokens and before closing native handles;
failed cleanup is reported and handle close is still attempted. This final
cleanup is best effort. Retained bits are process-local and are not preserved
after process exit. Channel cleanup can silence other tracks sharing the same
destination/channel; it never replays the retired source's music. Failed native
closure retains ownership as described below. The agent reports pending cleanup separately from queue
length and from proof of physical delivery.

Native outputs also retain a separate mask of channels that may have received
positive note-on or sustain/sostenuto activation since the last complete channel
cleanup. The bit is reserved before native submission, including a failed or
throwing send that may already have changed the device. Program, pressure,
pitch-bend, ordinary controllers, note-off and pedal-off do not expand this mask.
Ordinary note-off success does not clear it, since other sources or pedals may
still be active on that channel. This is a conservative use mask, not an exact
voice count, and it does not itself make an otherwise healthy output inactive.
Transfer stages all used channels for cleanup before the ownership commit;
native close stages them even when the client dies before its MidiPorts. A
complete four-controller cleanup clears both masks. Later activations reserve
their channel again, and a later failed port retirement can retain new cleanup.
Final close does not repeat previously completed cleanup when neither has
occurred. This can produce channel cleanup after ordinary individual releases
and can affect other sources sharing that destination/channel.

Closing is published before native cleanup/reset/close calls: input/output
activity and client availability become false, callback tokens are revoked,
and queued input from old registrations is invalidated. Ordinary output on a
closed client is refused. The existing internal device-refresh path can reopen
native handles; it retains requested endpoint names but requires explicit input
delivery reset before new input reaches the model. This does not add automatic
hotplug enumeration. Local input voice retirement is handled separately by the
base client lifetime path described above. Native calls can still block, final cleanup can fail,
and successful submission does not prove that hardware silenced its voices.

WinMM native handle ownership now survives a failed close. Ownership records are
allocated and registered before each native open and retain a copy of the API
call table. Unique owner generations prevent a later client allocated at the
same address from inheriting an old client's pending cleanup. Close revokes input
callback tokens and detaches live device/subscription tables, then services those
records outside the registry locks. A successful close, or a close reporting an
invalid handle, removes the record. Other error returns and caught native-call
exceptions retain it without retaining a MidiPort or dereferencing the old client.
This also covers partially constructed devices that never reached publication.

`transport.inspect.midiBackend.nativeRetirement` reports process-wide WinMM
retirement independently of the currently selected backend. Its fields are
`observationAvailable`, `retrying`, `pendingInputHandles`, `pendingOutputHandles`,
`cleanupPendingChannels`, `nativeErrorCode`, `lastNativeErrorCode`,
`retryOnBackendSelection` and `scope`. Builds without WinMM report observation
unavailable. Current errors refer to retained records; the last error remains
after successful cleanup. The channel count is the union of pending channel
numbers across retired outputs, not an event count or the number of sounding
notes. A committed backend-selection result includes this state inside its
transport snapshot. Selecting Dummy can succeed while old WinMM handles remain
unresolved; `retirementExceptions == 0` does not establish native closure.

A later explicit selection of the exact WinMM backend retries retained ownership
before enumerating/opening new devices. If any retained handle is unresolved, or
another cleanup pass is in progress, that candidate cannot open new devices. A
failed selection preserves the currently selected client. The existing private
device-refresh path also retries. Discovery and receipt replay perform no retry;
use a fresh selection request for a new attempt. There is no periodic retry worker.

Each pass visits only records present at its start, once per record, and attempts
other handles even if one native call fails or throws. Successfully started input
handles are reset before another close attempt. A previous output close reporting
`MIDIERR_STILLPLAYING` requests an output reset on retry. Remaining output cleanup
channels are also retried before close, with completed channels removed from the
retained mask. Reset failure does not skip close or other handles. If close
succeeds despite failed channel cleanup, ownership ends but the last error
remains; this does not prove the external instrument stopped sounding.

These choices follow Microsoft's documented
[input close](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midiinclose),
[output close](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutclose),
and [output reset](https://learn.microsoft.com/en-us/windows/win32/api/mmeapi/nf-mmeapi-midioutreset)
behavior. Output reset affects every MIDI channel on that device and may silence
other sources. Bounded attempt counts are not deadlines for a blocking driver.
Pending ownership and history are process-local; unresolved records remain until
a later successful retry or process exit. Eventual native closure, hardware
delivery, real driver failure behavior and this fixture's 24 added scenarios
remain unverified in this environment.

Runtime backend replacement now accepts a WinMM client with a usable input or
output registration, matching startup availability. An output-only client can
therefore retain native port models and requested endpoint names during the
handoff even though its input observation is inactive. No usable registration
still refuses the replacement. Factory/startup failure metadata uses an output
error when there is no input error. Failed output cleanup prevents ordinary
replacement, retains the old client and can be retried after the native send
recovers. Native close retry is provided by the retained-ownership path above.
Automatic reconnection, timestamp scheduling, an output queue and a hard
driver-call deadline remain unsupported.

Four new conditional cases cover retired-token/handle reuse, callback traffic
during client destruction, input-error recovery with MOREData delivery, and
unexpected close/missing subscription intent. They use diagnostic handles that
never reach native reset/close APIs. All are unrun; physical API failure/resource
cleanup and Windows driver behavior have not been verified.

The separate Windows-conditional `WinMmMidiIoTest` supplies a private API table
with the platform's function signatures/calling conventions. Its 29 authored
cases / 104 scenarios drive the actual constructors, callbacks, output methods
and engine transfer. Coverage includes seven message types, rejected malformed
packets, routing, partial multi-destination failures, note/pedal retry, independent
input/output health, loopback delivery, failed/null opens, output-only selection,
cleanup refusal/retry and callbacks during/after retirement. Eight added cases /
26 scenarios cover retained cleanup after note/pedal failure, all unreleased
channels, failure/exception in each cleanup controller, partial cancellation,
studio capability refusal/retry after the last port retires, final shutdown
across outputs, reused source addresses and cleanup loopback delivery. Another
eight cases / 31 scenarios cover client-first output retirement, failed
activations, channel/destination filtering, non-activating events, closing
observations, loopback/queued-input revocation, continued close after cleanup
failure, completed-cleanup reuse, pre-commit failure/retry and native close/reopen.
These fixtures also
remain uncompiled and unrun; simulated native results do not verify a Windows
driver or physical device.

## Bounded native MIDI note recording

`midi.record` takes required `track`, `clip`, and `durationMs` (1..86400000),
with optional `container`, `playMode`, `position`, `timeoutMs`, `maximumNotes`,
`quantization`, `recordPitch`, and `maximumPitchPoints`. It appends notes to an existing native instrument MIDI clip.
`playMode` is `midiClip` by default, or `song` / `pattern`; the latter two require
a target in the matching container. Pattern playback selects the target's native
pattern. Position uses that mode's native tick coordinates, defaults to zero for
clip/pattern playback and the target placement for song playback, and must be
within the native song range. Song note positions subtract both clip placement
and start-time offset; queued note signals retain their original onset snapshot.

`maximumNotes` defaults to 65536, supports 1..1048576, and counts accepted onsets,
including notes still held. `quantization` defaults to zero (no quantization);
positive values up to 3072 apply LMMS's native position and length grid in ticks.
Every recorded note has at least one tick of duration. `timeoutMs` defaults to
duration plus 10000 ms, capped at 86400000; its supported range is 1..86400000.

`recordPitch` defaults to true. Native MIDI pitch bends become independent
per-note detuning curves, including chords and sustained notes. They use discrete
interpolation at elapsed note ticks; a constant value is held until another bend
arrives. Repeated identical values add no nodes and another bend at the same tick
replaces that node. A note beginning while the wheel is bent captures its onset
pitch. `maximumPitchPoints` defaults to 65536, supports 1..1048576, and bounds all
distinct stored pitch nodes across the take, including initial zero nodes.

Curves store the difference from the track's starting pitch in semitones. The
track pitch is restored on completion and on failure, so replay does not apply
the final bend again. `pitchBaselineCents`, `pitchPoints`, `recordPitch`, and
`maximumPitchPoints` are returned. Native detuning supports +/-60 semitones;
a larger relative bend or exhausted point budget rejects the take. Note position
and length quantization do not rescale the curve's internal ticks.

Pitch capture currently requires the track pitch model to have no automation,
controller, or linked-model binding. These sources can otherwise change the
playback baseline and apply pitch twice; their combined recording semantics have
not been implemented. `recordPitch: false` explicitly selects note-only recording
with those bindings retained. It leaves native pitch parameter changes in place.
The Piano Roll uses the same curve builder for an independent pitch model, with a
1048576-point bound, and retains note-only behavior for a bound pitch model.

The operation requires stopped, disarmed native transport, an enabled registered
MIDI input with observed active backend state, and a running audio clock. It does
not require an audio input device. It discards the target port's pre-existing
queued input and voices before recording. Existing notes remain available as
accompaniment while new notes are staged separately. Other recording, native
DSP/input failures, backend loss, target removal/retiming, transport restart,
timeout, cancellation, and failed output cleanup prevent publication. Normal
completion also closes held/sustained notes from the native note handles' elapsed
musical time, then stops transport and restores the selected timeline's tick
position/loop flag and the prior pattern selection. A complete prepared batch is
appended; the session runtime supplies checkpoint rollback, undo/redo and receipt
replay. A replay does not open another take.

Duration uses native processed audio frames, with a control-thread check every
10 ms. Completion occurs at an observed processing boundary at or after the
requested duration; this is not an exact sample cutoff or a hard deadline.
Results report `frames`, `requestedFrames`, `sampleRate`, `durationSeconds`,
`elapsedMs`, `noteCount`, `acceptedNotes`, and `completedHeldNotes`, plus the mode
and limits. Incoming hardware timestamps are not used to compensate queue/device
latency. Native note durations accumulate musical time across processing periods
when tempo changes, rather than converting their entire frame count at the final
tempo. Pattern wraps do not reset that duration.

```json
{"id":"keyboard-take","op":"midi.record","args":{"track":0,"clip":0,"durationMs":8000,"maximumNotes":2048,"quantization":12}}
```

This records native input notes with onset velocity/panning and existing native
sustain behavior. The instrument's key-based overlap rules remain: independent
same-key voices on several channels are not represented separately. This does
not add CC/program/SysEx lanes, generated-plugin
MIDI recording, timestamp compensation, or physical-device delivery proof.
`transport.inspect.recording.midiRecordingTracks` exposes the shared recording
reservation used by both the Piano Roll and this operation.

Seventeen native cases / 46 executions are authored for note formation, tempo and
offset handling, three playback modes, sustain/held completion, bounded pitch
curves, playback pitch, failure cleanup, batch publication, reservation lifetime,
checkpoint recovery and replay. None has
compiled or run. CMake/compiler/Qt tools, 17 pinned submodules, and a native LMMS
executable remain unavailable; source diagnostics do not establish DAW operation.

## Bounded native automation recording

`automation.record` records the first connected parameter of an existing visible
automation clip. Required arguments are `track`, `clip`, and `durationMs`
(1..86400000). `container` defaults to `song`; `pattern` selects the target's
native PatternStore pattern. The target must be unmuted, have live automatable
bindings, and belong to an unmuted automation track. Native transport must be
stopped and all recording disarmed, with a running audio clock and no export.
Audio input is unnecessary. A connected native MIDI controller additionally
requires enabled queued input, observed active backend state and healthy delivery.

Optional `position` is a native tick coordinate: song time for an arrangement
clip and pattern-local time for a pattern clip. It defaults to the clip's song
placement or pattern tick zero and must lie inside the clip's playback window.
Recording and playback use the selected pattern's local coordinates even for
patterns after the first. Source ticks subtract the clip's start-time offset;
the portion before source tick zero records no points.

`maximumPoints` defaults to 65536 and supports 1..1048576. It bounds the entire
staged curve, including its existing nodes. `quantization` defaults to zero;
positive values up to 3072 quantize recorded source ticks with LMMS's native grid.
The GUI editor's current grid does not affect this take. `timeoutMs` defaults to
duration plus 10000 ms, capped at 86400000, with a supported range of 1..86400000.

Song playback samples the parameter at native musical ticks, including its
connected controller. Values retain LMMS's inverse-scale representation so log
parameters play back through their existing scale conversion. This is native
overdub behavior: changed values add or replace a point, while an unchanged
value removes a conflicting point at the visited source tick. Other existing
nodes remain. The curve retains its interpolation mode and tangent behavior;
quantization does not replace an entire visited interval. Other connected
parameters share the resulting curve during later playback, while only the
first binding supplies the captured performance.

The original curve remains unchanged during recording. A detached native take
owns its nodes until input, DSP, transport, target/binding, timeout and cancellation
checks succeed. Failure or point-budget exhaustion discards it. Completion stops
transport, disarms capture, restores the prior selected pattern and timeline tick
position/loop flag, and restores the first parameter's controller-use flag before
publishing the curve. Other live parameter changes follow normal native playback;
session checkpoint rollback restores project state on a failed transaction.
Successful takes support durable undo/redo, session recovery and request replay.

Duration uses native processed audio frames with a control-thread check every
10 ms. A song take also completes when playback is observed past the clip's end;
a pattern take can repeat with native pattern playback. This cutoff is not sample
exact. Results include `samples` (accepted tick samples, including repeated values),
`points`, `maximumPoints`, `quantization`, `frames`, `requestedFrames`, `sampleRate`,
`durationSeconds`, `elapsedMs`, `playMode`, and `stopReason` (`duration` or `clipEnd`).
An empty take fails. `transport.inspect.recording.reservedAutomationClips` reports
staged takes, which block another capture or rendering.

```json
{"id":"volume-performance","op":"automation.record","args":{"track":1,"clip":0,"durationMs":8000,"maximumPoints":4096,"quantization":12}}
```

This records parameter automation, including a bound MIDI CC controller's native
value. It does not retain raw controller-event packets or their timestamps.
Hidden global clips, multiple simultaneous recording targets, program/SysEx
events, physical latency compensation and device acknowledgments remain outside
this operation. `hardwareDeliveryVerified` is false.

Ten native cases / 24 executions are authored for staging, source coordinates,
quantization, native song/pattern playback, linear/log values, MIDI input health,
failure cleanup, clip-end completion, competing reservations, publication,
checkpoint rollback and durable undo/replay. They have not compiled or run.

## Native MIDI CC controller connections

Native project controller restoration checks persisted integer IDs and type
conversion before binding. Malformed IDs no longer select controller zero.
Comments, formatting text and processing instructions do not consume rack slots;
each unavailable controller element retains its own temporary slot so later
valid links keep their intended target. Finalization removes links to unavailable
slots in the same pass and reports native project errors instead of leaving
dummy modulation. Saving a repaired project uses the surviving native rack order.
Existing integer syntax, valid rack controllers and embedded owned MIDI
controllers remain native. This changes no structured operation or XML format.

ControllerConnectionRestoreTest authors eight cases / 37 scenarios, including
native save/load with annotated XML, unavailable and repeated dummy slots,
continued parameter editing and repaired save/reload, malformed IDs/types and
accepted legacy integers, embedded MIDI settings/ownership and retirement of
another connection during finalization. All remain uncompiled and unrun. The
weak resolution snapshot protects registry traversal; it does not establish
arbitrary callback deletion/reentry, later cyclic-model traversal, complete
project/plugin compatibility or actual native sound and resource behavior.

`controller.midi.connect` takes required `parameter` and `inputController`
(0..127), optional `inputChannel` (0 for all channels, or 1..16; default 0) and
optional `readable` (default true). Discover a current parameter handle with
`parameter.list` or `instrument.inspect`. The target must support automation
and native linear or logarithmic scaling. The operation replaces its direct
controller connection with a new native MIDI controller. It preserves the
parameter's stored manual value and enables controller-value evaluation.

The connection owns this controller; it is not inserted into the shared rack.
`parameter.inspect.controller` reports a null rack `index`, `ownership` and a
`midiSelector` object containing the target parameter. Use that selector with
`midi.inspect`, `midi.configure`, `midi.subscribe` or `midi.input.reset`. These
operations address only the direct connection; they do not search linked models
for another parameter's controller. `controller.disconnect` removes the binding;
`controller.connect` can replace it with an existing rack controller.

Creation uses a fresh native input port with no named subscriptions. For a
backend with named ports, inspect its available inputs and subscribe using the
exact reported name. Reconfigure an existing MIDI connection with `midi.configure`
to retain its subscription intent. Calling `controller.midi.connect` again
replaces the connection and its port. Configuration by parameter supports only
`readable`, `inputChannel` and `inputController`; -1 disables CC selection.
Output routing and note/velocity settings are rejected for this target.
Disabling readability follows LMMS's native behavior and clears selected input
subscriptions. Missing named endpoints retain intent until removed or disabled.

```json
{"id":"bind-knob","op":"controller.midi.connect","args":{"parameter":1234,"inputController":74,"inputChannel":1}}
{"id":"inspect-knob-input","op":"midi.inspect","args":{"parameter":1234}}
```

Here `1234` is illustrative; use the discovered handle. A reported endpoint can
then be selected with `midi.subscribe`, and the parameter can be bound to an
automation clip for `automation.record`. The recording path uses the controller's
native value and existing scale conversion. This provides 7-bit CC input and
native buffer interpolation, not raw event-lane or timestamp recording. Creation
does not establish physical connectivity. Named subscription checks, backend
activity and queue health remain separate observations.

Native project XML stores MIDI connections beneath the parameter's connection
element, including input settings and exact named-subscription intent. The last
received CC value, queued events and recovery counters are transient; reopening
a project awaits new input and begins with the native controller's zero value.
Ordinary project saving can honor LMMS's discard-MIDI option. Recovery checkpoints
always retain MIDI controller connections and instrument-port configuration,
regardless of that option, without changing the option itself. Connection,
configuration and subscription mutations use durable history and receipt replay.

Eleven native cases / 49 executions are authored for linear/log input, channel/CC
filtering, argument prevalidation, selectors, ownership during MIDI/rack rewiring,
named routing, failure/recovery without synthetic CC changes, XML export options,
checkpoint rollback, undo/redo, reopening and replay. The automation-recording
fixture now creates its controller through these capabilities. The executable
harness covers CC setup, discovery, edits, rejection, history and native XML.
None has compiled or run; physical devices and full-system behavior are unverified.

Detection, parameter-owned MIDI controller input and instrument CC dispatch share
`MidiEvent::isValidControlChange()`. It checks event type, channel 0..15 and signed
raw controller/value fields 0..127 before convenience accessors mask or narrow
them. Controller and detector entries scope native engine changes before checking
filters and updating state. Rejected CC input does not change the last/previous
normalized values, detection or the input's value-change signal. Channel/CC filters
and first/latest detection behavior remain native.

The connected MIDI controller previously used masked/narrowed fields without its
own raw validation. Values such as -1 or 256 could become a different valid-looking
controller/value, including interpolation values above 1. Scalar current-value
fitting clamps to 0..1, but the native parameter sample-buffer path maps controller
samples directly. Scalar clamping alone therefore does not establish buffer bounds.
Four new cases / 22 scenarios require preserved binding, stored parameter value
and normalized buffers after thirteen raw malformed events, valid filtering and
endpoints, and unchanged native parameter samples under linear/logarithmic mapping.
The tests advance native period counters manually with audio processing stopped;
they do not establish a live audio clock, measured timing or real device behavior.
All remain authored, uncompiled and unrun.

Live external MIDI input is now excluded during native export and while an audio
device reservation remains held, including renderer preparation and a failed
restoration awaiting retry. Native instrument, controller, detector and MidiPort
entries check this state under model ownership. Internal native events remain
usable for project playback and automation.

The selected MIDI client owns a shared atomic admission generation. Reservation
transitions update it under native model ownership; driver threads read that
client-owned state rather than Song/device pointers. Queued external packets
carry their admitted generation. A port removes stale external packets once per
observed transition before checking capacity, and a popped packet rechecks its
generation under native model ownership immediately before delivery. It therefore
cannot apply after an export has already started and ended while delivery waited.
Newly registered ports share the current admission state. Retired-client fallback
delivery adopts its new client's state without keeping a dead client pointer.

Intentional exclusion increments the existing dropped-event counter without
inventing a queue overflow or backend failure. It does not prove driver packet
loss. Stale packet storage can remain until the next intake or scheduled drain.
Both export and device reservation must finish before reopening input. Failed
device restoration leaves its reservation and exclusion intact; failed export
initialization releases the export reason. A throwing export stop retains its
existing export state and marks `exportCleanupPending` until cleanup succeeds.
Source classification is trusted
native metadata, not an authorization mechanism. Independently managed clients
receive direct native state checks but do not automatically receive the selected
engine client's admission generations.

MidiInputRenderTest authors ten cases / 20 scenarios. These cover combined and
completed reservations, stale full queues, blocked bursts, Internal delivery,
four direct processor paths, a controlled popped-packet/export race, failed export
initialization, busy/throwing device restoration, and two native sample/Mixer/CC
renders with live traffic injected at a reservation notification. The render
fixture requires decoded nonzero PCM to match and fresh input to work afterward.
All remain uncompiled and unrun. The shared state and queue stamps require native
rebuild; actual driver timing, atomic/layout behavior, PCM results, GUI/plugin/
legacy compatibility, CPU/RSS and full autonomous production remain unverified.

Native transport stop now uses its existing scoped model ownership throughout;
the extra manual recursive lock pair was removed because a returned timeline
callback exception could bypass its unlock and block another native worker.
This balances ownership on exceptions without rolling back every stop side effect.

`transport.inspect.exportCleanupPending` and
`transport.inspect.backend.restoration.exportCleanupPending` identify a failed
native export stop. The restoration object reports `pending`, `failed`,
`failure: "export-stop-exception"` and `retryEligible` where applicable. Cleanup
can remain pending even when an initialization exception already cleared the
export flag and no original audio device was stored. Live External MIDI remains
excluded in that state; new playback/export/backend or DSP-reset admission refuses
it. These flags are transient native runtime state, not saved project history.

The existing `audio.device.restore` operation retries marked Song export cleanup
before restoring or releasing a retained audio device. It rejects a running
render execution and ordinary active export, and reserves the transition against
reentrant restore callbacks. A failed retry keeps cleanup pending and the device
reservation intact, with `export-stop-exception`. A successful stop/sync clears
the pending/export flags; device notification/restart must then succeed before
any retained reservation reopens live input. Use a fresh request ID for a retry;
receipt replay reports the previously acknowledged result. An earlier failed
render remains failed and its destination is preserved, even if automatic unwind
cleanup succeeds. The agent is instructed to inspect these fields and retry
restoration when eligible.

SongExportRecoveryTest authors seven cases / 12 scenarios for model ownership
after returned standard/nonstandard stop exceptions, direct/reserved cleanup
retry, MIDI exclusion/resumption, running-context and restore-reentry refusal,
ordinary stop distinction, pending cleanup after startup cleared the export flag,
and native render failure/destination preservation followed by another render.
All remain uncompiled and unrun. This does not establish transactional rollback
of timeline/controller/plugin/MIDI effects, safe throwing destructors or arbitrary
callback deletion, asynchronous Qt exception recovery, hard cleanup deadlines,
actual native lock/PCM/platform/resource behavior or full DAW acceptance.

Native Song automation now retains weak model references between passes and for
stop cleanup. The current native value map is snapshotted into guarded targets
before recording/controller callbacks; recording curves and their first targets
are guarded too. Retired targets are skipped, including when a later allocation
reuses the same address. Removing a curve returns its surviving target to the
connected native controller, and native curve values, scaling and precedence
remain authoritative. Null legacy curve targets are omitted from Song/pattern
collections. No new structured operation or project XML field is introduced.

SongAutomationOwnershipTest authors ten cases / 15 scenarios covering removed
tracks, address reuse, controller handoff, another target/curve retired by a
callback during apply/recording/stop, null Song/pattern targets, project
replacement, linear/logarithmic scaling and controller recovery after a returned
callback exception. All remain uncompiled and unrun. Weak references do not pin
objects or replace native model ownership. Self deletion within a model setter
or curve recording method, reentrant transport/project replacement, arbitrary
track deletion during DSP, actual GUI/plugin/audio compatibility and allocation/
latency/CPU/RSS behavior still require separate work and native verification.

## Native MIDI learn

`controller.midi.learn` takes a required `parameter`, optional `inputChannel`
(0 for any channel, or 1..16; default 0), optional exact input `port` name and
`timeoutMs` (1..600000; default 30000). The target must support automation with
linear or logarithmic scaling. Discover its current handle before starting.

```json
{"id":"learn-knob","op":"controller.midi.learn","args":{"parameter":1234,"inputChannel":1,"timeoutMs":30000}}
```

Move a control on the selected native MIDI input while the operation waits. The
first matching, valid 7-bit CC supplies the channel, controller number and input
source for the new binding. CC 0 is valid. Notes, malformed CC fields and other
channels are ignored. Learning requires stopped, disarmed transport, no pending
capture or audio-device reservation, and an observed active backend with queued
MIDI delivery. It does not need a running audio clock.

Named backends listen to the specified available input, or all inputs available
when learning starts if `port` is omitted. The detected source must have a
reported name among those candidates and still be available before publication.
The resulting controller subscribes only to that name. Raw backends reject a
named `port` and report an empty source name. These observations do not establish
physical connectivity; `connectionVerified` remains false.

Learning uses a temporary native detector detached from the project. The prior
binding remains connected while input is observed. It can continue receiving
events. Only after detection and validation does the operation replace it with a
connection-owned native MidiController. The response includes `parameter`,
`inputChannel`, `inputController`, `port`, `learnedValue` (0..127), `elapsedMs`,
`ownership` and `connectionVerified`. The new controller starts at the detected
normalized value, even without an audio callback; the parameter's stored manual
value is retained.

Cancellation, timeout, input failure or dropped events discard the temporary
detector. Changes to the target, its prior binding, transport or selected input
also prevent publication. A second learn operation cannot enter while the first
is active. The session's existing busy-operation rules and cancellation path
apply. Polling and resource checks are cooperative; a blocked native callback is
not subject to a hard deadline.

Successful learning uses ordinary mutation checkpoints, undo/redo and durable
receipt replay. A repeated receipt does not listen again. Failure preserves the
project checkpoint and history. Learned channel/CC/subscription settings persist;
the last input value and detector state are transient. Reloading or restoring a
checkpoint creates the native controller at zero until new input arrives.

Nine native cases / 36 executions are authored for shared GUI/core detection,
raw/named input, filtering, prior-binding preservation, malformed events, timeout,
cancellation, queue/backend failure, target changes, ambiguous/lost sources,
reentry and session recovery/replay. The executable harness adds inactive-backend
refusal and history/binding preservation. These tests have not compiled or run;
GUI behavior, hardware delivery and the complete studio workflow remain unverified.

Four new learn scenarios submit thirteen malformed raw events before or after a
valid detection in both first-only and latest-value modes. They require unchanged
detection and no input value-change signal, followed by valid matching and an
accurate retained-value controller copy. The shared detector already checked raw
fields; it now uses the same predicate as binding and instrument input. These
scenarios remain uncompiled and unrun.

## Native track reordering

`track.edit` now accepts optional zero-based `to` alongside rename, mute and solo.
`track` remains required; `container` is `song` by default, or `pattern`. `to` is
the final row within the same container and must be below its current track count.
Discover the current order with `project.inspect` and rediscover indices after
moving or restoring. All supplied edit fields are validated before moving.

```json
{"id":"move-bass","op":"track.edit","args":{"track":3,"to":1}}
{"id":"move-pattern-instrument","op":"track.edit","args":{"container":"pattern","track":2,"to":0}}
```

When `to` is supplied, the usual name/mute/solo response also reports `from`, the
resulting `track` index and `changed` for row order. A move to the same row succeeds
with `changed: false`. A song pattern track also reports its
resulting `patternIndex`; `project.inspect.tracks` now includes that field for
every pattern track. This index selects a column in each PatternStore row, while
`track` selects a row in the song. They are distinct coordinates.

Reordering operates on native objects. Ordinary tracks retain clips, models,
controller/automation bindings, routing, mute and solo state. Moving a song pattern
track rotates every crossed pattern column and its ownership mapping, including
moves across intervening ordinary tracks. Existing MIDI, sample and automation
clips move between columns without being cloned. The current pattern selection
follows the same pattern's content during the move. Native row order still governs
automation precedence when multiple rows control the same parameter, so changing
that order can change the resulting audio.

The core validates membership, destination and all required pattern slots before
changing order, and rejects foreign-thread and reentrant moves. Missing pattern
slots are reported as inconsistent data. Model changes use the native engine
guard and publish one final `trackMoved` notification. The GUI delegates moves to
this same model path and follows its order, including pending view creation.

Moves participate in normal checkpoint rollback, undo/redo and durable receipt
replay. Native project XML stores the reordered tracks and pattern columns.
The selected pattern is preserved during the move and stored with the native
project/checkpoint, as described below.

Nine native cases / 30 executions are authored for both containers, multi-pattern
rotation, interleaved tracks, 7/8 meter, clip/model identity, malformed inputs,
sparse pattern data, reentry/thread affinity, native automation playback and
precedence, checkpoint history, project round trips and recovery/replay. The
executable harness also checks owner/content mapping, XML order and rollback.
None has compiled or run; GUI regression, rendered audio and full-system behavior
remain unverified.

## Pattern coordinates and selection recovery

A PatternStore column is identified by its native index. Its editor placement is
`index * ticksPerBar` in the current meter; its musical content uses local ticks.
Meter changes now reposition MIDI, sample and automation columns in the core.
Deleting a song pattern track removes that column and assigns the surviving clips
their current native positions without using a fixed 4/4 threshold. Removing a
missing clip from a sparse loaded column does not create a replacement first.
Clip-change notifications identify the owning pattern from the row's clip list,
including when old project data has an incorrect editor position.

Pattern automation now evaluates at local musical time for every column. A point
at tick 24 has the same time coordinate in the first and sixth patterns. The
earlier recording change had left a column offset in PatternStore's evaluation
call; that offset is removed. Arrangement PatternClips apply their signed source
offset and wrap automation within the native pattern length, matching note
playback. Each automation clip's own source offset is then applied as usual.
This corrects timing behavior; source coordinates, curve values, interpolation
and object bindings remain native LMMS state.

Native project saves and recovery checkpoints now store `currentpattern` on the
project's `head` element. Loading applies a valid zero-based selection after all
patterns have been constructed. Malformed or out-of-range saved values select
zero; projects without the attribute retain native constructor selection behavior.
`transport.inspect.pattern` reports the current selection. Undo/redo and reopening
therefore recover the selection captured by the corresponding project checkpoint.
This does not save live playback or uncheckpointed transport changes.

Nine native cases / 31 executions are authored for nonconstant automation in
different columns/meters, native playback ticks, signed and looping arrangement
offsets, meter changes, deletion at different positions, sparse data, ownership
notifications, legacy/invalid selection fields, ordinary project loading and
history/recovery. The process harness additionally checks selection across a
pattern move, undo/redo, rejected-edit rollback and native XML saving. None has
compiled or run; GUI regression, rendered output and project ecosystem
compatibility remain unverified.

## Native copies between tracks and pattern cells

`clip.clone` selects its source with `container` (default `song`), `track` and
`clip`. Omit `targetTrack` to use the source track, or provide a zero-based
destination `targetTrack`. `targetContainer` defaults to the source container
and requires `targetTrack` when supplied. Source and destination track types must
match: instrument, sample, automation or arrangement pattern reference.

For a song destination, supply the new clip's `position`. The copy appends to the
destination's native clip list and retains the source length and signed playback
offset. A copy whose end would exceed the native song limit is refused. Song
copies do not accept `targetClip` or `replace`.

```json
{"id":"copy-to-harmony","op":"clip.clone","args":{"track":0,"clip":0,"targetTrack":2,"position":384}}
{"id":"copy-pattern-to-song","op":"clip.clone","args":{"container":"pattern","track":0,"clip":1,"targetContainer":"song","targetTrack":2,"position":768}}
```

For a PatternStore destination, supply `targetClip` and `replace:true` and omit
`position`. The native copy replaces that existing cell at its current-meter
column position, preserving all other cells and the selected pattern. It refuses
a missing cell, an inconsistent destination row or replacement of the source
cell itself. Adding an owning song pattern track remains the way to add a column.

```json
{"id":"copy-into-pattern","op":"clip.clone","args":{"track":0,"clip":0,"targetContainer":"pattern","targetTrack":1,"targetClip":2,"replace":true}}
{"id":"copy-pattern-variation","op":"clip.clone","args":{"container":"pattern","track":1,"clip":0,"targetClip":2,"replace":true}}
```

The result reports `container`, `track`, `clip`, `position` and `replaced`. Copying
creates native clip identity in the destination. MIDI notes and per-note detuning
are independent; the destination instrument plays them. Automation nodes are
independent and retain their existing target models. Sample copies share the
immutable resident PCM buffer and independently retain gain, reversal and source
window settings, so unsaved audio does not need an intermediate file. A native
direct copy also retains resident audio whose original path is unavailable;
ordinary project persistence still follows the existing asset/checkpoint rules.

An arrangement PatternClip always references its owning PatternTrack. Copying
its marker to another PatternTrack therefore references the destination pattern;
it does not duplicate the source pattern's instrument/sample/automation rows.
Use pattern-cell copies or native pattern-track cloning for that content.

Source or destination note-recording reservations prevent copying. Sample and
automation recording state on the source or replaced cell must be completed or
disarmed. Other clips on a destination sample track must also be idle because
native sample creation refreshes that track's playback handles. Native copying
runs on the model owner thread under the engine guard.
The session applies normal mutation checkpoints, undo/redo and request replay.

Ten native cases / 42 executions are authored for these semantics. The process
harness also compares independently decoded renders before and after copying
split sample clips to another sample track. These tests have not compiled or run.
The existing GUI clipboard path is unchanged; runtime behavior, rendered output,
CPU/memory use and the complete conversational production cycle remain unverified.

## Native selection duplication

`clip.selection.copy` copies 1..4096 distinct source clips in one native mutation.
Its required `items` array contains source `container` (default `song`), `track`
and `clip`. Each item can specify `targetContainer`/`targetTrack`; omitted target
selectors use the source track/container. `targetContainer` requires
`targetTrack`. Unknown nested fields and malformed selectors are refused.

For an arrangement destination, supply `position` and omit `targetClip`/`replace`.
For a PatternStore destination, supply an existing `targetClip` and `replace:true`
and omit `position`. Pattern cells must be distinct and belong to a complete
native row. A selected source cannot also be a replacement destination. Source
clips may span song and pattern containers and compatible native track types.
Each destination must match its source's native track type. Clip ends must fit
within the native maximum song length.

```json
{"id":"duplicate-linked-phrase","op":"clip.selection.copy","args":{"items":[{"track":0,"clip":0,"targetTrack":2,"position":768},{"track":1,"clip":0,"targetTrack":3,"position":768}]}}
{"id":"duplicate-pattern-pair","op":"clip.selection.copy","args":{"items":[{"container":"pattern","track":0,"clip":0,"targetClip":2,"replace":true},{"container":"pattern","track":1,"clip":0,"targetClip":2,"replace":true}]}}
```

Native constructors retain resident sample PCM (including accepted buffers whose
original files were removed), independent notes/detuning and curve nodes, current
sample playback settings and existing automation targets. Only copied controls
use a shared document-local graph, so links spanning selected clips can restore
onto independent receivers. Missing/outside or omitted linked controls refuse
copying. Automation targets retain their native source bindings; they are not
remapped to copied controls. Shared rack bindings retain existing ordinal meaning.
Whole-clip inline graphs and native value/topology journals use the scoped paths
described above. Standalone-note/plugin-only graph copies remain incomplete.

The result contains `copied` and a `clips` receipt array in input order; each
receipt reports destination `container`, `track`, `clip`, `position` and
`replaced`. All selected sources/destination tracks/cells participate in queued
native identity tracking. A preceding operation that deletes or retargets an
existing selection ends that inference batch; rediscover before using new indices.
The agent can call `clip_selection_copy` or `studio_execute` with this operation.
Whole-project checkpoints, undo/redo and cached receipt replay use the existing
StudioRuntime transaction path.

Control validation and cooperative cancellation precede old pattern-cell
retirement. Prepared copies are discarded on earlier failures. Direct core calls
cannot roll back every callback or partial commit side effect; structured runtime
restores its project checkpoint on rejection. Constructor cleanup removes a clip
registered before a returned clip-added callback exception, assuming cleanup
returns normally. One huge native note/clip copy or callback is not preempted by
cooperative checks. Native compiler/Qt/MOC/ABI, actual audio/GUI/plugin/legacy,
provider musical judgment and CPU/RSS verification remain outstanding.

## Native whole-track and pattern duplication

`track.clone` accepts `track` and optional `container` (default `song`). It appends
a duplicate in the same container and returns its zero-based `track` index.
Instrument, sample and automation track clips are copied through native model
constructors, preserving resident sample PCM, independent note detuning and curve
nodes, and current automation targets. Instrument, effect, MIDI and track settings
continue through their native serializers. Duplicated parameters retain fresh
model IDs, avoiding source/duplicate identity collisions in native project files.

Cloning a song PatternTrack creates a new pattern column and duplicates every
instrument/sample/automation cell from the source column. It also copies the
source arrangement markers, which then reference the new pattern. The duplicate
is selected and named `Clone of <source name>`. Cloning an individual PatternStore
row instead duplicates that row across all existing columns.

```json
{"id":"duplicate-instrument","op":"track.clone","args":{"track":0}}
{"id":"duplicate-pattern","op":"track.clone","args":{"track":3}}
{"id":"duplicate-pattern-row","op":"track.clone","args":{"container":"pattern","track":1}}
```

Cloning checks source recording state before constructing a duplicate. A whole
pattern copy checks all affected PatternStore cells, including other columns,
because adding a sample cell refreshes the existing row's playback handles.
Sparse/incomplete pattern layouts are refused. Native failures discard the new
track and appended column, restore the previous pattern selection, and release
the engine model lock. The studio's normal checkpoint rollback handles the
request transaction. The GUI's clone action now displays a native failure instead
of continuing with a null track.

Cancellation and resource-budget checks run between clips and rows. Native asset
checks keep checking cancellation while event dispatch is suspended during the
clone transaction. This prevents a nested event from seeing a partially built
duplicate. It does not impose a hard deadline on arbitrary plugin restoration or
on the work inside one large clip copy.

Built-in resource loaders now reuse the data accepted by the source instrument.
AudioFileProcessor, SlicerT and TripleOscillator retain their decoded sample
buffers, including separate versions of the same original filename or different
waveform attributes on one oscillator. GUS, SoundFont and GIG players receive
independent verified copies of their accepted files. GIG copies include declared
sample pools and convolution companions. Removing or replacing the original
files therefore does not select different input during a clone. Original paths
remain project metadata; a damaged accepted copy or cancelled verification
rejects the duplicate and invokes the existing cleanup/rollback path.

This behavior uses the native resource registrations for each settings element
and attribute. Arbitrary file access inside opaque external plugins has not been
verified. Bank duplication still consumes temporary disk space and native engine
memory; sharing immutable sample buffers does not remove those bank costs.

Fourteen native cases / 33 executions are authored for track settings, all clip
types, complete pattern columns in different meters, saved automation identities,
MIDI controller/port copies, rejected recording/sparse states, failure cleanup,
cancellation, model-lock release, XML, history, replay and reopening. The process
harness additionally duplicates the bundled native instrument, saves distinct
automation targets and reloads the project. These checks remain uncompiled and
unrun; full audio, GUI, plugin compatibility, CPU/RSS and autonomous workflow
verification remain outstanding.

Nine additional resource cases / 26 executions are authored for reference
isolation, buffer ownership, changed/missing originals, accepted-file integrity,
bank lifetime, cancellation and cleanup, native GUS/GIG audio, bundled assets and
project reload. The process harness also compares independently decoded sampler
and oscillator renders before and after cloning. These additional tests have not
compiled or run.

## Native mixer channel ordering and selector ranges

`mixer.edit` accepts optional `to`, the final zero-based channel index. Source and
destination must both be non-master channels; master remains at index zero. The
response includes the channel's resulting `channel` index. All supplied fields
are validated before a move, rename or control change is applied.

```json
{"id":"move-bass-bus","op":"mixer.edit","args":{"channel":1,"to":4}}
{"id":"move-and-name-bus","op":"mixer.edit","args":{"channel":4,"to":2,"name":"Bass group"}}
```

The native mixer rotates the affected channel range. Effects, routing endpoints,
send-amount models, volume/mute/solo models, colors and their automation bindings
stay with their existing channel objects. Solo selection follows its channel.
Default channel names follow their new numbers; custom names remain attached to
their channels. The GUI uses the same core operation and preserves its selected
channel view. Rediscover numeric channel indices after a move.

Stored track routing assignments and reset values follow their channels, including
linked selectors and instrument/sample tracks in both the song and PatternStore.
Automation curves and controllers driving a track's routing selector retain their
native numeric-index meaning: a curve value of `2` still selects the current
channel at index 2. Moving channels does not rewrite those curves or reinterpret
continuous controller values as persistent channel identities.

Channel creation and deletion now update native track selector ranges without a
mixer view. This fixes headless tracks remaining limited to channels that existed
when the track was created. Selector property changes also refresh the associated
audio bus, including controller-driven routing whose stored value did not change.
Native channel deletion maps static assignments/reset values to master when their
channel is removed and decrements later indices once per linked group. The
`mixer.remove` and `mixer.reset` operations are described below.

Twelve native cases / 33 executions are authored for these behaviors, including
reentrant/foreign-thread refusal, native stereo mixing, XML, history, replay and
reopening. The process harness compares independently decoded renders before and
after moving an assigned bus and checks project reload. Source diagnostics pass;
these native tests and render comparisons remain uncompiled and unrun. GUI,
arbitrary callback failure, full project/plugin compatibility and CPU/RSS evidence
remain required.

## Native mixer removal and reset

`mixer.remove` takes `channel` and removes that non-master channel through the
native mixer. Its effects and incident sends are destroyed. Static track routing
and reset values targeting the removed channel return to master; later indices
decrement. Native selector ranges refresh for instrument/sample tracks in both
song and PatternStore. Master removal fails. Rediscover indices and parameter
handles after structural edits or rollback.

`mixer.reset` also takes `channel`, including zero for master. It retains the
channel object and track assignments, clears its effects, restores the native
name/color/level/mute/solo defaults, removes incoming and outgoing routes, and
recreates a non-master channel's unity send to master. Resetting master removes
all sends into master. Surviving control objects keep their native initial values,
automation targets and controller connections; these can affect subsequent values.
Reset is not a replacement of those controls or their external bindings.

```json
{"id":"reset-bass-bus","op":"mixer.reset","args":{"channel":2}}
{"id":"remove-empty-bus","op":"mixer.remove","args":{"channel":3}}
```

Both return `channel`, `removed` and the remaining `channels` count. They use the
normal durable checkpoint, rollback, history and receipt path, including when the
agent calls through `studio_execute`. The adapter unwraps before render timeout
clamping and request construction. Original provider call IDs and wrapped arguments
remain in inference transcripts and tool records; native receipts describe the
resolved operation and arguments. Malformed or unknown catalog calls become
durable failed tool results and skip dependent calls in the same batch.

The declaration budget bounds the number of individual function schemas, not
catalog size, discovery-output bytes or provider context usage. Providers must
still support the configured protocol and non-strict function calling. Official
OpenAI documentation recommends limiting initially available functions and
describes deferred loading; the shared LMMS adapter also works with its existing
Chat Completions transport without requiring provider-specific tool search.
[Function calling guidance](https://developers.openai.com/api/docs/guides/function-calling).

Three additional mixer cases / 12 executions and three catalog-dispatch cases /
18 executions are authored. The process fixture additionally uses catalog dispatch
for tempo/render calls and compares independently decoded audio after mixer reset
and removal, with history, replay and project reload. Native compilation, native
execution, actual provider compatibility, GUI regressions and CPU/RSS verification
remain outstanding.

## Mixer topology admission and presentation lifetime

Native channel creation/deletion/reset/moves, bulk clear/load and route changes
now share an owner-thread admission guard. Model, effect-chain, route-destruction
and selector callbacks cannot reenter another topology mutation while the original
operation is active. Solo-mask and solo-journal transitions also refuse topology
changes. Internal clear/load/create helpers compose the native operation inside
one guard; they do not reopen admission between constituent changes.

Refused channel deletion/reset/moves return false, refused send creation returns
null, and refused clear/send deletion returns without changing the graph. Channel
creation and settings replacement throw a logic error on refused admission. The
guard restores on ordinary C++ exception unwinding so StudioRuntime can restore its
checkpoint. This is not a guarantee that a direct core call is transactional or
that arbitrary plugin exceptions, destructor failures, raw-pointer writes, event
reentry, crashes or hangs are recoverable.

Mixer presentation now retires each channel's separately parented effect rack with
its channel view, owns the rename graphics scene, disconnects late rename signals,
and retains the selected channel object when an earlier channel is removed.
Refresh disconnects saved model connections without indexing the previous native
channel list. Existing channels receive racks when a view is attached. The mixer
widget can be constructed against an initialized engine without the full desktop
window hierarchy; effect controls use a child-owned window in that setting.

Queued effect deletion retains a guarded view reference, and rack updates handle a
retired model. Native topology admission and view edit guards cover their tested
API paths; they do not make arbitrary GUI event pumping during model destruction
safe or prove every plugin's view lifecycle.

Four additional core cases / 14 executions and eight GUI cases / 14 executions are
authored. These include native checkpoint rollback after a reset callback throws,
callback refusal across channel/route/solo/load changes, deferred rack/scene
destruction, selection, refresh after native deletion, queued effect removal and
late rename signals. The GUI suite uses QApplication with Qt's offscreen backend.
The production headless studio and core tests continue to use QCoreApplication.
None of these new native tests has compiled or run in the current environment.

## Native effect-chain editing and ordering

`effect.move` accepts a chain selector (`channel`, or `container`/`track`), an
existing zero-based `effect` index, and exactly one of `to` or `direction`.
`to` is the effect's final zero-based position in the current chain. `direction`
remains `up` or `down`; moving beyond the adjacent end is a successful no-op.
The result contains the final `effect` index. Invalid, fractional, out-of-range
or ambiguous destinations fail before changing the chain. Rediscover effect
indices after a successful move; the native effect and its control models retain
their identities and automation connections during the move.

`effect.clear` uses the same chain selector, destroys all effects in that chain,
disables the empty chain and returns `removed`, the number of effects removed.
An already empty chain returns zero. Mixer routes, channel controls and track
contents are unaffected. Move, removal and clear use the existing StudioRuntime
checkpoint, undo/redo and request-replay behavior.

Native EffectChain now admits structural edits only on its owning thread and
outside another chain transition or serialization. Append, detach, reorder,
clear/load and serialization hold the native audio model guard. Owned append
releases the caller's unique pointer when the effect enters the chain, before
callbacks; refused append retains caller ownership. Owned detach removes the
effect's native and QObject chain ownership before returning it. The legacy raw
remove API retains the QObject parent until the caller or parent deletes it.
Missing-plugin fallback XML is deep-copied when saved, preserving earlier DOM
snapshots and opaque plugin state.

Mixer topology changes also refuse admission while one of that mixer's effect
chains is in a transition or serialization. Mixer reset checks effect clearing
before proceeding. The GUI rack follows native order, reuses existing effect
views, and retires a view only after native detachment succeeds. These guards
cover these APIs; arbitrary cross-object deletion, GUI event pumping, destructor
exceptions and nonreturning plugins remain outside this verification scope.

EffectChainTest adds 12 cases / 42 executions for native DSP order, ownership,
empty/invalid/foreign/reentrant refusal, exception lock release, XML fallback,
five chain locations, history, rollback, replay and reopening. Two additional
GUI executions cover view identity/order and refused edits. The full-build
process fixture additionally configures the bundled Amplifier and Stereo Matrix
to produce different stereo output when reordered, then checks project reload,
clear/removal and history/replay. All of these native executions are authored
and unrun. The catalog now has 123 native operations; inference declarations
remain bounded at 128 through the existing catalog dispatcher.

## Effect, bypass and mixer-send control discovery

`effect.inspect` accepts exactly one chain selector: a mixer `channel`, or a
`track` with optional `container` (`song` by default, or `pattern`). It returns
the chain's effective `enabled` value, its `enabledParameter` metadata, and the
ordered `effects` array. Optional `effect` selects one existing zero-based slot;
indices in the response retain their full-chain positions. `details` and
`choices` default to true. With `details:false`, effect parameter arrays are
omitted; chain bypass metadata remains available.

The effect array accepts `offset`/`limit` (default 16, maximum 128) and returns
`total`, `nextOffset` and `hasMore`, with a 1 MiB array budget. If `effect` selects
one native slot, the filtered total is one and paging starts at offset zero;
the returned effect's `index` still identifies its full-chain slot. With details,
each effect has `parameters` and `parameterPage`. Select that effect and continue
with `parameterOffset`/`parameterLimit` (default 128, maximum 1024). A short page
can result from its byte bound; follow `nextOffset` instead of adding the limit.

Each effect reports its native plugin name, display name, subplugin `attributes`
and `missing` status. Available effects also report enable/wet/auto-quit/corruption
state and, with details enabled, their actual native parameter handles. Missing
plugins report their original saved plugin name and key attributes, with no fake
editable parameters. Their original plugin state remains preserved as opaque XML.
Detached effects awaiting legacy QObject cleanup are also excluded from parameter
discovery because they no longer participate in the native chain.

`parameter.list` now includes mixer effect controls, all chain bypass controls and
native mixer-send amounts. Filter by one of:

- `container`/`track`, optionally adding `effect` for a specific track's slot;
- `channel`, optionally adding `effect` for that mixer channel's slot;
- `from` and/or `to` for outgoing/incoming mixer sends;
- `controller` for native rack controller controls.

An `effect` filter requires a specific track or channel. Selector families cannot
be mixed. Existing effect edit operations likewise reject an ambiguous combination
of `channel` and `track`/`container` before changing the chain.

Parameter `location` identifies the current owner: track/container or mixer
channel plus `scope:"effect"` and `effect` for effect controls,
`scope:"effectChain"` for bypass, or `scope:"mixerRoute"` with `from`/`to` for
sends. Mixer level/mute/solo controls report their `channel`. Two instances of the
same plugin can therefore have identical control names and distinct locations and
handles. Use `parameter.set` with numeric 0/1 for the discovered bypass handle;
the same native handles support existing automation/controller operations when
their metadata permits them. These operations edit the models used by native DSP.

Project inspection includes effect order and chain enable state for song/pattern
instrument and sample tracks as well as mixer channels. Sample-track routing is
reported too. Send entries include their amount `parameter` handle. Instrument
inspection shares the same control collection rules as effect/parameter discovery.

Handles and locations must be rediscovered after restore, structural edits or
plugin control-model replacement. Discovery refreshes native plugin models before
collecting pointers. It does not provide durable semantic addresses or prove a
plugin's audio behavior. The catalog contains 124 native operations; inference
declarations remain bounded at 128.

Ten additional native cases / 30 executions are authored for five chain kinds,
repeated instances, filtering, missing/detached controls, refreshed models, native
bypass/send audio, controller connections and automation/history/replay/reopening.
The process fixture now discovers real bundled effect controls, distinguishes
repeated Amplifiers and compares rendered bypass/send changes. These native tests
and process scenarios remain uncompiled and unrun in the current environment.


### Native waveform and effect graphs (2026-10-13)

The native catalog now contains 129 operations. Five graph operations expose the
actual graph models owned by instruments and effects. Instrument and detailed
effect inspection also return graph metadata. Graphs use stable plugin-local keys;
effect/track/channel indices still require rediscovery after structural edits.

| Plugin | Graph keys | Native units |
| --- | --- | --- |
| BitInvader | `wave` | waveform amplitude |
| FreeBoy | `wave` | 4-bit amplitude, 0?15 with step 1 |
| Watsyn | `a1`, `a2`, `b1`, `b2` | waveform amplitude |
| Vibed | `string1` through `string9` | string excitation amplitude |
| Xpressive | `w1`, `w2`, `w3` | raw sample waveform amplitude |
| Waveshaper | `transfer` | absolute output amplitude |
| Dynamics Processor | `transfer` | absolute output amplitude |
| Multitap Echo | `tap-gain`, `tap-filter` | dB; log10(Hz / 20) |

Select an instrument with `track` and optional `container`. Select an effect with
`effect` plus either `track`/`container` or `channel`. A channel alone is insufficient,
and mixed owner selectors are rejected. Missing plugins have no editable graph.

- `graph.list` returns keys, names, units, visible length, `min`, `max`, `step`,
  `writable`, supported `actions` and `fileImport` for the selected native plugin.
- `graph.inspect` adds all current `values` for the required `graph` key.
- `graph.set` requires exactly the discovered number of finite `values` inside the
  graph's range. Native downward step snapping applies. It preserves an existing
  hidden waveform tail. Failed validation publishes no samples.
- `graph.transform` takes a discovered `action`. Common native actions are `clear`,
  `invert`, `normalize`, `smooth`, `smooth-noncyclic` and `shift`; `shift` requires
  integer `degrees` from -360000 to 360000. Continuous graphs containing -1 to 1
  additionally expose `sine`, `triangle`, `saw`, `square` and `noise`. The operation
  stages the native algorithm and refuses nonfinite output before publication.
- `graph.load` takes a `path` for graphs whose metadata allows file import. Native
  SampleBuffer decoding and Oscillator waveform sampling fill the existing graph
  length. The resulting samples are embedded in normal plugin state, so later
  project restoration does not require that source audio file.

Example requests for an already loaded native plugin:

```json
{"id":"waveforms","op":"graph.list","args":{"track":0}}
{"id":"curve","op":"graph.inspect","args":{"channel":0,"effect":0,"graph":"transfer"}}
{"id":"smooth-curve","op":"graph.transform","args":{"channel":0,"effect":0,"graph":"transfer","action":"smooth-noncyclic"}}
{"id":"load-wave","op":"graph.load","args":{"track":0,"graph":"wave","path":"/absolute/wave.wav"}}
```

Range/step constraints apply to manual replacement. Existing native transforms,
file imports and finite legacy sample data retain their amplitude semantics; they
can contain values outside the editor's display range. Transform actions are not
musical recommendations for every plugin: for example, native normalization removes
DC and scales amplitude, even when a curve represents dB. Inspect units first.
Graphs are not numeric automation handles; use the existing native automatable
parameters for supported continuous automation.

Xpressive raw-wave and smoothing changes now refresh processed waveform/DSP buffers
inside the instrument. A direct graph edit clears that wave's expression text so a
later editor refresh cannot regenerate a different curve from a stale formula.
Output and wave formulas are also available through the native expression
capabilities documented below.

Graph loading retains the native Base64 float serialization format. Present data
must have a valid encoding, aligned expected sample count and finite floats. Missing
attributes preserve native defaults. BitInvader also accepts bounded shorter legacy
tables with zero-filled unused storage and retains a full table behind its visible
sample length. Invalid attributes enter the native project error collector and
therefore cause studio project loading to fail and invoke checkpoint recovery.
Individual plugin load routines can change other settings before that failure.

GraphModelTest and StudioGraphTest add 19 cases / 46 executions, all authored but
uncompiled and unrun. The process harness adds eight bundled-plugin serializers,
BitInvader/Waveshaper/Xpressive DSP checks, hidden-tail and source-file independence,
malformed-project recovery and undo/redo/replay. Source diagnostics pass; native
Qt/MOC compilation, GUI regression, audible behavior, project corpus compatibility,
performance and full autonomous production verification remain outstanding.


### Native Xpressive expressions (2026-10-13)

The catalog now contains 132 native operations. `expression.list`,
`expression.validate` and `expression.set` operate on plugin-owned formula state.
They use the same instrument/effect selectors as the graph capabilities. Instrument
inspection and detailed effect inspection include `expressions`; unsupported native
plugins return an empty inventory. Missing plugins cannot expose editable formulas.

An expression resource reports its `key`, `name`, current `source`, `kind`,
`symbols`, `semantics` and `writable` status. Xpressive exposes waveform generators
`w1`, `w2`, `w3` and output formulas `o1`, `o2`. Formula keys are distinct from the
wave graph keys even where their names match: a formula generates or reads the
native samples, while a graph edit replaces the samples directly.

```json
{"id":"formulas","op":"expression.list","args":{"track":0}}
{"id":"check-wave","op":"expression.validate","args":{"track":0,"expression":"w1","source":"sinew(t)","preview":true}}
{"id":"generate-wave","op":"expression.set","args":{"track":0,"expression":"w1","source":"sinew(t)"}}
{"id":"use-wave","op":"expression.set","args":{"track":0,"expression":"o1","source":"W1(integrate(f))"}}
```

Validation returns `valid`, `error`, preview `sampleRate`/`frames`, and
`nonfiniteSamples`; `preview:true` adds the sample array. Invalid syntax or preview
output is a normal validation result with `valid:false`. `expression.set` rejects
invalid source as an operation failure. Accepted edits include the new expression
metadata. Studio history and checkpoints preserve expression text and generated
wave samples through native plugin serialization. Read-only validation preserves
those fields, project revision/history, and the random generator state used by
subsequent expressions. Successful generation can advance that generator.

Xpressive uses its existing ExprTk parser and native functions. Sources contain at
most 65536 ASCII characters without NUL. Standard math functions and Xpressive's
waveform, random, `integrate` and `last` functions remain available. Native parser
settings disable logic, assignment and control structures.

| Kind | Native preview and edit behavior |
| --- | --- |
| Waveform | Generates 4096 samples at `srate=4096`, with `t` spanning [0,1). Existing native smoothing then refreshes the DSP waveform. Nonfinite generated samples become zero and are counted. |
| Output | Previews 360 samples at `srate=360`, `f=10`, `key=5`, `v=0.5`, `rel=trel=0`, with current base note, tempo, A1?A3 and W1?W3. Nonfinite preview output is rejected. Playback uses real note values. |

Wave expressions have `t`, `srate`, mathematical constants and native functions.
Outputs additionally have `f`, `key`, `bnote`, `v`, `tempo`, `rel`, `trel`, A1?A3 and
W1?W3. Metadata explains these scopes. Empty/whitespace wave source clears its
formula while retaining its accepted waveform. Empty output source silences that
output for subsequent notes. Output formula changes affect newly constructed note
synths; a finite preview does not prove behavior at every note, release phase,
automation value, sample rate or later time. Render and evaluate the actual song.

Graph edits clear the corresponding wave formula. Wave formulas remain saved along
with their generated samples. Loading a native project/preset preserves those
samples; opening the editor no longer regenerates them as a side effect of loading
its text field. The GUI uses the same checked native edit path. Invalid editor text
is shown as a draft with a diagnostic while the last accepted formula remains in
the instrument; that invalid draft is not serialized.

Cancellation/budget checks occur before compilation and between preview batches,
with event delivery suspended during native work. They cannot interrupt an ExprTk
compile or evaluation already in progress. The bounded preview is a sound-design
check, not an audio-understanding or subjective musical evaluation claim.

XpressiveExpressionTest adds 10 cases / 19 executions using the actual native
ExprTk implementation, conditional on the bundled target. StudioExpressionTest
adds 5 cases / 13 executions for native ownership, dispatch, read-only validation,
refusal and DSP publication. The process harness authors real formula generation,
render/silence, failure retention, empty-source semantics, graph ownership,
undo/redo/replay, native projects/presets and a separate session restart. All 32
native executions and process scenarios remain uncompiled and unrun. Source checks
pass; native build, GUI, compatibility, audio and resource evidence is outstanding.

### Native VST parameter ownership (source implementation, 2026-10-13)

VeSTige and VST effects now create their generic parameter models when the native
plugin loads. `instrument.inspect`, `effect.inspect`, `parameter.list`,
`parameter.inspect`, `parameter.set`, automation and controller operations use
those same native FloatModels. Opening a parameter editor binds its knobs to the
existing models; closing it no longer deletes automation targets or their DSP
connections. Parameter names include the native index and the plugin label.

For VeSTige, load the `vestige` instrument and inspect its resources. The `plugin`
resource accepts a native VST binary through the existing checked resource API:

```json
{"op":"instrument.resource.set","args":{"track":0,"resource":"plugin","path":"/absolute/path/to/instrument.so"}}
```

Use the binary format supported by the current LMMS build and its native remote
worker. A successful resource request requires a loaded worker and a valid native
parameter reply. Dependencies adjacent to a plugin binary, Wine/native host
availability and plugin compatibility still require an actual installation test.
An asset entry for a binary does not package its dependent libraries or data.

Refresh retains each model's ID and skips values controlled by automation,
controllers or linked models. Synchronization does not echo values back into the
VST. Native model edits and automation use one direct parameter writer with the
captured native index, independent of the label. Values retain LMMS's normalized
0..1 model range; the existing remote float encoding and automation timing remain
unchanged. A write is not a DSP acknowledgment from the hosted plugin.

The native decoder checks contiguous indices, complete records, finite normalized
values and stable parameter counts. Labels may contain colons; legacy comma decimal
values and older plain numeric saved attributes are accepted. Invalid replies are
validated before changing cached model values. A different parameter count is
refused until the plugin is explicitly reloaded, preserving existing targets.

Native project serialization retains the VST chunk/parameter state and the existing
`paramN` automation IDs/controller metadata. Saving a controller cannot replace a
non-chunk plugin's `index:name:value` attribute with a model scalar. Repeated restore
reuses models for the same loaded plugin and retires old controller connections.
Headless adapter loading and UI creation are explicitly guarded against widget
creation. Request and DSP locks release on exceptions; owner-thread and synchronous
refresh checks precede the adapter mutex. Cooperative checks run while decoding and
creating controls; they do not preempt arbitrary third-party plugin execution.

VstParameterModelsTest authors 10 cases / 24 executions under QCoreApplication,
using the production model implementation and a deterministic parameter writer.
The suite covers lifecycle, XML, automation/controller retention, invalid data,
cancellation, ownership and native EffectChain dispatch in five locations. It is
conditional on the VST build target and has not compiled or run. It does not test a
real VST binary or prove remote transport, hosted audio, GUI behavior, complete
project compatibility or resource bounds. Indexed VST program operations are described below. Explicit FXP/FXB file APIs
and their native file-validation requirements remain unfinished. The full
application transformation is incomplete.

### Indexed native plugin programs (source implementation, 2026-10-13)

`plugin.program.list` and `plugin.program.select` address the loaded native plugin's
zero-based program index. They apply to VeSTige instruments and VST effects. This
interface is separate from `instrument.program.*`, which retains the existing MIDI
bank/program semantics for native sound-bank instruments. Instrument/effect inventory
reports `nativeProgramsSupported` without querying every program.

Select an instrument with `track` and optional `container`. Select an effect with
`effect` plus either `track`/`container` or `channel`. Mixed owners are refused.

```json
{"op":"plugin.program.list","args":{"track":0,"offset":256,"limit":128}}
{"op":"plugin.program.select","args":{"track":0,"program":512}}
{"op":"plugin.program.list","args":{"channel":0,"effect":1,"offset":0,"limit":64}}
```

A list reply contains `total`, `current`, `offset`, `limit`, `hasMore`, `numbering`
and `programs`. Entries have `program`, `name` and `nameAvailable`. The default page
size is 128; requests accept 1 through 1024 entries. Offsets beyond the catalog return
an empty page. A plugin with no programs reports `total:0`, `current:-1`. The native
index range is not restricted to MIDI's 128 programs or the former VST menu's first
256 entries. Native integer and page bounds are validated before selection.

Listing calls the plugin's indexed-name callback for unselected programs and obtains
the current program's actual name separately. It never switches the live program to
infer names. Some VSTs do not implement indexed names; their unselected entries report
an empty name with `nameAvailable:false`. This is distinct from a valid empty name.
Such a program remains selectable, and the selected reply can report its actual name.
Selection changes sound state; use native revision/checkpoint recovery when exploring
programs whose current unsaved settings must be recovered. An indexed-name callback
implemented incorrectly by a third-party VST can still have its own side effects;
LMMS cannot prove arbitrary plugin behavior from a successful reply.

The native worker acknowledges selection after reading back the requested index.
The adapter then refreshes its existing parameter models while keeping automation,
controller and linked-model targets. GUI next/previous/menu actions use this same
adapter. Native menu names use separate string entries, preserving `|` and `:` in
names. Project serialization retains its existing numeric `program` field, with a
fresh native current-program query; zero-program plugins retain compatibility with
older saved `program:0` values. The new protocol requires matching rebuilt LMMS and
remote-worker binaries. A failed or inconsistent reply is reported as failure, not
successful selection. Runtime mutations continue to use existing history/recovery.

VstProgramsTest authors 6 cases / 11 executions using the production query/selection
helpers with native callbacks. StudioPluginProgramTest adds 3 cases / 12 executions
for native ownership, read-only state, pagination, errors, schema/selector refusals,
identity retention and actual EffectChain buffer processing in five chain locations.
Both are registered under QCoreApplication with 120-second timeouts. All 23 native
executions remain uncompiled and unrun; no real VST host/worker, GUI, project corpus,
recovery trace, rendered hosted audio or CPU/RSS result is established here.

This program-index entry added two operations, bringing that source revision to
134 native operations. The following entry records subsequent native preset file
work and supersedes the earlier FXP/FXB implementation gap.


### Native VST preset files and bank persistence (source implementation, 2026-10-13)

`plugin.preset.inspect`, `plugin.preset.load` and `plugin.preset.save` address a
loaded VeSTige instrument or VST effect through the same exact selectors as
`plugin.program.list`. Use `container`/`track` for an instrument; add `effect` for
its effect chain, or use `channel`/`effect` for a mixer effect. Instrument and
effect inventories expose `nativePresetsSupported`.

```json
{"id":"preset-check","op":"plugin.preset.inspect","args":{"track":0,"path":"C:/sounds/bass.fxp"}}
{"id":"preset-load","op":"plugin.preset.load","args":{"channel":2,"effect":1,"path":"C:/sounds/space.fxb"}}
{"id":"preset-save","op":"plugin.preset.save","args":{"track":0,"path":"C:/sounds/current.fxp","format":"fxp"}}
```

Import format is determined from file contents. Save requires `format: "fxp"`
(current program) or `format: "fxb"` (bank), independently of the destination
extension. These operations import/export the hosted plugin's sound state;
`preset.load`/`preset.save` continue to operate on LMMS instrument-track `.xpf`
presets. Native file dialogs choose a path and then call the same guarded adapter.

Results report `path`, `format`, `encoding`, `name`, eight-digit hexadecimal
`pluginId`, `pluginVersion`, `formatVersion`, `programCount`, `parameterCount`,
`currentProgram`, `bytes` and SHA-256. `encoding` is `parameters`, `chunk`, or
`programs` for an FXB containing embedded chunk programs. Parameter count is `-1`
when the file does not describe one plain parameter program. Current program is
`-1` when absent. For chunk FXP files, `programCount` is header metadata and can
exceed one; it does not mean that the file contains that many program records.

The shared native codec supports `FxCk`, `FPCh`, `FxBk` and `FBCh`, big-endian
integer/IEEE float fields, FXP version 1 and FXB versions 1/2. Zero `byteSize`
fields from established writers are accepted; nonzero sizes must match the parsed
record. Version 2 selects the recorded native program; version 1 has no recorded
selection. FXB can contain parameter or chunk program records. Nested banks,
trailing bytes, truncated fields/payloads, impossible counts, nonfinite or
out-of-range normalized values and conflicting plugin IDs are rejected. The
loaded plugin must match the ID and parameter dimensions; indexed bank records
must fit its available programs. Opaque bank counts need not enumerate the
plugin's internal programs, but a recorded selection must fit the loaded plugin.

The parser uses checked byte cursors and bit conversions, without native struct
layout or float/integer pointer aliasing. A file is bounded to 256 MiB; decoded
allocation accounting is also bounded. These are file/object bounds, not a proof
of peak RSS. Input is fully decoded before any application callback. The host
captures an accepted private file, checks its digest, and asks the worker to
validate or apply that copy. The worker returns success/error explicitly.
Inspection does not call state-mutating plugin callbacks.

Export obtains native chunks when supported. Otherwise it reads native parameter
programs and restores the active program's index, name and unsaved exposed values,
including on capture failure. It does not derive a program name from the output
filename. A partial version-1 parameter bank retains the active edit buffer when
that program is outside the imported range. Native callback failures are reported;
arbitrary third-party state and a failed restoration require session rollback.

The worker writes a private output. The host parses it, checks the requested kind,
streams it into `QSaveFile`, verifies its digest, and commits without direct-write
fallback. Read/write/close failures and cancellation before publication prevent a
successful response. `plugin.preset.load` and `plugin.preset.save` are project
mutations in the session catalog so native checkpoint/recovery surrounds load and
bank capture. Save therefore creates a session revision even when exposed sound
state is restored. History does not undo exported files, and a published export
can survive a later session-state commit failure. File cancellation is cooperative;
it cannot preempt an arbitrary native plugin callback or blocked OS operation.

Native VST project serialization now embeds non-chunk program-bank state in a
canonical Base64 `nativepreset` attribute; zero-program effects use an embedded
FXP for their current values. Legacy current-program/parameter fields remain for
older readers. New readers validate and prefer the embedded native state, so bank
edits can participate in project saving, checkpoints and recovery. Opaque project
chunks keep their existing representation, with checked worker file I/O and
acknowledgments. An advertised chunk plugin that returns no valid chunk fails
serialization instead of silently degrading to a parameter-only snapshot. The
new worker protocol requires matching rebuilt host and worker binaries.

VST2 opaque chunk callbacks do not provide a reliable application acknowledgment.
A successful file operation establishes checked transport and completed callbacks,
not that an arbitrary plugin interpreted the entire opaque payload as intended.
Listening/rendering, real-plugin compatibility and recovery verification remain
required. New bank serialization also needs legacy corpus and performance checks.

The codec/storage design was checked against primary implementations: JUCE's
[FXP/FXB reader and writer](https://raw.githubusercontent.com/juce-framework/JUCE/7.0.9/modules/juce_audio_processors/format_types/juce_VSTPluginFormat.cpp)
and Steinberg's [VST2 persistence implementation](https://raw.githubusercontent.com/steinbergmedia/vst3_public_sdk/master/source/vst/utility/vst2persistence.cpp).
No SDK source was copied into LMMS.

Authored native tests: `VstPresetTest` (9 cases / 28 executions),
`VstPresetStorageTest` (7 / 10), and `StudioPluginPresetTest` (3 / 12).
They cover independent byte fixtures, every truncated prefix of the four formats,
zero sizes, invalid floats/counts/IDs, complete-bank preflight, callback restoration,
opaque/zero-program and partial-bank behavior, accepted-file ownership, bounded
capture, cancellation, atomic output, canonical project state, exact native effect
ownership, identity retention and native EffectChain buffer processing. These 50
executions are registered under QCoreApplication with 120-second timeouts. They
are authored only: no native compilation/execution, real VST worker, GUI, XML
round trip, undo/replay/restart, hosted audio or resource trace has run here.

Source validation now reports 328 changed/new C++ files and 137 native operations;
the inference declaration limit remains 128 with the complete catalog executor.
All six subsystem and full-application completion gates remain open.


### Checked VST parameter restore and worker tests (source implementation, 2026-10-13)

Bulk VST parameter restore now waits for `IdVstSetParameterDumpChecked`. The worker
validates the complete count/index/value message before any setter callback and
acknowledges only after every callback returns. Missing, extra, malformed,
overflowing or out-of-range fields return an explicit error instead of becoming
zero through permissive C conversions. The host checks the worker result and
requires its own QObject owner thread before entering the audio guard. This closes
the source ordering gap in legacy parameter XML restore, where the host could
submit its next audio block while the control thread had not applied the dump.
Callback completion does not prove that a third-party plugin accepted every
requested value unchanged; native inspection/audio evaluation remains necessary.

VST scalar writes and dump replies now format floats with `max_digits10` using
locale-independent character conversion. This preserves distinct representable
float values that the previous six-decimal transport could collapse. Worker scalar
parsing also validates the entire number. Individual automation writes remain on
the processing thread and do not add per-message acknowledgment traffic. Bulk
restore uses the separate checked control message; it is not an audio-thread API.
Parameter query replies include exact field-count validation and propagate native
query errors before replacing the cached host values. Matching host/worker builds
are required.

`VstParameterWireTest` authors 3 cases / 20 executions for exact float round trips,
complete preflight, field/count errors, empty layouts and callback exceptions.
`VstNativeWorkerTest` authors 6 cases / 12 executions that instantiate production
`VstPlugin`, launch production `RemoteVstPlugin`, and load three actual MODULE
fixtures built from `tests/fixtures/native-vst/NativeVstFixture.cpp`. These fixtures
implement the repository's native VST ABI: a parameter bank, an opaque chunk bank,
and a zero-program effect. They are deterministic test plugins, not substitutes
for LMMS instruments or evidence of third-party compatibility.

The worker tests run under QCoreApplication with `DISPLAY` removed. The fixtures
advertise an editor and terminate the worker if it is opened. Coverage includes
native stereo DSP immediately after bulk restore, exact adjacent floats, owner
thread refusal, 700 programs and missing indexed names, FXP/FXB formats independent
of extension, malformed inputs, XML state across worker destruction/recreation,
rejected bulk-message replies, and failed advertised chunk capture without fallback
or destination replacement. The opaque fixture intentionally returns zero from
`effSetChunk`, preserving the protocol's documented acknowledgment limitation.

Native MIDI dispatch also replaces unchecked copying into fixed 1,024-event arrays
with `VstMidiBuffer`. It sizes the native pointer packet for the accepted batch,
checks integer/size arithmetic, preserves sample order and equal-offset order,
and owns records until the next batch or worker teardown. The worker fixture
defers reading 1,500 events until its audio callback to exercise pointer lifetime.
`VstMidiBufferTest` adds 1 case / 4 executions for empty, single, 1,500-event and
8,193-event batches, stable ordering and producer-queue reuse. Storage retains peak
capacity, but allocation/growth latency and resource limits remain unmeasured.

CMake registers the worker target for matching native Windows host/worker bitness
and 64-bit Linux with the existing native worker target. It builds the fixtures
and worker as dependencies. It does not claim Wine, cross-bitness, 32-bit Linux,
third-party VSTs, GUI behavior, full StudioRuntime undo/replay/restart, agent
workflows, real-time scheduling or CPU/RSS verification. Those remain separate
completion gates. No native test has configured, compiled or run here.

Source diagnostics pass for 334 changed/new C++ files and 137 native operations.
The 36 new native executions are authored only; the missing toolchain/Qt and pinned
dependencies remain unresolved. Existing helper/storage tests and these worker
targets must all run before native preset/state recovery can be considered verified.


### VST project recovery and legacy chunk validation (source implementation, 2026-10-13)

Legacy VST `chunk` attributes now pass through `decodeVstChunkState` before native
program selection. Previously, permissive Base64 decoding could drop invalid
characters and apply the remaining bytes. The decoder accepts canonical Base64,
ASCII space/tab/CR/LF wrapping and fully omitted final padding. It rejects invalid
characters, misplaced/partial/excess padding, incomplete bytes, nonzero padding
bits, empty decoded state and chunks larger than 256 MiB. Long input scanning
checks the existing cooperative file-I/O cancellation/budget scope. This validates
the encoding; opaque plugin semantics still cannot be proven from `effSetChunk`'s
return value.

Thirteen additional `VstPresetStorageTest` executions cover valid legacy forms and
malformed encodings. One additional `VstNativeWorkerTest` execution checks that a
corrupt chunk cannot change the selected program or parameters and that a wrapped
chunk still restores through the production worker. These targets now author
8 cases / 23 executions and 7 cases / 13 executions respectively. They remain
uncompiled and unrun.

`StudioVstProcessTest0`, `StudioVstProcessTest1` and `StudioVstProcessTest2` add actual
LMMS process scenarios for plain parameter banks, opaque chunk banks and effects
with no indexed programs. The Python harness supplies calibration PCM and test
requests; `lmms studio` owns native project state, the `vsteffect` wrapper, worker,
rendering and durable recovery. Scenarios cover bank edits beyond program 256,
FXP import and FXB bank restoration, automation bindings, undo/redo, receipt replay,
malformed-project rollback, failed chunk checkpoint capture, normal session reopen,
recovery after an acknowledged mutation followed by abrupt process termination,
continued editing and saved-project reload. They compare stereo PCM with the dry
native render after restoration and revision. Handles are rediscovered after
project replacement, and automation targets are checked against the new objects.

The process scenarios require a full matching Windows or 64-bit Linux build with
VST support and Python. `StudioVstProcessFixtures` builds the executable, native
effect wrapper, worker and three test modules. The harness puts copies of the
built effect/base modules in its private workspace's supported plugin directory,
so worker and module output paths can differ with multi-configuration generators.
Native VST fixture binaries remain at their original paths; this does not establish
third-party binary/resource portability. Each run keeps a unique evidence directory
with protocol traces, stderr, WAV comparisons and a verification record. CTest
allows 360 seconds per scenario. After a complete build, the intended commands are:

```text
cmake --build <build> --target StudioVstProcessFixtures
ctest --test-dir <build> -R "^StudioVst(ProcessTest[012]|DenseProcessTest)$" --output-on-failure
```

Multi-configuration builds also require their selected build/test configuration.
These commands have not run here. Python syntax/import/CLI checks and source
diagnostics pass (334 changed/new C++ files, 137 native operations), as does
`git diff --check`. CMake, Ninja, C++ compilers and qmake are unavailable on PATH;
all 17 pinned submodules remain uninitialized. The 14 new C++ executions and three
process scenarios are authored coverage, not recovery or audio execution evidence.
Full plugin compatibility, autonomous musical workflows and CPU/RSS gates remain
open with the rest of the complete-application commission.


### VST binary and parameter text input boundaries (source implementation, 2026-10-13)

`VstPlugin` no longer maps an entire PE file and dereferences its unchecked header
offset. `readVstPeMachine` reads the 64-byte DOS header and 24-byte PE signature/COFF
header, checks signatures and file bounds, and uses unaligned little-endian reads.
Short files, offsets beyond EOF and incomplete headers return errors before worker
selection. The layout and machine values were checked against Microsoft's
[PE format specification](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format).
This classifies the existing Windows x86/x64 paths; the platform loader still
validates the complete binary. It does not pin a mutable path between inspection
and loading, add architectures or change the existing Linux `.so` worker route.

Failed VST effect initialization now sets the native effect's `isOkay()` status to
false as well as disabling processing. Previously, a missing or refused binary
could leave an inert effect marked usable, so `effect.add` could acknowledge it.
The existing studio initialization check now refuses that effect and invokes the
normal checkpoint recovery. Native project loading can preserve its XML using the
existing missing-effect mechanism. An absent effect key is also handled without
dereferencing a null pointer.

Parameter labels and display strings retain their existing byte-length wire
format. The host now splits the byte payload before converting each field to
QString, checks ASCII lengths 0..8, requires exactly the native parameter count,
and publishes a replacement cache only after complete validation. The former
QString/digitValue loop could use a byte count as a Unicode character count and
then fail to advance on a non-digit. Single-entry updates validate indices,
argument counts and byte lengths. The worker validates requests, returns explicit
query errors and clears its local text buffer before each callback, including a
callback that declines to write text. Owner-thread queries take the native audio
guard and verify replies. GUI parameter views catch reported errors, and knob
tooltips show an unavailable message instead of throwing through Qt events.

Twenty-nine additional C++ executions are authored: `VstPeFileTest` has 4 cases / 11
executions including every truncated fixture prefix; `VstParameterWireTest` adds
14 text-framing executions (now 5 / 34); and `VstNativeWorkerTest` adds four cases
for Unicode metadata, empty callbacks, rejected requests/replies, cache retention,
owner-thread refusal and malformed binary loading (now 11 / 17). Worker fixtures
exercise UTF-8 labels whose byte and character lengths differ. All native tests
remain uncompiled and unrun.

The three `StudioVstProcessTest` scenarios also author empty/missing/short/invalid-
offset plugin refusals, unchanged history and effect chains, subsequent successful
loading, and retained automation/audio after refusing an additional bad effect.
The process harness is instrumentation only and still requires the actual built
LMMS executable, effect wrapper, native worker and module fixtures.

Source diagnostics pass for 336 changed/new C++ files and 137 native operations.
Python syntax/import/CLI checks and `git diff --check` pass. CMake, Ninja, C++
compilers and qmake remain absent from PATH; all 17 pinned submodules are still
uninitialized. Native tests, GUI compatibility, real plugin/corpus behavior,
timing, CPU/RSS and complete autonomous music-production workflows remain open.


### Large VST parameter transfers (source implementation, 2026-10-13)

Native parameter reads and bulk restores previously packed the whole plugin
layout into one message. The shared-memory transport correctly refuses messages
larger than 512 KiB by invalidating the connection, so a sufficiently large valid
layout could disconnect during discovery, project persistence or editing. Bulk
labels and displays had the same single-message limitation.

The rebuilt host and worker now exchange at most 256 parameters per message.
This is a page size, not a parameter-count limit. Parameter replies contain a
checked total, offset, count and ordered index/name/value entries; metadata pages
contain ordered index/text entries. The host checks every page against the loaded
layout and publishes its cache only after the complete transfer succeeds. Native
float precision, colon-containing names, UTF-8 conversion and empty metadata are
retained. Read cancellation leaves the previous cache intact.

Bulk parameter restores use acknowledged `begin`, `append`, `commit` and `abort`
messages. The worker validates native counts, contiguous offsets, exact field
counts and every normalized value while staging the complete layout. Native
setters run only on commit, after every value has arrived. Malformed, duplicate,
out-of-order or incomplete transfers discard staging and return errors. Host
cancellation before commit requests an abort when the worker is still usable.
Owner-thread operations hold the audio-change guard and communication lock;
paged waits preserve request deadlines without dispatching Qt events that could
reenter the transfer. Audio-thread scalar parameter updates retain their existing
asynchronous path.

Existing message numbers and legacy handlers remain. The new IDs are appended,
and this path requires matching rebuilt host and worker binaries. Legacy bulk
handlers retain the single-message size limit. Page bounds do not bound the total
memory required by the complete native layout, native callback duration or GUI
latency. A setter that fails during commit may already have changed earlier
parameters; studio checkpoint recovery remains responsible for restoring that
native state. Plugin-owned UI/background changes are not made atomic by paging.

Thirty-five additional native test executions are authored. The new
`VstParameterTransferTest` has 8 cases / 32 executions for page boundaries,
malformed replies, native order and float precision, staged validation, aborts,
empty layouts and throwing setters. Three new `VstNativeWorkerTest` cases bring
that target to 14 cases / 20 executions. A fourth actual ABI fixture exposes
65,537 parameters and metadata that exceed the old transport limit. It checks
all restored values, scalar editing, cancelled-read cache retention, native XML
state across worker recreation and stereo DSP. Worker protocol cases check that
accepted pages do not change native parameters before commit, malformed requests
remain recoverable, and a cancelled restore permits a later valid restore.

Source diagnostics pass for 338 changed/new C++ files and 137 native operations;
Python syntax and whitespace checks pass. CMake, Ninja, C++ compilers and qmake
remain unavailable on PATH, and all 17 pinned submodules remain uninitialized.
The 101 executions in the VST wire/transfer/header/MIDI/worker targets are authored
only; none has compiled or run. Dense-layout throughput, real plugin/corpus and
GUI compatibility, audio/resource measurements and full autonomous production
remain unverified. The complete-application commission remains active.


### Bounded native control discovery and session receipts (source implementation, 2026-10-13)

The VST transport can now transfer large layouts, but the studio's discovery
operations still serialized all matching controls into one result. A sufficiently
large result exceeded the durable receipt archive's 16 MiB limit, causing a read
to fail during manifest publication and stop the session. Merely increasing the
worker's message capacity would not resolve this native session boundary.

Parameter, instrument, effect and choice inspections now expose continuation
metadata and bound returned arrays by both count and encoded JSON bytes. All
native controls remain reachable across pages; owner selectors apply before
parameter pagination, and effect indices retain their native positions. The agent
instructions explicitly require following continuation metadata and rediscovering
after edits. Discovery checks cancellation while collecting and describing models,
with FileResourceIO event dispatch suspended so those checks cannot reenter Qt
while holding model pointers. The existing durable transaction remains the owner
of receipt publication, history and recovery.

`StudioParameterPagingTest` authors 7 cases / 21 native executions. They cover
1,025-control fixtures, all three parameter discovery routes, owner filtering,
duplicate-free traversal, last-control editing, ordered effect pages, complete
choice-label traversal, invalid numeric bounds and cancellation. A 2,049-control
fixture with long native names produces more than 16 MiB of combined metadata;
the test requires bounded pages, replayable receipts, unchanged history/revision
and a usable session. A single oversized choice must return an ordinary error,
after which concise inspection and session inspection still succeed.

Existing subsystem tests now collect actual pages when they require a complete
inventory, including all native MIDI CC models. The Python process harness records
each native request/response separately, and uses `expectedRevision` for continued
pages. All three VST recovery scenarios also assert that paged `parameter.list`
and nested `effect.inspect` return the same handles without changing history.
These native scenarios remain authored and unrun. Python imports, CLI parsing and
four helper checks pass; those checks exercise test instrumentation only.

Source diagnostics pass for 340 changed/new C++ files and 137 native operations;
the eight agent operations and 128 inference-declaration limit are unchanged.
Whitespace checks pass. CMake, Ninja, compilers and qmake remain unavailable, and
all 17 pinned submodules remain uninitialized. Model enumeration and plugin refresh
still precede parameter paging, so total allocation and dense-layout throughput
are not bounded by the response limits. Other inspection metadata is outside these
array budgets. Matching clients must follow the new pagination fields. Native
execution, GUI behavior, real plugin/project compatibility, CPU/RSS and complete
autonomous production remain unverified.


### Dense VST session recovery and journal allocation (source implementation, 2026-10-13)

`StudioVstDenseProcessTest` adds a fourth full-process VST scenario, using the
65,537-parameter native ABI fixture through the actual headless LMMS executable.
It requires every native parameter to appear exactly once across bounded pages,
with correct values, native ownership, stable revision and unchanged history.
The combined inventory must exceed 16 MiB while individual parameter arrays stay
within 512 KiB. The first large receipt's archive length and digest are checked
before its result is replayed, while it is still in the recent request ledger.
Nested effect inspection must resolve the same current handles.

The scenario edits the first, middle and last native parameters, binds global
automation to the last, and verifies native rendered PCM. It exercises durable
undo, replay without reapplying an undone edit, redo, native project saving,
graceful reopening, an acknowledged edit followed by abrupt process exit,
recovery, continued editing and original project reloading. It independently
reads every saved native FXP float and legacy XML parameter value. A private copy
of the saved project omits the native preset so that project loading must also
exercise the complete staged legacy parameter restore. Restored automation is
checked against freshly discovered native model handles.

Complete requests and responses stream to JSONL files instead of accumulating
all dense pages in Python memory. A real run records LMMS process CPU and resident
memory samples, their native measurement interval, and harness wall times. The
native samples identify the actual LMMS PID and process instance; VST worker and
Python resource use are excluded. These measurements have not been collected,
and no performance threshold or whole-process-tree resource claim is made.

Review also found that native journal ID allocation restarted each collision
scan from a random value below 32,768. Dense model layouts repeatedly traversed
the same occupied IDs. `ProjectJournal` now retains an allocation cursor, checks
both live and saved aliases and wraps within the existing 23-bit namespace.
Exhaustion throws an explicit error after one full traversal. Clearing retired
journal entries retains the cursor; saved ID encoding and loaded-file identity
policy are unchanged. `JournalIdAllocatorTest` authors 5 cases / 11 executions:
65,537 unique allocations with linear occupancy-probe counts, actual journal
alias reservations, cursor retention, wrap, bounded exhaustion and invalid
namespace/cursor refusal. These native tests are uncompiled and unrun.

The process-fixture build target now includes fixture 3, and the dense CTest
scenario has a 900-second timeout. Source diagnostics pass for 342 changed/new
C++ files and 137 native operations; Python syntax/import/CLI checks and whitespace
checks pass. CMake, Ninja, C++ compilers and qmake remain absent from PATH, and all
17 pinned submodules are uninitialized. Native execution, dense-layout throughput,
GUI and third-party/project compatibility, audio/resource evidence and full
autonomous production remain open. The complete-application commission remains
active.


### Durable large results in native agent conversations (source implementation, 2026-10-13)

Bounded discovery pages could still overflow an agent session: each native result
was copied into both active provider input and durable tool records. A long batch
could exceed the 16 MiB manifest limit before the provider had another opportunity
to request context compaction. Normal commits now move large responses, and older
moderate responses under aggregate pressure, into immutable full-record archives.
The provider receives checked retrieval references in the same call/output slots.
The complete native result remains available through `agent.history.read` by call
ID, both before and after compaction or restart. Native operations and model
ownership are unchanged.

`StudioToolResultTest` originally authored 11 cases / 35 executions under QCoreApplication.
The current inventory is 14 cases / 50 scenarios, including the preview and
publication checks described in the current protocol above.
Coverage includes chat and Responses batches whose original duplicated results
exceed 16 MiB, small committed manifests, exact provider call/output pairing,
UTF-8 payload retrieval, context/record and call selection, compaction, reopening,
archived jobs, missing or changed payload refusal before native replacement,
fresh evidence refusal after tampering, native project-load rollback when payload
publication fails, legacy inline migration, pending-call recovery and call-ID
reuse refusal. Rehashed payloads must still match the committed call and outcome;
nested references are rejected. A scripted provider retrieves a stored inventory
and uses its actual tempo to request a native revision. This tests the runtime
path, not independent musical reasoning or listening quality.

Source diagnostics pass for 343 changed/new C++ files and 137 native operations;
the eight agent operations retain the 128 inference-declaration budget. Python
syntax and whitespace checks pass. Native compilation and the original 35 executions remain
unrun: CMake, Ninja, C++ compilers and qmake are absent from PATH, and all 17 pinned
submodules remain uninitialized. Actual provider continuation, native performance,
CPU/RSS, GUI/plugin/project compatibility, audio correctness and the complete
autonomous production cycle remain unverified. The full commission remains active.

### Bounded native arrangement observation validation record (2026-10-13)

The bounded project protocol described above is implemented in source. Automatic
agent observations now request the compact native summary directly, track/clip/
note pages preserve native ownership and indices, and native project new/load/
import can return summaries. `StudioProjectInspectionTest` has nine authored cases
and 28 executions, including dense and oversized metadata, native ordering,
large-project durable load/recovery, cancellation and scripted note discovery and
revision in both provider protocols. None has been compiled or run.

Source diagnostics pass for 344 changed/new C++ files and 137 native operations,
with Python syntax and whitespace checks. No native build tools or LMMS executable
are available, and all 17 pinned submodules are uninitialized. The full snapshot
and other capability inventories remain outside the new response limits; native
loading/checkpoint allocations, CPU/RSS, real provider reasoning, audio and complete
autonomous workflow behavior require native validation. Full completion remains
unproven.

### Bounded mix/automation and native history validation record (2026-10-13)

The bounded mixer/rack/global inventories, tick-based curve pages, native target
pages, compact mutation responses and compact undo/redo responses described above
are implemented in source. Undo and redo are individually declared agent tools;
the complete supplemental catalog has ten operations, with 137 native production
operations and the existing 128 function-declaration limit.

`StudioMixInspectionTest` has 12 authored cases / 57 executions. It includes native
inventory/curve value preservation, signed tick boundaries, long labels, aggregate
target metadata above 16 MiB with receipt replay, absent/unbound curves, invalid
selectors, cancellation, compact edits, a 200,001-point native curve with undo/
replay/redo/reopen/continued editing, and scripted agent discovery/revision/history
restoration in both provider protocols. The tests are uncompiled and unrun.
Source diagnostics cover 345 changed/new C++ files. Native tools and executable
remain unavailable and all 17 pinned submodules remain uninitialized. Allocation,
CPU/RSS, actual provider reasoning, audio and ecosystem compatibility require native
validation; full completion remains unproven.

### Native agent decisions after project replacement (2026-10-13)

Successful native checkpoint undo/redo and project new/load/import now retain the
successful result and durably reject the rest of the same inference batch before
returning to reasoning with a fresh observation. This closes a source gap where a
queued numeric track index could silently edit a different restored track.

`StudioAgentRestoreTest` authors six cases / 30 executions across both provider
protocols. Coverage includes direct/catalog routing, skipped edits/reads/render/
finish, native history/revision and operation counts, current observation contents,
continued edits after rediscovery, rejection of skipped evidence and reused IDs,
restart, archived large replacement responses, failed manifest publication with
native rollback, and intact ordinary composition batches. Four MIDI-import rows
require the built native importer and explicitly skip when it is unavailable.

Source diagnostics pass for 346 changed/new C++ files and 137 native production
operations plus ten supplemental agent operations. Python syntax and whitespace
checks pass. The native target is uncompiled/unrun; toolchain and executable remain
unavailable and all 17 pinned submodules remain uninitialized. Other structural
changes and complete native/audio/provider/ecosystem/CPU-RSS behavior still need
verification. The full commission remains active and incomplete.


Queued native selection verification (2026-10-13): `StudioSelectionTest` adds
seven cases / 47 authored executions. Nineteen structural scenarios run through
chat/direct declarations and Responses/catalog dispatch, checking paired failed
outputs, native state, operation and revision counts, fresh observation and
continued editing. Further cases cover note assignment/copy/clone and forced
address reuse, source/destination clip resolution, missing/invalid future
indices, native global and playback-pattern lifetimes, unaffected/new targets,
restart and failed-manifest rollback. None has been compiled or run. The existing
ordinary composition and whole-project restoration fixtures remain registered.


Replanning and abandoned-decision verification (2026-10-13):
`StudioReplanningTest` authors five cases / 54 executions. They cover both provider
protocols, project/provider/both changes, zero or partial batch execution,
restart, exact per-call history, operation and memory preservation, call-ID reuse
and invalid-evidence rejection, fresh native observation and editing, replacement
of paused/cancelled jobs, replay, archive/manifest failures, and unchanged resumes.
None has been compiled or run. The new archive-size guard has source coverage;
no boundary-size native execution was obtained. Older archives whose pending
outcomes were already discarded are not retroactively reconstructed.


History record discovery verification (2026-10-13): `StudioHistoryIndexTest`
authors six cases / 19 executions for both protocols, referenced/inline/final
contexts, restart, record order and exact reads, unchanged whole-context bytes,
stored large payloads and skipped calls, 64 KiB pages with escaped/Unicode errors,
cancellation, legacy missing records, invalid selectors, corrupted archives and
receipt replay, and scripted discovery/readback followed by a native tempo edit.
These are uncompiled/unrun fixtures; scripted recovery is not proof of model
musical judgment, native resource use or complete production continuity.


Native headless track solo continuation (2026-10-14): `track.edit` now reaches
core-owned solo isolation without constructing TrackView. The native Track
constructor connects its solo model directly to the existing policy, so native
setters, automated model writes and parameter access share that effect. Selecting
solo unmutes that track, mutes peers and clears other solo flags within the same
container; the modern preference preserves automation-track mute states, while
`app.sololegacybehavior` includes them. Switching solo retains the original
pre-solo model mute caches; clearing the last solo restores them. Song and
PatternStore rows remain separate. Requested `muted` is applied before `solo`, so
the returned effective mute can differ from the request when solo is selected.
Native controller and linked-model precedence remain in force.

Native track/project/linked-state restoration preserves serialized masks and
pre-solo caches rather than replaying incoming flags as commands. Portable preset
loading that ends an active solo restores peers using the old pre-solo cache;
cloning a solo track performs its final solo transfer in core after restoration.
The former GUI connection and clone-only follow-up were removed to avoid duplicate
policy execution. Existing GUI buttons still write the same native models.

Solo snapshots weak peers before notifications, skips retired/detached peers,
stops when its source retires and scopes its internal transition guard across
ordinary exceptions. `track.edit` checks its weak target/container after native
move/name/mute/solo callbacks and reports the current row after an edit containing
`to`. These checks do not make intermediate model signals atomic or resolve
conflicting external reentry. Native per-control journalling remains unchanged;
there is no new claim of one compound native undo checkpoint for solo switching.
Session undo/redo and returned-error recovery still use whole native project state.

`StudioTrackSoloTest` authors 17 cases / 24 scenarios, including modern/legacy
policy, switching caches, automated writes, PatternStore isolation, stored state,
linked restore scope, preset and clone behavior, peer/source retirement, exception
scope cleanup, structured receipts, durable history/replay/reopening, returned
rename/move retirement and solo failure, cancellation and four scripted
Chat/Responses direct/catalog routes. All remain uncompiled/unrun. Source checks
pass across 469 changed/new C++ files and 162 native operations; the tool catalog
retains its 128-declaration limit. No native GUI/legacy/plugin/DSP/audio/hardware,
CPU/RSS/latency or real-provider complete musical production is established.


Native track lifecycle continuation (2026-10-14): `track.remove` validates idle
native recording before any solo or removal mutation. The core
`Track::requireIdleForRemoval` checks the track's note recording and owned clips.
For a pattern owner it also validates the native column mapping, complete row
cell counts and recording state of every pattern row/cell, because native pattern
removal repositions surviving cells. Admission never creates missing cells.
The operation rechecks ownership and admission after cooperative work and after
clearing solo. Solo restoration notifications happen while the whole track is
alive, before entering native destructors. Ordinary pre-teardown notification
exceptions and returned retirement are rejected through normal session recovery.
An effective solo that remains active is refused before deletion. Raw teardown,
loaders and GUI deletion admission retain their existing behavior.

Native TrackContainer detachment no longer holds its roster lock while clearing
solo. It scopes read/write locks around roster access, preserves the engine change
guard across the operation, uses weak owner/target handles and re-finds the exact
target after notifications. Native callbacks can read the container, move or retire
peers without a stale erase iterator; a nested detachment does not emit a second
removal. Conflicting callbacks can still interrupt mask restoration. Direct native
exceptions can leave partial masks and are not an atomic transaction.

`track.clone` now reports the duplicate's current registered row after completion
notifications, including a callback-driven reorder. Core clone completion verifies
registration and cleans an ordinary detached duplicate instead of releasing an
unowned object. Removing or cloning can still reorder numerical selections; use
the returned row and rediscover state before further operations. Both operations
now share the protected model-work scope with track editing.

`StudioTrackLifecycleTest` authors twelve cases / 25 scenarios for native container
reads, peer retirement, nested detach, move and exception behavior, structured
solo removal, seven recording/sparse-pattern refusals, clone receipt reorder,
native pattern cell identity/content/selection, durable removal undo/redo/replay/
reopening, four pre-teardown/clone failure recoveries, cancellation and four scripted
Chat/Responses direct/catalog routes. All are uncompiled/unrun. Native destructor
and plugin cleanup exceptions remain unsupported; native per-control journalling
has no new compound track removal/clone undo guarantee. Source diagnostics pass
across 470 changed/new C++ files and 162 native operations. Actual native build,
GUI/legacy/project/plugin/DSP/PCM/hardware, performance and real-provider complete
musical production evidence remains missing.


Native pattern query continuation (2026-10-14): pattern length and selected-column
automation evaluation read existing cells without calling the lazy getClip accessor.
Absent/empty columns retain a one-bar metadata fallback, including safe handling of
negative selectors; nonpositive meter returns zero. Native playback rejects an
invalid column, skips missing cells and refuses a rounded loop whose tick extent
is nonpositive or outside signed native tick range. It retains native signed
pre-roll coordinates. Pattern automation queries return no values for invalid
columns, negative query time or an unrepresentable loop, and retain their existing
clamp at the pattern end for valid requests.

Explicit column reads retain native row precedence even if editor cell positions
are not globally sorted. Song-range automation still uses the existing chronological
collection. Querying sparse automation rows no longer asserts in debug builds or
creates cells in release builds. MIDI extent rounding uses widened arithmetic;
PatternTrack playback and song pattern automation validate widened loop products
before modulo. Playback offset addition is widened and narrowed only after the
native signed remainder, preserving pre-roll while avoiding signed tick overflow.

`PatternQueryTest` authors thirteen cases / 29 scenarios for extreme/absent column
indices, sparse queries/dispatch, negative automation time versus native pre-roll,
local timing/row precedence, read-only target values, end clamping, six native
length boundaries, loop wrapping, extreme signed offsets, signed song pre-roll,
invalid meter/rounded-loop refusal, empty projects and native project round trips.
Its observed instrument subclass records native dispatch coordinates without
producing PCM. All cases remain uncompiled/unrun. Source diagnostics pass across
472 changed/new C++ files and 162 native operations; no operation was added.
Other raw GUI/sizing arithmetic and malformed-project behavior remain unverified.
Raw pattern deletion and callback-driven mutation during destruction retain their
existing unsupported limits and are not made transactional by this query change.
Native build, compatibility, actual audio/hardware, resource use and real-provider
complete music production remain unproven.


Native automation interruption continuation (2026-10-14): automation evaluation now
checks whether its native playback context still matches after recording/controller
ownership/value notifications. A callback that stops, restarts, pauses, seeks,
replaces the project, or changes/retires the selected pattern abandons the saved
pass before subsequent old-context writes. The context uses native playback
generation, mode/state, actual position and weak registered pattern identity.
A fresh pass can apply newly observed values after restart. Native tempo changes
that retain this context continue through the existing automation path.

The native buffer snapshots weak playback tracks and consumes interruption before
metronome/track work. Returned retirement/detachment of later tracks is skipped;
transport/position changes after a track's callback stop the remaining dispatch.
Timeline increment callbacks are checked before overwriting their frame offset,
with widened expected-tick admission. Completed-buffer frame counts exclude an
abandoned buffer even when earlier native calls in that buffer already had effects.
There is no new structured operation or provider declaration change.

`SongAutomationInterruptionTest` authors twelve cases / 42 scenarios across five
transport interruptions at ownership, value, prior-controller release, recording,
buffer and timeline stages; later-peer deletion/detachment; track callback transport
changes; fresh restart; retained tempo automation; and selected-pattern change/
retirement. Its instrument subclass counts native dispatch without synthesizing
PCM. All cases remain uncompiled/unrun. The prior ownership fixture uses the new
private weak-track seam and keeps its eleven cases / sixteen scenarios.

This is a continuation check, not rollback or preemption of a native setter/plugin
call. Earlier writes remain observable. Pause/seek/pattern changes can retain
current controller ownership until the next pass or stop. Arbitrary nested DSP/
automation reentry, reverted context changes inside one callback, curve binding/
geometry mutation without transport change, callee/self/parent/global/plugin
retirement, concurrent mutation and throwing cleanup retain unsupported limits.
Weak snapshots add allocation and membership checks have no measured real-time
bound. Source checks pass across 473 changed/new C++ files and 162 native operations;
native build, GUI/project/plugin/DSP/PCM/hardware, performance and real-provider
complete production remain unverified.


Native automation take commit continuation (2026-10-14): staged recording publication
requires capture to be disarmed. The existing native recording operation already
disarms before committing. Commit snapshots its receiver/take lifetimes, original
and captured maps, recording counters/configuration/error state, binding order and
identities, owner membership, geometry, interpolation/tension and meter at entry.
Cooperative checkpoints run outside the clip mutex; changed context refuses
publication. Full map comparison occurs at final admission and again under the
publication lock, so a direct native node edit cannot be silently overwritten.
Native AutomationNode equality includes owner, position, in/out values, tangents
and tangent locking.

Prepared nodes are rebound to the receiving curve before map publication. The
pending take is retired under the mutex, then native sizing and final dataChanged
notification run outside that mutex. Returned receiver retirement is checked after
checkpoints/final notification. A post-publication exception can retain committed
data and a retired take; this remains an observable native boundary, with broader
whole-session recovery supplied by the application. No compound native journal
checkpoint was added.

`AutomationTakeCommitTest` authors sixteen cases / 28 scenarios for native content/
node rebinding, discarded/replaced takes, additional samples, eight context changes,
four direct node edits, new recording error, legacy disconnected-target retirement,
clip detachment, active capture refusal, first/final checkpoint exceptions, returned
receiver retirement, cross-thread mutex readers, independent name/mute retention
and post-publication notification failure/retirement. All remain uncompiled/unrun.
The snapshot begins at commit entry, not at reservation; earlier same-context edits
during capture and external parameter scale/controller changes are not universally
pinned here. Native sizing, arbitrary reentry/callee/global/plugin destruction,
concurrent mutation, throwing cleanup and malformed-state behavior retain shared
limits. Source checks pass across 475 changed/new C++ files and 162 native operations.
Actual native recording, build/compatibility/audio/hardware, resource utilization
and real-provider complete production remain unproven.


Native automation reservation continuation (2026-10-14): the source baseline now
begins at reservation, closing the gap documented in the previous commit change.
It preserves an independent native point map, pending binding IDs, weak owner and
binding identities, geometry, interpolation/tension/meter and direct target min/
max/step/scale/automation support. The point map explicitly detaches from the live
map, so an AutomationNode reference retained before reservation cannot silently
change the baseline.

Native capture checks context and target dimensions before accepting each sample;
an invalid sample context latches the existing recording error. It avoids scanning
the whole point map per sample. Visible point conflicts are detected by the full
comparison before commit, preserving the user's edit and refusing publication of
the older take. Commit checkpoints also validate the same reservation baseline.
Discarding permits a new reservation that includes the current visible state.
Ordinary control values, clip name and mute remain editable. Effective source
restoration without an invalid sampled context may be accepted; an already latched
capture error still requires discard.

Detached take construction now occurs outside the receiver mutex, because its
native constructor emits length notifications. Returned receiver retirement,
source changes or a nested active reservation are checked before publishing the
new take. The unused internal length copy was removed; detached take sizing stays
disabled. Commit/discard retire the source baseline together with the take.

`AutomationTakeReservationTest` authors fourteen cases / 40 scenarios covering
fourteen changes before commit, twelve capture context changes, deferred visible
point conflict, a retained mutable node reference, restored context and latched
error, fresh retry, normal value/name/mute changes, constructor callback editing/
retirement/reader/nested reservation, three checkpoint target metadata changes and
forced target-address reuse. All remain uncompiled/unrun. The existing commit and
application recording suites remain authored evidence only.

The baseline is process-local and does not preserve an unfinished take through a
restart. Complete linked-controller/driver state remains the caller's responsibility;
StudioRecording retains its broader checks. Native post-publication callbacks,
arbitrary callee/global/plugin deletion, other reentry/concurrent mutation/throwing
cleanup and malformed state retain shared limits. The independent map adds storage
and copy work proportional to point count, with no measured CPU/RSS/latency or
capture deadline. Source checks pass across 476 changed/new C++ files and 162 native
operations. Native build, compatibility, actual capture/PCM/hardware and real-provider
complete music production remain unproven.


Native prepared curve edit continuation (2026-10-14): `automation.point` and
`automation.edit`, together with their global and note-detuning routes, now share
AutomationClip::editCurve. Point data and optional progression/tension are prepared
with the native putValues/setNodeTangents routines, bypassing GUI quantization.
The complete native map/settings publish under the clip mutex before final native
notification. A point call remains an applied edit and returns changed; empty/equal
settings skip notification. Native null-target cleanup remains part of point edits.
Raw GUI setters and clear/remove/connect/disconnect retain their prior contracts.

Checked edits require the model owner/application thread, a ready engine, idle
recording/reservation and no active curve drag. Preparation snapshots independent
native source data and binding dimensions, checks cooperative cancellation/context,
validates point/tangent fields, source tick range and finite prepared node data,
and admits final geometry before publication. It preserves native song automatic
length/offset refresh and PatternStore length/offset rules. Settings-only edits keep
geometry. Final state is checked after sizing and notification. Note-detuning point/
settings calls retain the helper during native construction and check MIDI owner,
note lifetime and helper identity before returning a receipt.

`StudioCurveEditTest` authors seventeen cases / 38 scenarios for complete notification
contents, invalid/busy/no-op input, preparation/final callback faults, song/pattern
sizing, mutex readers, native tangent unlocking and GUI-grid independence, all three
structured curve locations, session failure/replay/undo/redo/reopening, nonfinite
generated tangents, unsupported source ticks, cancellation/interference and detuning
owner retirement. All are uncompiled/unrun. A Qt keyword collision in an older
authored test was also corrected; source diagnostics are not C++ compilation.

This prepares curve data, not an atomic whole-project operation. Temporary native
curve construction can emit global length notifications. Native automatic sizing
and final callbacks remain observable; direct returned failure may retain published
data. Session recovery supplies the broader project rollback. No compound native
journal checkpoint was added. Checked point edits refuse unsupported existing
source ticks/nonfinite prepared data; raw editing/repair behavior remains available.
Global/inline object creation and outer inspections are not universally transactional.
Arbitrary executing-callee/parent/global/plugin destruction, reentry/concurrent
mutation/throwing cleanup and external effects retain shared limits. Point-map copies
and callback work have no measured CPU/RSS/latency or hard deadline. Source checks
pass across 477 changed/new C++ files and 162 native operations; native build,
compatibility, PCM/hardware and real-provider complete production remain unproven.

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

### Native value receiver failures and LFO timing (2026-10-15, source only)

NativeValueNotification now provides an internal stack-owned reporting frame for synchronous native value delivery. Receivers catch locally and retain the first exception_ptr; the emitting native caller rethrows after Qt delivery finishes. Nested scopes restore their predecessor without accessing a QObject root. Reports use the current thread's active scope, so a render-thread emission works without changing QObject affinity; another thread cannot report into that emission's frame. Empty/outside reports reject. No per-model/controller callback/exception field, virtual interface or studio operation was added. This production frame supplements the earlier private model test probe; reported receiver errors take precedence over that probe.

The frame covers explicit AutomatableModel changed/unchanged notifications, native value/topology journal notifications through that dispatcher, Controller periodic updates, binding rewires and MidiController/AutoDetectMidiController CC updates. Existing signal-to-signal model forwarding inherits the frame from those native origins. The LFO waveform/speed/multiplier receivers now catch/report locally. Its engine sample-rate receiver separately uses AudioEngine's existing notification frame, preserving setup failure and retained NotificationException restoration/retry behavior. The normal MIDI autodetection view receiver also catches/reports or diagnoses unframed delivery. Arbitrary other receivers remain responsible for containing their own errors.

LFO duration evaluation now checks returned LFO/engine/rate context after controller-backed speed and multiplier reads. Nonfinite/nonpositive duration is rejected before replacing the previous value; ordinary calls retain their throwing contract. Existing float arithmetic and multiplier factors remain. A nested device switch can establish a newer duration, and the older evaluation refuses to overwrite it. These are returned-context checks, not safety for deleting an executing getter/model/parent or replacing global roots during native calls. Unframed engine-rate delivery diagnoses locally rather than throwing through Qt.

NativeValueNotificationTest authors four groups / twenty-three scenarios covering six emitter families with standard/nonstandard first-error identity, later observers, nested native setters, empty/outside/thread isolation and receiver-error precedence over test probes. LfoRateNotificationTest authors nine groups / thirty-seven scenarios covering getter/numerical faults, later rate/model/controller observers, export setup and retained restoration, per-phase retry, unframed diagnostics, ordinary calls, nested real WAV device switching, valid sample-rate/multiplier combinations and three actual AudioEngine MixException latch/silence/clock/reset/repaired-output scenarios. Nine scenarios author twelve independently decoded SampleTrack/LFO/native mixer/ProjectRenderer/WAV artifacts. The native getter adapter supplies controlled input faults and does not replace LFO DSP. One existing ControllerState fixture now catches locally, reports and checks later observer delivery before returned failure/retry. All native fixtures remain uncompiled/unrun.

Source checks pass 559 C++ delimiter files / 165 operations. The lexical scan covers 201 test C++/header files with thirty-nine candidates across twenty-eight files, now including locally caught controller clock and lifecycle slots. The count neither proves safety nor finds every indirect failure. Complete core/application/plugin rebuild is required for the new internal exported symbols. Stack frames add TLS/exception-handle work; no native allocation, CPU/RSS/realtime/latency/deadline measurement has succeeded. Direct faults can retain model/binding/packet/phase/journal/callback effects. Raw/manual/meta-object/subclass/unrelated emissions, outer Qt callbacks, other controller conversions/receivers and foreign plugins remain outside any general containment guarantee. Reports have no origin token and must not attribute late/queued or unrelated overlapping work. The controlled getter adapter is unpersisted; these fixtures do not prove project ecosystem compatibility or autonomous musical production. Full Qt/MOC/UIC/link/ABI/thread/sanitizer/GUI/default-plugin/legacy/project/preset/clone/automation/PatternStore/DSP/PCM/MIDI/backend/hardware and real-provider Interpret-Plan-Compose-Render-Listen-Evaluate-Revise-Preserve acceptance remain outstanding. The complete DAW commission stays active and unproven.

### Controller lifecycle receiver failure reporting (2026-10-15, source only)

Song added/removed/roster notifications now use stack-owned thread-local frames holding weak Song identity and the first receiver-reported exception_ptr. Synchronous owner-thread receivers catch locally and report before returning to Qt. The native caller rethrows only after notification delivery finishes. Nested delivery restores its predecessor on return/unwind; empty, outside-frame and foreign-thread reports are refused. This adds an internal reporting method without a new studio operation or per-Song callback/exception field.

The three ControllerRackView lifecycle handlers now catch/report locally. Its Add button and queued Remove command also catch and diagnose errors outside a native reporting frame. Pending view construction has weak cleanup, inserts into the layout before publishing its vector entry/index and releases cleanup only after commit. Song's QObject-destruction roster hook catches locally. Explicit remove holds an outer destruction frame, allowing a reported late roster error to return after the controller destructor finishes. Independent direct deletion has no native caller to receive such an error and diagnoses it locally. This does not make unrelated Qt receivers or throwing destructors safe.

Two existing StudioControllerLifecycle fault methods now catch standard/nonstandard exceptions inside their slots and report them, replacing unsupported Qt unwinding. Their six existing scenarios remain authored. Four persistent add/remove failure rows additionally close/reopen the actual studio, replay the cached failure and check restored controller/binding/manual value/tempo. SongControllerNotificationTest authors six groups / twenty-three scenarios: first-error identity after later observer delivery for all three notifications, nine nested outcomes, rejected empty/outside/foreign-thread reports, native-remove versus independent-delete destruction handling, retired payloads and blocked-signal/frame cleanup. All native C++/Qt/thread/model/persistence/GUI assertions remain uncompiled/unrun.

Source checks pass 555 C++ delimiter files / 165 operations. The lexical scan covers 199 test C++/header files and finds thirty-nine candidates across twenty-eight files. One retained controller lifecycle candidate now contains a local catch; the lexical count is not a count of unsafe receivers and cannot prove semantic containment. Prior model/export/SoundFont work remains source evidence only.

Full core/application/plugin rebuild and GUI refresh/partial-allocation, Qt/MOC/UIC/link/ABI/thread/sanitizer, legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/PCM/MIDI/backend/hardware verification remain necessary. Frames do not pin native callees/parents/roots or reverse partial native/view/plugin/external effects. Direct edits remain distinct from StudioRuntime checkpoint recovery. Retirement listeners, modified notifications, arbitrary direct/meta-object entry points and foreign receivers still need their own contracts. Queued work cannot return a failure to a completed native transition, and overlapping same-Song frames carry no origin token for unrelated queued work. Native/Qt/plugin/layout/allocation/cleanup work remains synchronous/cooperative, with CPU/RSS/realtime/latency/deadlines unmeasured. Real-provider autonomous musical-production acceptance and the full DAW commission remain active and unproven.

### Native model notification fault boundaries (2026-10-15, source only)

AutomatableModel's explicit changed/unchanged notifications and native control-link journal restoration now use one native dispatcher. Its private test installation is thread-local and adds no per-model fields. With no probe installed, notification delivery returns before any probe callable copy or checkpoint weak-reference construction. With a probe installed, the dispatcher copies the callable before Qt delivery and invokes it only after delivery returns, provided the receiver survived and signals were initially unblocked. The scoped test adapter filters the weak target identity, optionally includes unchanged notifications and requires installation/reset on the same thread in lexical/LIFO order. A pending invocation remains owned even if an observer removes its installation. No studio operation or DSP implementation was added.

Twenty-four existing methods across ten suites now inject faults at this ordinary native boundary: setter initialization/controller attachment/tempo, linked propagation, value/topology journal restoration, persistent startup/rollback/commit/undo/replay/session ownership and agent replanning, rack/MIDI assignment/disconnection and MIDI-learn publication. Eighteen explicit throwing connections and six indirect helper-based connections were removed. Mixed controller/MIDI retirement and mutation rows retain real Qt observer coverage. This supersedes earlier fault-fixture descriptions for these paths; neither the old nor repaired fixtures have native execution evidence.

ModelValueNotificationTest adds eight groups / twenty-three authored scenarios covering later-observer delivery before standard/nonstandard errors, manual/automated and unchanged behavior, blocked signals, peer propagation stopping/retry, callable survival through observer/self-removal, receiver retirement, nested lexical scopes and independent worker probe scopes. The worker row allocates/registers and retires its model on the journal owner thread, temporarily moves a detached model for automated-only worker updates, then joins and returns affinity. It does not establish concurrent journal safety. Native setter/journal/runtime assertions remain uncompiled/unrun; no PCM proof is claimed for this suite.

Source checks pass 554 C++ delimiter files / 165 operations. The lexical audit covers 198 test C++/header files and retains thirty-nine candidates across twenty-eight files. It cannot find all indirect native/callee failures or decide whether a local handler contains a throw. This boundary is test instrumentation, not permission for exceptions to escape Qt slots. Even a completed model emission may be nested inside another Qt receiver, which still needs its own local catch. Existing controller signal-to-signal forwarding, subclass/external/manual/meta-object emits and unrelated Model signals bypass it.

Core/application/plugins require rebuilding. No per-model callback storage was added, but installed callback copies and native setters/journals can allocate; thread-local overhead, CPU/RSS/realtime/latency and deadlines are unmeasured. Weak references do not pin executing callees/parents/roots, reverse arbitrary callback/plugin/external side effects or prove universal allocation/cleanup safety. Native C++/Qt/MOC/UIC/link/ABI/thread/sanitizer, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/PCM/MIDI/backend/hardware and real-provider autonomous musical-production acceptance remain outstanding. The full DAW commission remains active and unproven.

### Native song export transition and pending recovery (2026-10-15, source only)

Song export start/stop now use a weak-Song/engine transition guard whose activity is published/released under the existing native model-change mutex. Nested start/stop calls are refused before cleanup exception handlers can change the outer transition, and an already active export cannot be restarted. Failed initialization, including failure returned from its initial stop, now clears exporting and records pending cleanup while updating native MIDI admission. A partially initialized song therefore requires existing audio.device.restore recovery rather than appearing idle enough for another export or accepting fresh external input. ProjectRenderer's normal failure cleanup still attempts native stop/recovery automatically.

Four private post-call test checkpoints cover start timeline/playback and stop timeline/playback boundaries after Qt dispatch returns. Installed callables are retained during invocation; weak scoped adapters reset them. The stop-timeline point also supports ordinary stop fault tests, which still do not become export recovery. Three direct throwing connections in TrackRenderScopeLifecycleTest/Sf2RenderAuditionTest and eight helper-based indirect connections in SongExportRecoveryTest now use these ordinary native boundaries. Existing cleanup, lock-release, MIDI reservation/admission, audition exclusion and recovery assertions remain authored. The native MIDI failed-start row now checks two dropped events, blocked input before recovery and accepted fresh input afterwards.

SongExportTransitionTest authors six QCoreApplication groups / thirty scenarios: standard/nonstandard failure at four phases with explicit recovery, reentry refusal at each checkpoint, playback observer reentry caught inside the slot, active-export restart refusal, eight worker initialization faults and eight transient/persistent worker cleanup faults through RenderManager and studio entry. Sixteen successful retries author independently decoded finite stereo sample-rate/RMS PCM through real SampleTrack/ProjectRenderer/WAV. All native fixtures remain uncompiled/unrun. Source validation passes 552 C++ delimiter files / 165 operations. The lexical scan now covers 196 test C++/header files and reports fifty-seven candidates across thirty-eight files; indirect helper discovery here demonstrates that the lexical count is not a complete semantic audit.

Native C++/Qt/MOC/link/ABI/thread/sanitizer/transport/callback/MIDI/DSP/PCM execution remains unproved. Song layout/private instrumentation requires a complete core/application/plugin rebuild. The guard covers export transition reentry/active restart, not arbitrary direct transport/project/plugin mutation during DSP, deep graph rollback, executing callee/global-root retirement or universal allocation recovery. Probe installation/reset requires control ownership with workers stopped; callbacks can execute on the real render thread and must synchronize their data. Checkpoint faults occur after returned native calls and do not establish escaping foreign-slot recovery. Test PCM and software MIDI assertions do not establish physical silence/delivery, production cancellation latency or autonomous composition/listening. CPU/RSS/realtime/latency/deadline, full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware and real-provider autonomous musical-production acceptance remain outstanding. Whole DAW scope remains active and unproven.

### SoundFont notification failure reporting (2026-10-15, source only)

AudioEngine initialization, device setup and restoration now emit rate/quality notifications inside a scoped owner-thread failure frame. Receivers catch their own exceptions and report a nonnull exception_ptr through reportDeviceNotificationFailure; the first report is retained, subsequent reports do not replace it, and foreign-thread/out-of-frame reports are refused. Nested notifications restore the prior frame on normal return or unwind. The engine rethrows only after Qt signal dispatch returns, preserving native setup failure and NotificationException restoration/retry behavior without unwinding Qt's signal delivery. Completion flags still advance only after accepted notification delivery.

The normal native-engine SoundFont rate receiver now uses a typed weak-engine connection, catches reloadSynth failures locally and reports them into that frame. Unframed delivery produces a diagnostic instead of an escaping exception. Direct reloadSynth/resource behavior, committed-preview restoration, early retained-synth rate conversion, candidate/native synth ownership and actual DSP remain in source; there is no additional DSP failure latch. ProjectRenderer still reports failed SoundFont export preparation as an execution failure before worker startup. This repairs the normal production receiver gap identified by the previous continuation. Arbitrary direct/meta-object invocation of reloadSynth outside that receiver retains its original throwing contract and is not a general slot-safety guarantee.

Eight new AudioDeviceTransition scenarios author first-error/type retention after later observer delivery, nested inner/outer outcomes, cleared frame state and rejected outside/foreign-thread reports; that suite now has twelve case groups / thirty-six scenarios. Sf2RenderAudition adds two case groups / five scenarios: standard/nonstandard setup file callbacks and snapshot/standard/nonstandard restoration failure, later Qt observer delivery, retained NotificationException reservation, per-phase explicit retry and independently decoded actual SoundFont retry PCM. Three retained-rate fallback scenarios additionally check a later rate observer runs before original failure propagation. Sf2RenderAudition now authors twelve groups / twenty-four scenarios and ten decoded PCM scenarios, retaining its required real module/replay/checkpoint case. All native fixtures remain uncompiled/unrun.

Source validation passes 550 C++ delimiter files / 165 operations. Frame/catch/order checks do not prove native C++/Qt/MOC/UIC/link/ABI/plugin import/thread/sanitizer/FluidSynth/DSP/PCM execution. Core/application/plugin rebuild is required for the engine API/layout addition; Instrument's virtual API and studio tool count are unchanged. The bridge assumes live native owner-thread roots, does not replay unframed/queued/foreign-thread failures into unrelated transitions, and does not establish arbitrary foreign-slot/callee/root retirement safety, deep graph rollback or universal allocation recovery. Native work remains synchronous/nonpreemptible, with CPU/RSS/realtime/latency/deadlines unmeasured. Sixty lexical test candidates across forty files and indirect connected-callee audits remain open. Full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware and real-provider autonomous composition/listening/revision acceptance remain outstanding. Whole DAW acceptance remains unproven.

### Ordinary native transition fault injection (2026-10-15, source only)

AudioEngine now provides four private test-peer checkpoints after setup/restoration quality/rate notification dispatch returns. Empty checkpoints do no callable copying. An installed checkpoint copies its callable before invocation, allowing a probe to remove its own installation or retire an export owner while its executing callable remains alive. Restoration marks each notification complete only after checkpoint acceptance, preserving retry of a rejected phase. A scoped, weak-engine test adapter restores the previous installation and resets idempotently. These are ordinary native call-boundary faults, not a supported contract for exceptions escaping connected Qt slots. No public studio operation, substitute codec/plugin/device or DSP was added; AudioEngine layout changes require a complete application/plugin rebuild.

Seventeen fault-bearing methods across eight suites now inject at those checkpoints: RenderManagerPublication/Lifecycle, StudioRenderOwnership, AudioDeviceTransition, StudioExportSettings/Stem, MidiInputRender and StudioBackendSelection. Existing native output/decode, destination preservation, scope/device/reservation ownership and recovery assertions remain authored. Twelve new AudioDeviceTransition scenarios cover four points with normal/standard/nonstandard outcomes, removal of the active installation, retained callable lifetime and eventual release, failure return/identity, reservation retry and candidate destruction. That suite now authors nine case groups / twenty-eight scenarios. All native tests remain uncompiled/unrun. The earlier named RenderManagerPublication/StudioRenderOwnership direct-slot fault limitation is superseded for the migrated methods, without claiming foreign Qt-slot recovery.

An executable source-only lexical audit scans 194 tests/src C++/header files. It finds sixty connect-call arguments with explicit throw tokens across forty other files; none remain in the eight migrated files. These are candidates requiring semantic review, not a verdict that each exception escapes or that other calls are safe. Indirect callbacks, getters, macros and native internals are outside the scan. Source inspection also found a production gap: Sf2Instrument::reloadSynth is connected directly to sampleRateChanged and retains throwing resource/preview/export-rebuild paths without a slot-local catch. Its delivery and the existing export-failure expectation require repair; private test checkpoints do not contain that production slot. Native timeline cleanup and other model notifications also remain in the broader audit.

Source checks pass across 550 C++ delimiter files / 165 operations. This does not prove native C++/Qt/MOC/link/ABI/thread/sanitizer/callback/codec/IO/PCM behavior. Probes are owner-thread, lexically nested test instrumentation; non-LIFO/foreign-thread mutation, arbitrary executing callee/global Engine retirement and universal allocation recovery are not established. Checkpoints run after notification completion, so they must not be described as faults inside Qt dispatch. Default GUI/CLI/plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware regressions, CPU/RSS/latency and real-provider autonomous composition/listening/revision acceptance remain outstanding. The complete DAW goal remains active and unproven.

### Studio callback exceptions at Qt event boundaries (2026-10-15, source only)

Studio render cancellation now catches callback exceptions inside its Qt timer slot, retains the original exception, stops polling timers, aborts/joins the native renderer and rethrows after the event loop returns and processing has been joined. Normal timeout/cancellation and structured capability error handling then remain outside Qt dispatch. The production inference HTTP transport similarly catches cancellation and reply-collection exceptions inside connected slots, retains the first failure, stops timers, aborts its actual QNetworkReply and rethrows after the loop/collection return. Collection becomes a no-op once failure, oversize, timeout or cancellation has been recorded. Native DSP and provider protocols are unchanged.

This follows [Qt's exception-safety guidance](https://doc.qt.io/qt-6.8/exceptionsafety.html#exceptions-in-client-code): application exceptions must be handled inside signal-connected slots. Earlier inspection found unsupported direct-slot fault injection in RenderManagerPublicationTest and StudioRenderOwnershipTest; the native checkpoint continuation above replaces those injections in eight suites. Broader fixture and connected-callee audits remain open. The cancellation fixtures throw from callbacks caught inside the production slot and rethrown only after returning from Qt dispatch.

StudioRenderCancellationTest authors twelve scenarios across boolean cancellation/standard/nonstandard exceptions, project/stem and direct/structured capability entry. Test-only progress instrumentation pauses the actual renderer between periods without replacing DSP, codecs, samples or plugins. Cases check active native execution at cancellation, one callback, original exception/error identity, unchanged destination, staging/preferences/context/device/scope cleanup and native retry; all twelve retries author independent decoded finite stereo sample-rate/RMS PCM. StudioInferenceTest adds four production-HTTP loopback scenarios covering standard/nonstandard callback failure for generation/audio-request entry, connection release and fresh transport retry. That suite now authors nine case groups / twenty-one expanded scenarios. The loopback destructor disconnects member-capturing slots before fixture fields retire. Every native test remains uncompiled/unrun; loopback audio payload/replies are transport fixtures, not provider listening evidence.

Source checks pass across 549 C++ delimiter files / 165 native operations. Catch/order checks do not prove native C++/Qt/MOC/link/ABI/thread/sanitizer/network/codec/PCM behavior. Reply-collector allocation exceptions are contained in source without allocator-fault execution; universal out-of-memory recovery, foreign Qt slots, native abort/wait/destructor faults, arbitrary callee retirement/global teardown remain outside this guarantee. Abort cannot preempt a blocked native call. The finite three-second test gate is synchronization instrumentation, not production latency/deadline/CPU/RSS/realtime evidence. Full GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware and real-provider autonomous composition/listening/revision acceptance remain outstanding. Whole DAW scope remains active.

### Studio render reservation and project ownership (2026-10-15, source only)

Studio audio.render now binds returned continuations to weak native engine/Song identities, project replacement generation/filename and the accepted audio-device reservation generation. Admission, asset/cancellation callbacks, worker completion, finalization, restoration and publication check that identity. The cancellation timer aborts when ownership changes. Cleanup after stack renderer destruction/worker joining and stem-scope release restores only its still-owned reservation/preferences and rechecks after native restoration callbacks. Temporary removal also requires the same engine/reservation generation: a newer reservation reusing the old stage pathname retains its output. Uncertain old staging can remain for owner recovery. QSaveFile direct-write fallback is explicitly disabled; the full native ProjectRenderer/codec/DSP path remains authoritative.

StudioRenderOwnershipTest authors eight QCoreApplication case groups / up to twenty-nine scenarios with all codecs enabled (twenty-five with WAV/FLAC alone). Cases cover project/stem success for each enabled codec with digest/preferences/decoded PCM, project clear or filename change during restoration/publication, a newer reservation on separate/reused staging paths through both own cancellation and outer FileResourceIO callbacks, newer ownership on renderer destruction after setup failure, three publication interruption classes across project/stem, transient/persistent device restoration failure, accepted output plus newer output after post-commit renderer destruction, and two stem admission mutations. Up to fifteen scenarios author independent decoded finite stereo sample-rate/RMS PCM (eleven with WAV/FLAC); the post-commit case decodes two files. Enabled codec/decoder failures fail instead of QSKIP. Prior ProjectRendererFailureTest lossy studio requests were corrected to omit bitDepth, which native admission rejects for lossy formats. All native fixtures remain uncompiled/unrun.

Source checks pass across 548 C++ delimiter files / 165 native operations. These prove structure/order only, not native API/ABI/MOC/link/thread/Qt/callback/sanitizer/codec/OS/PCM behavior. Identity checks do not freeze a deep project graph, undo callback edits, validate arbitrary foreign/raw ownership bypass or guarantee retirement inside an executing callee/constructor/destructor/global engine teardown. Native worker execution still requires live authoritative roots/device. Generation supersession skips old preference restoration and retains uncertain stage paths; new ownership is responsible for recovery. Stage naming/removal and per-file atomic publication do not establish hostile-link isolation, same-size tamper authentication, storage durability/capacity or crash/batch atomicity. A file committed before a later destruction callback stays accepted. Native calls remain synchronous and nonpreemptible, with CPU/RSS/realtime/latency/deadlines unmeasured. Full optional-codec preservation, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware and real-provider autonomous composition/listening/revision acceptance remain outstanding. The full DAW commission remains active.

### Native renderer format refusal and encoding-failure recovery (2026-10-15, source only)

ProjectRenderer now validates native format indices before indexing its codec table or calling a factory. Negative, sentinel and out-of-range formats produce an unready renderer without opening the requested output; extension lookup returns an empty QString for those values. Enabled formats still select the existing production native codec, and disabled optional codecs remain unavailable rather than silently selecting WAV.

The renderer records a sticky atomic encoding-failure observation before device installation, after returned setup, at worker period boundaries and after finalization. An already failed encoder cannot install an output device or launch the worker; a failure returned from setup prevents worker startup. During rendering, recorded failure stops subsequent periods, and a new check after each native write precedes progress delivery. The cached observation can be read after recovery deletes the file device without dereferencing that old device. Studio audio.render and native RenderManager report encoding/output-writing failure distinctly from processing exceptions, DSP/plugin failure and cancellation. Existing staging/publication and cleanup preserve prior destination bytes on failure and allow a fresh render attempt.

ProjectRendererFailureTest authors nine case groups / up to forty-two scenarios with all four codecs enabled, or twenty-four with WAV/FLAC alone. Coverage includes five invalid format values through both renderer and manager, enabled native construction, disabled optional-codec refusal, input refusal before installation and during setup without QThread startup, worker refusal without further progress, refusal before finalization retained after recovery, three manager fault phases per codec with prior-byte preservation/staging cleanup/retry, and studio failure with no result artifact followed by retry. Up to sixteen successful retries author independent decoded finite stereo sample-rate/RMS PCM observations (eight with WAV/FLAC). Controlled faults call the actual native encoder with invalid input; no replacement encoder or DSP is used. Enabled construction/decoding failures fail the fixture, with no QSKIP. Every native test remains uncompiled/unrun.

Source checks pass across 547 C++ delimiter files / 165 native operations, including format-bound and failure-check ordering. Input-fault propagation does not prove real disk-full/short-write/permission-loss, library crash, native close-failure or allocation-failure handling; the finalization case injects input refusal after worker exit. Checks remain cooperative between synchronous native calls, with no arbitrary foreign mutation/reentry/callee retirement/global teardown safety guarantee or measured CPU/RSS/latency/deadline. ProjectRenderer layout changes require a full application/plugin rebuild. Optional codec preservation requires enabled builds, and MP3 retry decoding needs independent libsndfile support. Per-file publication, platform/codec/GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/instrument/PatternStore/DSP/MIDI/backend/hardware regressions and real-provider autonomous composition/listening/revision acceptance remain unproven. Full DAW scope remains active.

### Native encoder input and retained packing storage (2026-10-15, source only)

All four native file encoders now pass nonempty input through AudioFileDevice admission before packing allocation or narrowed codec calls. Admission refuses finalized/failed/closed output, null input, nonfinite available stereo samples and counts whose packed float byte size exceeds native int representation. Zero-frame public writes are no-ops; OGG sends end-of-stream only through its private finalization path. Zero channels, zero rate and rates above native int representation refuse construction before opening/truncating output. The count bound applies to each write, not export duration: ordinary engine-period streaming remains the execution path. Caller pointer capacity remains a trusted native contract.

WAV/FLAC float packing and shared native/swapped S16 conversion now pad channels beyond the fixed stereo SampleFrame with zeros. This also changes shared hardware conversion for extra output channels; its backend behavior requires regression execution. WAV/FLAC retain float/integer packing vectors, and MP3 retains interleaved/encoded vectors, calculates LAME output capacity with integer arithmetic and uses a fixed bounded flush array. Valid mono/stereo conversion and FLAC's existing negative-clip workaround remain in source. OGG stream serial creation uses QUuid rather than explicitly reseeding/consuming the process C random sequence; opaque Qt/codec RNG effects remain unverified.

NativeAudioEncoderTest authors ten case groups, up to sixty-two scenarios with all four codecs enabled or thirty-five with mandatory WAV/FLAC alone. Cases cover independently decoded stereo roundtrip with zero writes and close/rename/idempotence, five invalid-input classes per codec, late-write byte preservation, six multichannel format/depth rows, both integer endian modes, five retained packing-storage observations, failed file initialization, OGG C RNG sequencing, native RenderManager staged publication per enabled codec and three invalid initialization classes preserving prior bytes. Up to fourteen scenarios author independent decoded PCM. Enabled-codec constructor/decoder failures fail the fixture; there is no QSKIP. Optional codecs disabled at build time have no corresponding rows, which does not establish full feature preservation. Enabled MP3 needs independent libsndfile decoding support for its strict PCM gate. Every native fixture remains uncompiled/unrun.

Source checks pass across 546 C++ delimiter files / 165 native operations, including Python integer boundary calculations for one/two/four/65535-channel packing and stereo LAME output capacity. These establish source structure/arithmetic only. Finite admission adds a synchronous scan; vector retention is not globally allocation-free rendering or measured CPU/RSS/latency evidence. Codec allocation, buffer growth, direct nonfile caller counts, arbitrary reentry/callee retirement/global teardown and process-wide allocation failure remain outside universal guarantees. Native API/ABI/MOC/link/sanitizer, library/OS closure and channel-ordering/delay/resampling/PCM behavior require execution and a complete application/plugin/codec rebuild. GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware compatibility, provider listening/revision and full autonomous DAW acceptance remain outstanding.

### Native CLI/GUI export destination publication (2026-10-15, source only)

RenderManager now renders each native CLI/GUI output through the full ProjectRenderer and selected native codec into a uniquely reserved adjacent QTemporaryFile. It publishes that completed file with QSaveFile and direct-write fallback disabled. The destination is unchanged while rendering, during returned factory/setup failures and when publication is interrupted before commit. Native AudioFileDevice finalization now flushes and closes output idempotently after codec finalization, allowing publication before a later bulk device switch without depending on an open file handle. Late base-file writes remain errors.

Publication streams bounded chunks through existing FileResourceIO cancellation, checking weak manager/renderer, DSP status, reservation identity and Song identity/generation/filename before commit. A final output waits for successful device restoration; intermediate completed stems commit individually before the next stem. Earlier committed stems remain if a later stem fails, and cancellation after commit does not roll back an accepted file. Owner retirement before commit cancels the QSaveFile destination. Independent staging ownership also covers a native constructor/factory that throws after opening its requested output. Temporary cleanup verifies reservation generation: if a newer reservation deliberately reuses the staged filename, the old frame retains uncertain leftovers instead of deleting newer output.

RenderManagerPublicationTest authors ten QCoreApplication cases / sixteen scenarios: project/stem replacement with prior bytes unchanged during setup, standard/nonstandard setup failures, DSP refusal, publication cancellation and two exception classes, retirement during copy, restoration failure with explicit retry, partial bulk success, native WAV16/24/32 finalization/rename/idempotence, destination-directory preservation and late base-file writes. Seven scenarios author independently decoded PCM. One lifecycle row also adds newer reservation reuse of an old staging path; earlier retirement rows now decode closed staging before retirement and assert no uncommitted destination. All fixtures remain uncompiled/unrun.

Source validation reports 545 delimiter-checked C++ files / 165 native operations. Atomicity is per destination file, not a batch/project transaction or crash-recovery journal. Temporary cleanup remains best effort after persistent device failure or superseded ownership. Staging plus atomic-copy publication adds temporary disk space and O(encoded bytes) copying; chunked streaming does not make native codec/Qt/fs/device/destructor calls preemptible or establish CPU/RSS/latency/deadline evidence. Hostile-link isolation, authentication of same-size external staging edits, storage durability, disk-capacity and OS sharing behavior are not proven. The encoder continuation above additionally authors closure/PCM checks for every enabled codec; all WAV/OGG/MP3/FLAC native execution remains outstanding. AudioFileDevice/RenderManager contract and layout changes need a full application/plugin/codec rebuild. Full C++/Qt/MOC/link/ABI/sanitizer, native callback/atomic-replace/DSP/audio/hardware execution, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/MIDI/backend compatibility and real-provider autonomous musical/listening/revision acceptance remain outstanding. The entire DAW commission remains active and unproven.

### Native renderer creation failures and stale callbacks (2026-10-15, source only)

RenderManager now contains exceptions and null results from renderer creation/setup, stops a failed bulk queue and finishes through existing native device/scope cleanup. A translated failure message is cached when the manager is constructed, so the catch path copies an owned QString instead of constructing exception diagnostics after an allocation failure. Creation failure is distinguished from explicit cancellation, and state resets permit the same manager to retry. Factory-time cancellation cannot launch the returned renderer, while a factory that releases/replaces the device reservation cannot transfer its newer scope to the old job. The protected creation method defaults to the complete native ProjectRenderer; the new seam changes RenderManager's vtable and requires a full rebuild.

Queued progress and completion callbacks now carry a weak renderer identity and compare it with the manager's current renderer before delivery. An old renderer's posted events cannot advance or update a reused manager. These identities do not keep an already-returned renderer alive; local shared ownership still protects executing native calls and releases unattached output before failure removal.

RenderManagerConstructionTest authors eight QCoreApplication cases / twenty scenarios: allocation/standard/nonstandard/null failures before project/stem output preserving prior bytes, exceptions after real native output construction with renderer destruction and retry, all four precreation failures followed by same-manager render recovery, returned/throwing factory owner retirement, factory-time cancellation, nested start refusal, reservation supersession and a failed bulk queue stopping before explicit successful retry. Eight scenarios author independently decoded native SampleTrack/ProjectRenderer PCM. Successful factories delegate to production native creation, with no replacement DSP. One existing lifecycle case also posts old native progress/completion signals, starts a replacement and checks exact completion count plus exclusion of stale progress. All native fixtures remain uncompiled/unrun.

Source validation reports 544 delimiter-checked C++ files / 165 native operations. Typed allocation errors are injected at the creation boundary; these fixtures do not exhaust process memory or inject failures inside every Qt/codec/library constructor. Cached diagnostic assignment reduces recovery allocation, but surrounding signal delivery, restoration, destruction and other native work may still allocate/fail; this is not universal out-of-memory recovery. Native factories must return a newly owned, unstarted renderer for the requested output. The publication continuation above now owns staging independently of returned renderer creation and commits per file; native filesystem and complete regression proof remain outstanding. Arbitrary callee self-retirement, foreign QObject mutation, global engine/root teardown and deep project rollback remain outside this source guarantee. Full C++/Qt/MOC/link/export/vtable/ABI/sanitizer and actual queued-event/native lifetime/audio execution, default-plugin GUI/CLI/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware compatibility, CPU/RSS/latency/deadline and real-provider autonomous musical/listening/revision evidence remain outstanding. Whole DAW acceptance remains unproven.

### Native renderer lifetime and reservation ownership (2026-10-15, source only)

RenderManager now keeps the actual native ProjectRenderer alive through local shared ownership while setup, finalization, abort and console-progress calls execute. Manager retirement marks cancellation before detaching renderer/selection ownership, cancels setup even before the worker starts, and joins processing before clearing selection flags. Returned weak manager continuations replace the starting-state reference rollback and avoid resuming retired member state. An orphaned returned call releases the renderer's native context before attempting device recovery. Persistent native restoration failures retain the existing reservation/status for explicit audio.device.restore retry. Finalized files can remain after owner retirement without any manager completion receipt; unfinalized owned output is removed after closure when possible.

Completion now serializes reentry across finalization, device restoration and renderer destruction, checks owner survival after native callbacks, contains finalizer exceptions and rechecks cancellation after renderer destruction. It dismisses its old cleanup guards before finished can synchronously start another job. Setup releases its temporary renderer pin before ordinary failure cleanup, so an unattached file closes before removal on Windows. Native AudioEngine stores now allocate monotonic uint64 reservation generations and refuse exhaustion rather than wrap. Manager/orphan cleanup compares engine identity and reservation generation before restoration and partial-output removal, including after QObject destruction callbacks: a later reservation or output on the same path remains outside the old frame's ownership.

RenderManagerLifecycleTest authors fifteen QCoreApplication cases / twenty-three scenarios. They cover four quality/rate project/stem setup retirements with actual renderer survival/destruction observations, two setup exception classes after retirement, two restoration retirements retaining finalized PCM, two late restoration cancellations, renderer destruction retiring or cancelling the manager, two transient restoration failures, persistent restoration failure and explicit recovery, immediate same-manager completion reuse, restoration reentry refusal, a newer reservation and native WAV on the same path, monotonic accepted/refused stores, progress retirement closure/removal of an uninstalled DSP-refused file and exclusion of old queued progress/completion during a replacement render. Nine scenarios author independent libsndfile decoding of native SampleTrack/ProjectRenderer output, plus two decodes of native AudioFileWave replacement output. An additional RemotePluginTest case retires a manager during a real fault-host hung-rate setup wait, authoring cooperative cancellation, timing and cleanup assertions; that suite now has twenty-three cases. These are native fixtures, not GUI automation or substitute DSP. All remain uncompiled/unrun.

Source validation reports 543 delimiter-checked C++ files / 165 native operations. AudioEngine and RenderManager layouts changed and require a full native application/plugin rebuild. Reservation generations identify accepted storeAudioDevice scopes rather than arbitrary unreserved device replacements or an immutable project transaction. Arbitrary executing native constructor/callee/getter/destructor internals, foreign QObject mutation and global root/engine teardown remain outside universal safety or rollback. The creation continuation above contains returned creation/setup faults in source; actual allocator/library constructor/finalizer behavior and process-wide memory exhaustion remain unverified. Orphan restoration/removal is best effort, native library/DSP/host/codec/destructor waits remain nonpreemptible in their existing boundaries and timing assertions are not measured latency or hard deadlines. The publication continuation above adds staged per-file commit in source; batch atomicity is not established. Full C++/Qt/MOC/link/export/ABI/sanitizer and actual signal/worker/shared lifetime/host-platform execution, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware compatibility, actual audio, CPU/RSS and real-provider autonomous music/listening/revision acceptance remain outstanding. The complete DAW commission remains active and unproven.

### Native bulk export eligibility and filename preparation (2026-10-15, source only)

RenderManager, shared by native CLI and GUI export, now prepares bulk eligibility and filenames through owned weak Song/PatternStore roster and mute/solo snapshots. Effective mute getters and cancellable asset callbacks no longer resume live native container iterators. Returned work checks manager/engine/root/thread identities, Song generation, project filename, output clock, native export/remote/pending cleanup and finite stored eligibility. Complete membership and order are checked before the eligible queue is published. Existing reverse queue numbering, native filename filtering, mute/solo values and previously-admitted bulk eligibility semantics are retained; only native instrument/sample rows require an effective mute read.

Output naming owns directory/format values before invoking the virtual name getter, checks returned ownership before copying its borrowed string and polls cancellation around the owned result. A newly constructed TrackRenderScope stays local until manager survival and cancellation have been checked. Weak starting-state cleanup covers eligibility, selection construction and naming; abort is retained even before a ProjectRenderer exists, and nested starts refuse. Standard and nonstandard preparation errors finish through existing native error/device cleanup. If the manager retires during these preparation callbacks, weak continuations avoid publishing into the dead owner and local scope ownership unwinds.

RenderManagerPreparationTest authors twelve QCoreApplication cases / thirty-one scenarios: actual native sample stems and sanitized numbering, six eligibility getter roster/control changes, standard/nonstandard getter failures, getter manager retirement, cancellation and nested starts, four first/final file callback retirements, three file cancellation/exception classes, six filename actor/project/manager/failure paths, naming cancellation, manager retirement during local selection construction, two same-manager explicit-repair/retry rows and two PatternStore eligibility rows. Three scenarios author independently decoded libsndfile checks of native SampleTrack/ProjectRenderer output. PatternStore rows inspect eligibility rather than PatternStore playback. Tests inspect actual queue/atomic flags and independent thread model-lock acquisition after failure. All native fixtures remain uncompiled/unrun.

Source validation reports 542 delimiter-checked C++ files / 165 native operations. Preparation checks are finite ownership/state checks, not deep immutable project/controller/automation/plugin/clip snapshots or rollback of callback edits. Transient changes ending in the same final state can be accepted. O(n) roster identities and collection, plus O(n) validation/naming per stem, remain synchronous and provide no hard deadline or CPU/RSS/latency evidence. The publication continuation above adds staged per-file commit in source; batch atomicity is not established. The renderer-lifetime continuation above replaces member-only setup ownership and the reference rollback in source; native execution and broader lifetime proof remain outstanding. Asynchronous finalize/restore/progress/destructor callbacks, arbitrary executing getter/callee internals, foreign QObject mutation and global teardown remain separate lifetime boundaries. Full C++/Qt/link/ABI/sanitizer, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware, actual audio and real-provider autonomous music/listening/revision evidence remain required. RenderManager layout changed and a full rebuild/regression run is still necessary; the entire DAW commission remains active and unproven.

### Native stem admission callback ownership (2026-10-15, source only)

TrackRenderScope no longer polls cancellable asset work while retaining borrowed Song/PatternStore track iterators. It first owns weak snapshots of both native rosters, then traverses those identities. Admission checks weak engine/root/selected-track/mute/solo ownership, root generation, project filename, output rate, stored selected eligibility and export/remote/pending-cleanup state after asset callbacks and the effective mute getter return. Each visited identity must still belong to its native container. A final linear pass checks complete membership, order and ownership before any selected/excluded processing flags are published. An interrupted or changed admission releases its active reservation and native model guard without publishing partial flags; callback edits themselves are not rolled back. Native mute/solo models and the existing bulk-export previously-admitted policy remain authoritative.

TrackRenderScopeLifecycleTest authors thirteen QCoreApplication cases / thirty-six scenarios. They cover four Song/PatternStore sample/instrument selections, six selected/excluded/project retirements at first/final callbacks, five roster/project/context changes, two already-visited/not-yet-visited retirements at the second 128-row polling checkpoint, two stored eligibility changes, cancellation and standard/nonstandard exceptions with independent thread model-lock acquisition, nested admission preserving the outer reservation, three installed-scope retirement cleanups, foreign-container/thread refusal, three native export gates including pending cleanup with the export flag false, three effective-controller getter context/failure rows, and two structured Studio stem failures preserving the prior output followed by explicit composition repair and independently decoded native sample audio. The fixture reads actual private atomic flags because public selection/exclusion getters deliberately gate them on active Song export. The two decoded cases use native SampleTrack, ProjectRenderer and libsndfile, without an alternate DSP generator or GUI automation. All native fixtures remain uncompiled/unrun.

Source validation reports 541 delimiter-checked C++ files / 165 native operations. These are finite admission checks, not a deep immutable composition/clip/plugin/controller snapshot or general rollback. A transient edit whose final roster matches the accepted identities may still be admitted. Arbitrary executing getter/constructor/callee/destructor internals, foreign QObject mutation, global root/engine teardown and opaque state are not universally guarded. Snapshot/classification/final validation require synchronous O(n) work and O(n) identities; no hard deadline or CPU/RSS/latency result is established. Installed scopes still must be released only after processing stops, and retirement fixtures do not run simultaneous DSP. Instrument rows check flags rather than instrument-plugin audio; PatternStore PCM correctness is not established by these rows. Full C++/Qt/link/ABI/sanitizer execution, GUI/CLI/default-plugin/legacy/project/preset/clone/controller/automation/DSP/MIDI/backend/hardware regression and real-provider autonomous composition/listening/revision acceptance remain outstanding. The complete DAW commission remains active and unproven.

### Native SoundFont retained-synth rate fallback (2026-10-15, source only)

SoundFont reload now prepares the retained native synth for the engine's already-published output clock before fallible resource work. Previously, resampling was updated only after accepted-bank verification: a missing/changed snapshot or early file callback failure could leave the old synth active with its old conversion ratio. Immediately after native resource admission, reload now updates the ratio from the retained synthesis rate to the current output rate and clears buffered conversion state under the synth mutex. It releases that mutex before effective program restoration/getters, cancellation polling or bank verification. Successful replacement still resets conversion again from the accepted replacement's native rate. The existing FluidSynth/libsamplerate pipeline remains authoritative, and export preparation failures still abort through ProjectRenderer.

Sf2RateFallbackTest authors six QCoreApplication cases / twelve native scenarios using production SF2 C++/MOC/UIC, actual FluidSynth/libsamplerate, NotePlayHandle and instrument play buffers. A production AudioDummy subclass supplies explicit native output clocks and is never started; it supplies no synthetic PCM. Five verification-failure rows cover 32000, 48000, 88200, 96000 and 192000 Hz. Further rows cover an absent private snapshot, cancellation and standard/nonstandard file callback failures after audition, a second rejected rate change that clears pending conversion input while retaining the same live native voice pointer/id, successful explicit retry at the new clock with safe old note-data cleanup, and a required built sf2player module across four rejected transitions. Every scenario authors finite stereo PCM, energy and pitch checks. Metadata alteration leaves PCM and RIFF structure intact while invalidating the accepted snapshot hash. A byte-level Python check confirmed unchanged sample/preset tables and a changed hash; it did not execute the native loader or DSP. These private failpoints are not exposed user repair workflows. Two earlier SoundFont dialog/render fixtures also now include the FluidSynth function declarations explicitly.

Source validation reports 540 delimiter-checked C++ files / 165 native operations. All native fixtures remain uncompiled/unrun: the dummy clock does not prove hardware behavior, and direct instrument buffers do not prove complete mixer/device/offline-file/PatternStore output. Allocation/resampler/program-selection failures are not deterministically injected; conversion preparation itself can fail. Buffer reset can discard already-generated input, so acoustic continuity and quality still require execution. Arbitrary native callback/getter/destructor lifetimes, foreign mutation and global teardown are not universally guarded or rolled back. Full C++/Qt/MOC/UIC/link/ABI/module imports, default-plugin GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI regression, actual PCM/hardware/CPU/RSS/latency/deadline and real-provider autonomous musical/listening evidence remain outstanding. The full headless agent-native DAW commission remains active and unproven.

### Native SoundFont audition and export preparation (2026-10-15, source only)

SoundFont auditions are now refused during native export admission and execution. The public preview entry point checks a retained, executing or cancelled RemotePlugin render context before taking the engine model-change guard, then checks file-device ownership, Song export/pending cleanup, device reservation and stem selection under native application-thread resource admission. This early render-context check avoids waiting for a DSP period merely to refuse an audition. Public restoration also refuses a consuming export context, but permits owned-token restoration during an untouched device reservation or stem selection: those admission scopes can end without a sample-rate notification, so cancelling a dialog there must still restore its previous audition.

Committed-state preparation uses the existing ProjectRenderer path. It begins the native render context, installs the file device on the owner thread and only then starts its worker. AudioEngine stops processing and emits qualitySettingsChanged followed by sampleRateChanged during device installation, including when the numerical sample rate is unchanged. The existing SF2 rate slot restores any current preview to checked effective project bank/program controls before verifying its accepted snapshot or trying to rebuild FluidSynth. The private rebuild helper now returns success; a failed rebuild during render admission raises into ProjectRenderer's existing setup-failure path, so no worker starts and finalization cannot report success. Ordinary rebuild failures retain the existing diagnostic/old-synth fallback, with the audition already restored. No new Instrument virtual hook or public Qt slot signature is introduced.

Sf2RenderAuditionTest authors ten QCoreApplication cases / nineteen scenarios against production SF2 C++/MOC/PatchesDialog UIC, real FluidSynth and the calibration SF2. Coverage includes seven admission windows, a standalone file device, Song and idle PatternStore synth preparation through quality/rate/finalization callbacks, damaged-snapshot fallback and export refusal, two sample rates, callback audition refusal, an independently held model-change guard with a refusal timing assertion, cancellation before setup, destination preservation with explicit failpoint recovery, and a required real-module Runtime render with receipt replay and owned-checkpoint reopen after original bank removal. Five scenarios author independent libsndfile decoding and pitch checks for committed versus auditioned programs. The PatternStore row checks synth preparation, not PatternStore PCM playback. Two additional QApplication dialog rows cover Cancel during device reservation and stem selection. Required module discovery fails rather than skips. All native fixtures remain uncompiled and unrun.

Source validation reports 539 delimiter-checked C++ files / 165 native operations. This preparation guarantee is scoped to ProjectRenderer's device-notification path used by native export and Studio render/stems. Bare internal Song::startExport or manual renderNextPeriod without device setup has no new preflight barrier, and arbitrary project/program/controller/automation mutations during export remain outside an immutable project transaction. Audition still changes live playback and existing voices/tails; cancelling before device setup can intentionally leave the owned audition active. The retained-synth continuation above moves clock conversion ahead of fallible preparation in source; acoustic/hardware-rate correctness remains unverified. The fixture's private snapshot corruption/repair is a failpoint, not a demonstrated user recovery workflow. Full C++/Qt/MOC/UIC/link/ABI/module imports, native worker/signal/lifetime and sanitizer execution, GUI/legacy/default-plugin/DSP/MIDI regression, decoded PCM, CPU/RSS/latency/deadline/hardware and real-provider Interpret → Plan → Compose → Render → Listen → Evaluate → Revise → Preserve evidence remain outstanding. Whole DAW acceptance is unproven.

### Native SoundFont dialog and audition ownership continuation (2026-10-15, source only)

The SoundFont patch dialog retains live FluidSynth audition, its filterable program table, numeric sorting and Up/Down/Enter/Escape controls. It now owns its catalog and weak native/root/model/widget context, including heap source/proxy/selection models and the filter field. Setup reads checked effective program values for the initial selection while storing manual bank/program values for context validation. Program/bank tokens, project/routing generations, filename/rate and a setup epoch are checked across returned getters, widget actions and native commit. Old bindings are disconnected and queued notifications check their setup epoch. Caption/catalog values and retired accepted-bank identity remain owned across callbacks. The modal caller uses a weak heap dialog and checks selected view/track/project ownership, replacing the stack child across modal parent retirement.

Sf2Instrument keeps its existing preview API and adds opaque typed preview tokens. Each successful audition issues a token after native selection without changing stored bank/program controls. Only the current token may restore the current effective controls. Restoration checks token identity again after effective getters and before native selection, so a newer audition wins. Committed program/model changes, bank publication/clear/pending state and successful synth rebuild supersede old tokens. A failed preview request preserves the previous restorable token. Expected restoration during fileLoading uses native observation admission and does not admit nested resource changes. Cancel, invalidation, reused setup and destruction attempt restoration of that dialog's current token; filtering all rows also restores the hidden audition. Empty/pending banks cannot open an editable program dialog, and unavailable stored choices require explicit acceptance of a visible valid fallback.

Unavailable effective programs unassign the native channel rather than leaving the audition selected. FluidSynth documents that unassignment retains SoundFont/bank IDs while marking the preset unassigned; fixtures therefore inspect native program validity instead of assuming those IDs clear. See the [FluidSynth MIDI channel API](https://www.fluidsynth.org/api/group__midi__messages.html). This source-level API check is not evidence of local native execution or immediate removal of existing voices/tails.

Sf2PatchDialogTest authors thirty-one cases / sixty-seven scenarios using QApplication, production SF2 C++/MOC/PatchesDialog UIC, real FluidSynth and the calibration SF2. They cover initial selection, three Cancel routes with actual native channel inspection, commits, empty/pending refusal, six actual modal context invalidations, external controls, returned getter and native/widget owner retirement, nested/reused setup/queued notifications, root/rate signals, exceptions, caption, returned bank metadata, missing widgets, unavailable/current choices, old resource cleanup, token supersession and six native invalidations, restore during resource admission, filter/keyboard audition, destruction, multiple dialogs, invalid/null tokens, unchanged serialization, five filter/source/proxy/selection/rebound-model cases two source-row ownership callbacks and two Cancel rows during device/stem admission. One required module route persists the selected program in portable project resources and reopens after original source removal. The existing QCoreApplication resource lifecycle case also authors token restore/replay refusal. CTest requests Qt offscreen; required module discovery fails rather than skips. These native unit fixtures remain uncompiled and unrun, and synthetic rate notifications do not prove hardware changes.

Source validation reports 538 delimiter-checked C++ files / 165 native operations. Complete C++/Qt/MOC/UIC/export/link/ABI/offscreen/module-import verification, native modal/proxy/signal ordering and sanitizer execution remain required. Arbitrary executing constructors/callees/destructors, foreign Qt mutation, global teardown, native callback/parser/voice/tuning/effect and opaque/controller/automation state are not universally guarded. Finite context checks do not roll back getter effects or partial native commits; cleanup catches failures but cannot promise successful restoration after arbitrary native/controller failure. A dialog deleted after successful native publication may leave the authorized program committed. Audition still changes live playback and existing voices/tails. ProjectRenderer admission and preparation source changes are described in the continuation above; arbitrary native mutations and bare manual DSP without device setup remain outside that boundary. No hard deadline, latency, CPU/RSS, actual PCM, hardware or real-provider musical/listening evidence is established. Full default-plugin GUI/legacy/project/preset/clone/bank/controller/automation/DSP/MIDI/export regression and whole DAW acceptance remain outstanding.

### Native GIG patch-dialog ownership continuation (2026-10-15, source only)

The GIG patch dialog now owns an editable program-list snapshot and commits through GigInstrument::selectProgram. Its former audition helper performed no native action; candidate selection therefore remains local until Accept, and Cancel leaves bank/patch models and the selected native instrument unchanged. Setup starts from the stored native selection, falls back visibly when that program is unavailable, and requires explicit acceptance to change it. Accepting the unchanged selection does not republish native program changes. Numeric sorting and one row per bank/program are retained; stable native catalog sorting preserves enumeration order for equal program keys.

The dialog replaces raw GigInstance/libgig iterator, model and label pointers with weak instrument/track/container/Song/Mixer/AudioEngine/model/widget ownership, accepted bank identity/path, project/routing generations, project filename/rate, stored bank/program and a setup epoch. Returned resource getters and widget/native callbacks are checked before continuing. Owned caption/program copies and weak scoped cleanup prevent old setup/commit frames from resuming after retirement or reconfiguration. Prior connection bindings are disconnected on setup; epoch checks also refuse queued notifications from an older setup. File, project, rate and external control changes invalidate the candidate. Changed resource metadata or generation is checked at the next action even without a notification. Resource identity is pinned before replacing it so retired snapshot QObject cleanup cannot resume into a dead dialog. Native commit exceptions stay within the dialog; direct native faults can still leave partial state.

The view's modal caller now uses a heap dialog through a weak pointer and scoped cleanup. It checks weak view/selected track/project context around setup and modal return, and copies the reported track name only after the owner survives the getter. This removes the stack-child dialog lifetime across parent retirement. It does not establish universal safety for arbitrary Qt/native executing constructors, callees or destructors.

GigPatchDialogTest authors twenty cases / forty-five native Qt widget scenarios using QApplication, production GIG C++/MOC/PatchesDialog UIC and libgig-written split banks with an optional second program. Coverage includes initial selections, Cancel/window close, native commit and unchanged acceptance, empty/pending refusal, six actual modal-loop context invalidations, external edits, returned getter/commit/widget retirement, nested/reused setup and queued old notifications, root/routing/rate notification changes, standard/nonstandard getter exceptions, copied caption, returned resource metadata mismatch, required widget removal, unavailable stored programs and retired-bank cleanup that deletes the dialog during reused setup. One required actual-module route commits a program, saves portable native project resources, removes original root/companion files and reopens the selection. The test target uses QT_QPA_PLATFORM=offscreen; missing plugin discovery fails rather than skips. These are native widget unit fixtures, not external GUI automation. All remain uncompiled and unrun; the rate case emits a native notification and does not verify physical sample-rate changes.

Source checks report 537 delimiter-checked C++ files / 165 native operations. Full Qt/C++/MOC/UIC/export/link/ABI, offscreen platform availability, native module imports and actual modal signal ordering remain unverified. Direct getter effects and partial commits are not rolled back by the dialog; a dialog deleted after successful native publication can leave the authorized program committed. Stored controls and bank identity are finite checks, without deep automation/controller/opaque/native-DSP snapshots. The SoundFont dialog source continuation is recorded above; native verification remains open. Full default-plugin GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI regression, actual PCM, hardware, CPU/RSS/latency and real-provider autonomous musical evaluation remain outstanding. Whole DAW acceptance remains unproven.

### Native missing GIG group repair continuation (2026-10-15, source only)

GigInstrument now retains missing bank references as editable pending instruments. Native settings with an absent root, or an unbound root whose declared companion group is incomplete, preserve the resolved absolute root filename and stored bank/patch/gain controls. Pending state has no accepted snapshot or program inventory: native instance/note retirement removes the previous bank, playback returns zero instrument buffers and clears pending queued notes. Other instruments and downstream effects retain native behavior. Explicit `gigplayer/src` plus `unresolved="1"` stays pending when files appear later; malformed references and pending markers conflicting with accepted bindings are refused. Native rate reset retains pending state; explicit resource clear and empty settings remove the retained reference.

FileResourceSnapshot keeps its existing captureGig entry and adds a typed failure overload. Missing companions are distinguished only after complete declaration parsing, safe/noncolliding names, validation of present companions' RIFF/identifiers and final integrity checks for captured root/present members. A missing declaration cannot bypass a corrupt present member or malformed table. Root/copy/read errors, cancellation, unsafe/colliding names, invalid present members and damage to an accepted owned group remain errors. No partial group becomes an accepted snapshot. This establishes structural consistency of an incomplete source, not native program/sample validity; complete libgig validation occurs during repair. Absence comes from best effort filesystem metadata and is not an atomic filesystem/content guarantee.

Repair uses `instrument.resource.set` with resource `bank`, or the native file APIs. A pending instrument requires a complete replacement group containing its stored bank/program. Incomplete/corrupt/missing candidates or unavailable programs leave pending state and controls intact; successful native candidate adoption clears the marker and owns the complete group. Edit native bank/patch parameters while pending, or explicitly clear before choosing an unrelated new bank. Ordinary settings replacement uses its own mode rather than inheriting prior pending repair constraints. Root and controls are durable; absent companion names/content are not preserved as an immutable asset manifest. Capture diagnostics can name missing companions, while pending inventory/checkpoints identify the retained root. Repairing the same root after restoring companions is supported.

DataFile now admits the exact marked `gigplayer/src` reference as unbound absolute checkpoint XML without invented asset bytes, alongside marked SoundFont references. Accepted bindings cannot be discarded this way, and portable bundles still require all dependencies. Inspection reports declared pending state independently of root filesystem readability; a readable root does not imply a complete playable group. The GUI patch picker requires a loaded program inventory. Successful repair does not erase historical warnings.

`GigPendingBankTest` authors nineteen cases / thirty-four scenarios using production GIG C++/MOC/UIC and actual libgig-written banks. They cover absent/marked/appeared roots and legacy missing companions, three repair routes, unavailable programs, incomplete/corrupt/missing replacements versus clear, empty settings, pending checkpoints after source appearance, malformed/accepted-bound references, independent settings replacement, corrupt root/present companion diagnostics, rate reset, notification retirement, admission failures, inspection/mixed assets and native NotePlayHandle buffer silence/recovery. Four required plugin-module scenarios cover Runtime undo after the root appears/redo/replay/rejected edit/owned split-group reopen after root and companion removal; pending reopen after a companion appears followed by same-root repair; and scripted Chat/Responses inspection/search/repair/verification. Missing plugin discovery fails rather than skips. FileResourceSnapshotTest adds three cases / eleven scenarios for typed classification, accepted member damage and cancellation/exception behavior; those structural fixtures contain no playable programs. All native fixtures are uncompiled and unrun.

Source validation reports 534 delimiter-checked C++ files / 165 native operations. Native build tools remain unavailable and seventeen submodules are uninitialized. This does not prove C++/Qt/MOC/UIC/export/link/ABI or test-host module imports, GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI regression, actual PCM, hardware, latency, CPU/RSS or real-provider autonomous musical evaluation. The GIG patch-dialog source continuation is recorded above; constructor/destructor/native callback, streaming/voice/dimension/rate and effective/opaque-state limits remain; direct faults can retain partial effects until Runtime rollback, and the engine model-change guard spans synchronous file work. Full DAW acceptance remains outstanding.

### Native GIG resource lifecycle continuation (2026-10-15, source only)

GigInstrument file/settings/program work now uses an application-thread, nonreentrant resource guard. It pins weak plugin/track/selected instrument/container/AudioEngine/Song/Mixer ownership, root generations, project filename/output rate, current GigInstance and accepted dependency-group snapshot/path, and three stored bank/patch/gain controls. Settings copy the native DOM reference and filename, validate each returned model stage and accepted ResourceReferenceScope, and restore bank/patch signal blocking only on surviving QObjects. File, patch and label notifications have post-return checks. Resource metadata now pins the accepted group; `programSnapshot()` returns an owned native inventory, and labels/patch-name lookup use owned program copies and checked effective model reads.

Group capture runs before the libgig parser error handler. Cancellation and callback exceptions can propagate through native file work rather than being converted into a libgig rejection. Context is checked after capture and before translating native parser exceptions. A final cooperative check occurs before publication. Capture and this check hold neither instance synth nor notes mutex. Libgig still constructs and validates the native bank, resolves sample associations and streams from the owned group; instance adoption and old note/instance retirement remain under native locks, followed by model/label/signal publication after unlocking. The engine model-change guard spans the operation, including file work, so this does not establish uninterrupted audio or bounded latency. The GUI file dialog checks weak view/model/button/selected-track/project identities across modal return and restores a surviving button.

`GigResourceLifecycleTest` authors fifteen cases / thirty-six scenarios using production GIG C++/MOC/PatchesDialog UIC and actual libgig-written single and split banks. Coverage includes accepted group/control roundtrips; file-work mutex probes from another thread; owner/track retirement; all three load/save fields; weak blocked-signal cleanup; label/file/patch retirement; reentry; early and during-capture cancellation/standard/nonstandard exceptions; copied inputs and changed controls/root; corrupt/missing root/companion rejection versus clear; native programs; two owned-group source-removal/NotePlayHandle buffer/restore/copy paths; and Qt event exclusion. One required actual-module Runtime scenario covers publication failure, checkpoint rollback, receipt replay, retry, rejected edit and reopen after both original root and companion removal. Missing native module discovery fails this scenario rather than skipping it. All native fixtures are uncompiled and unrun; the suite registers when gigplayer is enabled, and full default-plugin builds remain required.

This earlier lifecycle step did not add unresolved banks; the missing GIG group continuation above records subsequent pending root/companion retention, checkpoint admission and repair. The GIG patch-dialog source continuation is now recorded above; its native execution remains unverified. Constructors/destructors, arbitrary libgig callbacks, parser/streaming/decompression/voice/dimension/rate internals, derived selected-instrument state and opaque/effective/controller state are not universally guarded. Direct faults can leave partial effects until Runtime checkpoint rollback. Native calls remain synchronous and nonpreemptible; no hard deadline, latency, CPU/RSS, actual PCM, hardware or real-provider musical evaluation has been verified. Source validation reports 533 delimiter-checked C++ files / 165 native operations. Full C++/Qt/MOC/UIC/export/link/ABI and test-host module imports, GUI/legacy/project/preset/clone/plugin/DSP/MIDI regression and whole DAW acceptance remain outstanding.

### Native missing SoundFont repair continuation (2026-10-15, source only)

A missing SoundFont referenced by native settings now remains an editable instrument dependency. Sf2Instrument retains its resolved absolute filename, thirteen native controls including bank/program, and an `unresolved` flag. It unloads the old bank through the native FluidSynth path, invalidates queued voice generations, exposes no available programs or accepted-bank snapshot, and returns zero instrument output while pending. Downstream effects and other instruments keep their native behavior. Legacy absent files become recoverable warnings; existing unreadable or corrupt files remain fatal load errors. Explicitly marked pending settings remain pending even after a different file appears at that path. Malformed references and an unresolved marker conflicting with an accepted resource binding are refused.

Use the existing `instrument.resource.set` operation with resource `soundfont` to repair the bank. Native `setFileResource` and `openFile` share the same behavior: require the replacement bank to contain the stored bank/program before adoption, retain those controls and effect settings, and clear pending state after native load succeeds. A missing/corrupt replacement or unavailable saved program leaves the pending declaration intact. To intentionally choose a bank with different programs, edit the native bank/patch controls while pending, or clear the resource with an empty path before loading a new bank. Ordinary already-loaded bank replacement keeps its existing selection behavior. Settings loading uses its own mode so an unrelated preset/project can replace pending state without inheriting those repair constraints. Empty settings also clear the retained missing reference.

Native save emits `sf2player/src` plus `unresolved="1"` for pending state without registering invented bytes. DataFile accepts this exact marked reference as unbound absolute checkpoint XML with no asset entry; portable bundles still require every dependency. Accepted banks retain their existing owned snapshot behavior. Dependency inspection reports the pending declaration independently of current filesystem metadata; an appeared readable file does not repair it implicitly. The GUI patch picker stays disabled until a native program inventory exists. Successful repair does not erase historical native warnings.

`Sf2PendingBankTest` authors eighteen cases / thirty-one scenarios using production Sf2 C++/MOC/UIC, FluidSynth and real calibration SF2 bytes. They cover legacy/marked/appeared references, three repair routes, unavailable banks/programs, failed file replacement versus clear, empty settings, SF2/SF3 pending-path checkpoint metadata, malformed and conflicting references, independent settings replacement, corrupt-file diagnostics, synth rebuild, notification retirement, admission failure, inspection, mixed accepted/pending assets and one native NotePlayHandle zero-then-nonzero buffer path. Three required plugin-module scenarios cover Runtime load/undo after the original path appears/redo/receipt replay/rejected edit/reopen after source removal and scripted Chat/Responses discovery/search/repair/verification. Missing plugin discovery fails those scenarios; all native fixtures remain uncompiled and unrun. SF3 compressed-bank decoding has no new fixture evidence.

Source validation reports 532 delimiter-checked C++ files and 165 native operations. This does not verify Qt/MOC/UIC/export/link/ABI or test-host module imports, GUI/legacy/preset/project/clone/controller/automation/DSP/MIDI behavior, actual PCM, latency, CPU/RSS, hardware or real-provider autonomous music/listening. Raw constructor/destructor/native callback, effect/tuning/voice and opaque state boundaries retain the preceding lifecycle limits; native file work and FluidSynth loading remain synchronous/nonpreemptible. Full DAW acceptance remains unproven.

### Native SoundFont resource lifecycle continuation (2026-10-15, source only)

SoundFont file replacement, settings serialization, native program selection and synth rebuilding now share an application-thread resource guard. It pins weak plugin/track/selected instrument/container/engine/Song/Mixer identities, project and routing generations, project filename and output rate, accepted bank snapshot, native synth/settings/font identities and thirteen manual controls. Resource/settings transitions refuse reentry and suspend cooperative Qt event delivery. Expected nested patch observation remains available. Returned model, label, file and patch notifications are checked before continuing; bank/patch signal suppression restores only surviving QObjects.

Bank capture and retained-bank verification run before taking either instance load/synth mutex. FluidSynth still loads, enumerates, selects and adopts native banks under its instance mutexes. Bank/patch model publication and labels occur after releasing those mutexes. Rebuilding verifies retained bytes first, keeps independent candidate synth/settings and native voice generations, unlocks before reporting errors, and checks returned effect/tuning updates. Resource inventory now pins the accepted bank snapshot. The file dialog checks weak view/model/button/selected-track/project identities across modal return and restores a surviving button through scoped cleanup.

`Sf2ResourceLifecycleTest` authors fifteen cases / fifty-one scenarios using production Sf2 source, its MOC and PatchesDialog UI, FluidSynth and the real calibration SoundFont. Coverage includes two file-work mutex probes from another thread, four file-work retirement paths, twenty-six load/save field retirement rows, weak blocked-signal cleanup, label/file/patch retirement, nested work refusal, cancellation/exception admission reuse, copied inputs and changed context, native program behavior, retained source removal and Qt event exclusion. One required actual-module Runtime scenario covers publication failure, checkpoint rollback, durable replay, retry and reopen after source removal; absent plugin discovery fails that scenario rather than skipping it. The suite is registered when the native sf2player target exists; full default plugin builds remain required.

These fixtures are uncompiled and unrun. This earlier lifecycle step did not implement pending banks; the missing SoundFont continuation above records the subsequent source implementation of retention, checkpoint admission and repair. Existing raw constructor/destructor, FluidSynth/custom callback, render/voice, effective model, tuning and effect-slot internals are not universally protected. Direct failure can retain partial native/model/signal effects until Runtime checkpoint rollback. Native loading/iteration remains serialized and nonpreemptible; no hard deadline, latency, CPU/RSS or acoustic evidence is established. Full C++/Qt/MOC/UIC/export/link/ABI and GUI/legacy/preset/project/clone/plugin/DSP/MIDI regression remains required, including test-host module binding and Windows executable imports. Source validation reports 531 delimiter-checked C++ files and 165 native operations; no native build, PCM, hardware or real-provider production execution is proven. Whole DAW acceptance remains outstanding.

### Native resource-name observation continuation (2026-10-15, source only)

Dependency inventory no longer calls virtual Track/Controller name getters while walking raw native rosters or while validating a partially captured roster. It first captures weak roots, track/clip/chain/effect/controller/shaping actors and native resource state. Initial stored names use qualified Track::name and Controller::name access, so callbacks cannot run inside that capture. Virtual reported names are then read through a separate returned-stage boundary: validate all captured native actors, call the getter while binding its QString reference, validate again, and only then copy the returned text. No cooperative work is dispatched between the returned reference and that validation/copy.

This fixes two concrete raw continuations: a track name getter could retire its track before the constructor read further track fields, and an LFO name getter could remove a rack entry before the constructor read its ID/wave state or continued the controller vector. Validation itself previously called those getters before continuing raw actor reads. The structural validator now compares qualified native base names without virtual reentry. Virtual reported names remain the displayed owner names, separately captured and compared again after final plugin getters. Native name overrides are preserved rather than silently replaced by base storage.

Reported track names apply to instrument, track-effect and nonempty shaping-wave rows. Reported LFO names apply to controller-wave rows. Sample-clip names remain clip names and mixer-effect owners retain native mixer-channel names. Initial name reads poll cooperative work between owners; the final reported-name pass performs returned-context checks without another work poll, followed by full structural validation. Existing resourceGeneration, text previews, item/page limits, metadata-only file probing and read-only receipt/history behavior remain unchanged. A reported name change or observed native owner/base/resource change refuses the observation instead of publishing its partial result.

StudioResourceNameTest authors ten cases/forty-three scenarios without module skips: four naming scopes including sample-clip name preservation; initial/final track and LFO self retirement with independently retained return text; sixteen clip/controller/effect/Mixer peer retirements across both kinds of name owner and both phases; four native backing renames; two reported-only name changes; two plugin-getter changes to reported names; eight standard/nonstandard getter exceptions with read admission reuse; cancellation before a second initial getter; large owner-name preview and stable paging-token checks; and actual native Runtime LFO rename, historical receipt replay, history neutrality and reopening. The fixture instrument provides resource metadata only and generates no audio. Fixture callbacks are cleared before native teardown. All native cases are uncompiled/unrun.

Source diagnostics pass across 530 changed/new C++ files and 165 operations. Ten supplemental tools plus universal routing retain 128 declarations. This is returned-stage lifetime and observed-value validation, not a transactional or immutable project snapshot. Getter code that already executes invalid operations, a dangling reference unrelated to a surviving owner, concurrent external mutation, global teardown and ownership stealing remain outside guarantees. Late name getters can mutate opaque declarations inspected by earlier plugin getters, and later getters can change earlier dynamically reported names without another read; no universal quiescence is claimed for adversarial stateful getters. Direct callback effects persist after a refused read and receive no automatic rollback. Full roster capture still precedes virtual work, validation/name lookup can be quadratic, and model/DSP ownership, copies/huge names, callbacks, native getters/Qt/JSON/filesystem/hash work have no hard deadline or measured CPU/RSS/latency evidence. Full C++/Qt/MOC/link, GUI/legacy/plugin/project/controller/sample/resource/DSP/MIDI compatibility, actual PCM/hardware, real-provider autonomous production and whole DAW acceptance remain unverified.

### Native sound-shaping envelope/LFO wave continuation (2026-10-15, source only)

The volume, cutoff and resonance EnvelopeAndLfoParameters groups on native InstrumentSoundShaping now own checked user-wave loading and retained missing-reference state. Their native elvol/elcut/elres settings preserve userwavefile plus userwaveunresolved=1, independently from empty sample buffers. Legacy absent sources become recoverable missing-reference diagnostics. Explicit pending state skips file probing/decoding after a file appears at that name; corrupt existing content remains fatal and malformed paths or accepted-binding conflicts are refused. Empty ordinary settings now clear an older waveform instead of retaining stale content. The native legacy reversed sus sustain conversion remains intact.

Project resource inventory captures all three groups for Song and PatternStore instrument tracks before any plugin resource getter. Only nonempty shaping paths produce dependency rows, with owner kind instrumentShapingWave, current container/track/target, waveform metadata, selection handle and instrument.shaping.waveform.set repair hint. Empty groups are still captured and checked so a getter cannot silently introduce an unreported reference. Inventory validates weak shaping/parameter actors, native backing/path/pending/selector state and existing track/root context. This covers the three native instrument shaping groups; generic standalone/custom envelopes and opaque plugin references are not implicitly discovered.

Use the observed owner coordinates with the new native operation:

```json
{"op":"instrument.shaping.waveform.set","args":{"container":"song","track":0,"target":"cutoff","path":"/absolute/replacement.wav"}}
```

Target is volume, cutoff or resonance. The row's userwave resource key is metadata, not an argument to this operation. Existing asset.search can locate filename candidates. Checked setLfoUserWave repair retains fourteen manual controls and the current wave selection; select:true explicitly selects the user waveform. Empty path clears its source and pending marker. Missing/corrupt replacement failure preserves the previous resource. Successful changes invalidate cached native LFO shape data and validate dirty publication. Zero user-wave contribution does not mean silent audio: envelope amounts, base levels and other native modulation remain active. A matching filename does not prove equivalent waveform content or musical improvement.

The native DataFile asset table now includes all three persisted envelope tags. Accepted samples are registered after the sound-shaping parent renames the generic el node to elvol/elcut/elres; binding before that rename would have been ignored by the resource table. The parent pins the saved resource and checks returned serializer hooks before binding it, rejecting source substitution. Original accepted bytes remain available for native restore and checkpoint copying after the source is removed. Pending references remain absolute XML paths without invented asset fingerprints; ordinary copied assets still require exact manifest equality. Bundles continue to refuse unresolved dependencies, including when different bytes appear at the retained name. The generic standalone el tag is not added as a new asset-table scope.

A native resource guard rejects reentry/foreign-thread execution and observes weak parameter/full QObject ancestry/optional instrument-track/container/engine/Song/Mixer actors, replacement generations, project filename and retained backing/pending state. It owns path/buffer values, copies new adoption values before releasing old pins, checks fourteen manual values during save/decode and validates after returned model calls. Loading retains ordinary native control notifications and legacy conversions. The outer sound-shaping saver now checks its returned lifetime between filters and envelope groups. GUI sample/clip drops call the checked core loader with weak view/model validation, and user-wave notification reads the current weak ModelView model; direct GUI buffer assignment is removed.

EnvelopeWaveResourceTest authors twenty-one cases/seventy-eight scenarios without module-conditional skips: nine target/legacy/marked/later-file roundtrips; six direct/structured repairs retaining inactive selection; selection/failure/clear behavior; three native asset-tag checkpoint/bundle/manifest paths; accepted snapshot/same-retained/sample-backed conflicts; malformed settings/protocol inputs; twenty-eight returned model load/save retirement rows; parent serialization retirement; nested/cancel/standard/nonstandard cleanup; copied input/control/root changes; cached native fillLevel contribution invalidation/repair; owned restore, empty clear and legacy sustain; parent tag binding after source removal and serializer-hook substitution refusal; sparse Song/PatternStore inventory and getter-introduced references; corrupt-source fatality and Qt event exclusion; Runtime repair/undo/redo/failure rollback/replay/reopen after source removal; and two scripted Chat/Responses dependency/search/catalog-routed repair sequences. The observer instrument in one fixture only provokes a native resource-getter callback; it supplies no sound generator or production substitute. All native cases are uncompiled/unrun. Native modulation arrays are not rendered PCM, listening or real-provider production evidence.

Source diagnostics pass across 529 changed/new C++ files and 165 operations. Ten supplemental agent tools plus universal catalog routing retain 128 declarations. Layout/accessor/helper/resource-table and GUI changes require full application/plugin C++/Qt/MOC/link/ABI and GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI/bundle/checkpoint regression. Older versions ignore pending markers; cross-version behavior and other instrument/bank/binary/opaque dependency recovery remain unverified. Direct exceptions, callbacks and hooks can retain partial state until Runtime rollback; XML, tempo-sync/automation/controller/opaque state is not deeply frozen. Returned-context checks do not protect arbitrary invalid execution inside constructors/model slots/callees/serializers/destructors/global teardown or unsupported concurrent/ownership-stealing Qt mutation. Native parameter mutex/registry/DSP assumptions remain; stage allocation/recalculation, file work, paths/copies/serialization/JSON and repeated scans join DSP or allocate without hard deadline or measured CPU/RSS/latency evidence. Native build/tests, actual PCM/hardware, real-provider complete musical workflows and full DAW acceptance remain unproven.

### Native TripleOscillator pending waves continuation (2026-10-15, source only)

TripleOscillator now retains a missing source independently for each of its three native oscillator objects. The settings keep userwavefile0/1/2 and matching userwaveunresolved0/1/2 markers, while the affected native sample buffer is empty and its antialias table is null. Ordinary absent project sources become typed recoverable diagnostics. Explicit pending state skips file probing/decoding even if different bytes later appear at the retained name. Other oscillator resources, manual controls, wave selection, modulation modes and antialias settings stay editable. Existing corrupt wave files remain fatal; malformed pending filenames and conflicts with accepted resource bindings are refused.

Project resource inventory uses the existing instrument FileResource API to report exact keys oscillator1/2/3, retained paths, pending flags, backing identities, selectors and instrument.resource.set repair hints. Resource identity pins native backing during inspection; even an empty buffer can have a non-null identity, which does not prove decoded or audible content. Select the observed current track/container and exact resource key after any project replacement. Native asset.search can find candidates, but a matching filename is not equivalent waveform content.

```json
{"op":"instrument.resource.set","args":{"track":0,"resource":"oscillator2","path":"/absolute/replacement.wav"}}
```

Successful repair uses the existing native sample decoder and FFT antialias-table generator, then publishes the new sample/table pair and clears that slot's marker. It retains ten manual control values and other slots' backing; selector changes still require explicit select:true. Empty path clears a resource without changing its waveform selection. A missing/corrupt replacement returns failure and retains its previous reference/backing. Existing active voices can retain prior sample/table pins until their native voice lifetime ends: these operations are not an immediate replacement of every sounding voice. Pending user-wave content contributes zero for new voices using that resource; other oscillators/modulation paths can still produce sound.

DataFile checkpoints now admit explicit unbound pending paths only for the three native TripleOscillator resource attributes and their matching per-slot markers. Pending paths are absolute XML references without invented asset hashes. Accepted sources still require exact resource correspondence and stored-asset manifest equality. Portable bundles refuse pending dependencies even if another file has appeared at the retained name. Mixed pending and accepted resources can coexist, and accepted waveform bytes remain owned for clone/history/restart after original-source removal. A pending marker cannot discard original-name, same-retained-path or sample-backed accepted bindings.

An instrument-wide native ResourceGuard rejects settings/file-work reentry and foreign-thread execution. It captures weak instrument/track/current selection/container/engine/Song/Mixer actors, the complete three-oscillator roster, project/Mixer replacement generations, project filename and retained buffer/table/pending values. Save and file work check all thirty manual values; load checks each returned model call and freezes manual values during each slot's decoding phase. Serialization owns buffer/path values, and replacement copies new backing/table/path before releasing old pins. The same copy-before-release ordering is corrected in the LFO controller resource context. Modal GUI wave selection checks weak oscillator/instrument/track/current selection/project generation, and initial view tooltips use retained filenames.

TripleOscillatorResourceTest authors seventeen cases/sixty-three scenarios using production plugin C++/MOC source: nine per-slot legacy/marked/later-file settings and control-roundtrip rows, six direct/structured active/inactive repairs, failed/clear/accepted-state behavior, mixed checkpoint/bundle/manifest rules, malformed paths and accepted-binding forms, twenty model load/save retirement rows, native actor/project-path changes, nested settings/resource refusal, cancellation/standard/nonstandard cleanup, copied inputs/other-slot control edits, owned serialization after original removal, resource inventory, corrupt-source fatality and Qt-event exclusion. Two fresh native NotePlayHandle buffer scenarios exercise direct-wave and antialias paths before pending state and after repair, checking zero old content only for fresh voices. Three scenarios require the actual built module to be discoverable: Runtime repair/undo/redo/failure rollback/replay/reopen after source removal and two scripted Chat/Responses dependency/search/catalog-routed repair sequences. Module discovery failures fail these rows instead of skipping them. The suite is registered when the normal tripleoscillator target is enabled; this does not permit a reduced build as full acceptance evidence.

All native fixtures are uncompiled/unrun. Source diagnostics pass across 526 changed/new C++ files and 164 operations, with ten supplemental tools and universal routing within 128 declarations. TripleOscillator/OscillatorObject layout and helper changes need full application/plugin C++/Qt/MOC/export/link/ABI and GUI/legacy/project/preset/clone/controller/automation/DSP/MIDI/bundle/checkpoint regression. Older LMMS ignores pending markers, so cross-version behavior remains unverified. Direct exceptions and callback edits can retain partial state until Runtime rollback; XML is not deeply frozen, and model-load control notifications between slots retain native semantics. Returned-context checks do not protect arbitrary invalid execution inside constructors, model slots, serializers, callees, destructors or global teardown, nor unsupported ownership stealing or concurrent raw Qt mutation. The native FFT generator retains its shared plans/scratch and has cooperative polls only outside generation; file/DSP joins, paths, copies, allocations and native work have no hard deadline or measured CPU/RSS/latency evidence. Other instruments/waves, banks/plugin binaries, parameter-owned LFO and opaque assets retain separate recovery work. No actual PCM/render/hardware, real-provider musical production or complete DAW acceptance is established.

The fixture exports test-host symbols for native module loading. Platform module imports and test-host binding still require native link/execution checks, particularly Windows plugins built against the normal LMMS executable. Source registration and dependency paths do not establish module-host compatibility.

### Native pending LFO user-wave repair continuation (2026-10-15, source only)

LfoController now retains an unresolved user-wave filename independently from its empty playback buffer. Loading an ordinary absent waveform keeps the native controller editable and emits a typed recoverable missing-reference diagnostic. Saving writes the original resolved path in userwavefile and userwaveunresolved=1. Explicit marked state remains pending even if a file later appears; it skips file probing/decoding. This keeps history/restart from silently substituting new bytes. Corrupt existing wave files still produce fatal native diagnostics, while invalid empty/null/unresolvable pending paths are refused before controls change. A pending marker cannot discard a captured accepted resource binding, including snapshot-only, sample-backed and same-retained-path bindings.

A selected missing user wave contributes zero modulation through the existing empty-buffer oscillator behavior. The LFO base value still applies; this is not a silent-audio guarantee. Native base/speed/amount/phase/wave/multiplier controls and controller connections remain the editable authority. An inactive pending wave also keeps its filename; another selected native waveform continues its existing modulation behavior.

Controller inspection and project.inspect section resources expose the retained path and unresolved flag independently from file presence, plus the current controller coordinate and controller.waveform.set repair hint. Search can locate candidates with asset.search. Use the observed current index rather than a handle retained across project replacement:

```json
{"op":"controller.waveform.set","args":{"controller":0,"path":"/absolute/replacement.wav"}}
```

The checked native loader repairs or clears the reference. Successful repair clears the pending marker and retains existing control values, connections and wave selection. Set select:true only when user-wave selection should also change. Empty path clears the source and marker without changing selection. A failed missing/corrupt replacement preserves the previous pending reference or accepted backing. Matching names do not establish equivalent modulation or a musical improvement. The model can call this operation through studio_execute when it has no individual function declaration.

Native checkpoints now admit explicit unbound pending LFO userwavefile references as absolute paths covered by the project XML digest, without inventing an asset fingerprint or claiming stored audio. Existing stored-asset reference equality and accepted-source correspondence remain required; portable bundles refuse unresolved dependencies even if another file appears at the retained name. Repaired waves retain accepted source bytes through normal checkpoint ownership and can reopen after the original source is removed. Runtime history may restore pending or repaired state; prior native handles must be rediscovered after replacement. Diagnostics remain cumulative under the existing native policy until cleared/reloaded.

Resource work checks application-thread ownership, weak controller/parent/engine/Song/Mixer identities, project/Mixer generations, filename and retained backing/pending state around returned cooperative work. Serialization keeps owned buffer/path values and checks six manual control values; decoding copies input paths and checks exposed controls around adoption/notification. Structured repair checks backing/path/pending and native root/path after selection and project dirty publication, then rebuilds resource metadata. GUI waveform selection checks weak dialog/model/button/project generation around the modal boundary and displays the retained path. This source work does not make arbitrary native callback, constructor, callee, serializer or destructor execution safe. Direct exceptions can leave partial state or callback effects; Runtime whole-project rollback is the recovery boundary.

LfoWaveResourceTest authors nineteen cases/forty scenarios without module-conditional skips: legacy/marked/later-present waves, zero modulation contribution and control roundtrip, direct/structured active/inactive repair, retained connection, failure/clear/malformed settings, accepted-binding conflicts, pending checkpoint/bundle/manifest behavior, inspection/file-status separation, dirty feedback, copied inputs/control changes, project changes, serialization mutation/owned wave after source removal, ordinary empty state, corrupt-wave fatality and failed Runtime project-load rollback/replay, queued-event exclusion, repair/undo/redo/replay/reopen preserving a controller binding after original removal, and two scripted Chat/Responses dependency-to-search-to-catalog-repair sequences. These fixtures inspect native state and controller buffers; they are uncompiled/unrun and provide neither rendered PCM nor real-provider musical-production evidence.

Source diagnostics pass across 525 changed/new C++ files and 164 native operations. Ten supplemental tools plus universal routing retain the 128-declaration budget. LfoController layout/helper changes require a complete application/plugin C++/Qt/MOC/link/ABI rebuild and controller/automation/legacy/preset/GUI/DSP/MIDI/bundle/checkpoint regression. Older LMMS versions ignore the new pending marker; cross-version behavior remains unverified. Other instrument waves, missing banks, plugin binaries, parameter-owned LFO resources and opaque assets retain separate recovery gaps. Native model changes join DSP during file work; buffers, path/stat/decoder/JSON/serialization/callback/allocation work has no hard deadline or measured CPU/RSS/latency evidence. Native execution, hardware, real-provider complete musical workflows and full DAW acceptance remain unproven.

### Native asset roots and filename lookup continuation (2026-10-15, source only)

`asset.roots` exposes fourteen actual native PathUtil directory aliases for samples, presets, projects, soundfonts, GIG, VST and LADSPA files, with resolved/canonical paths and metadata status. It creates no directories. `local` is unresolved without a project filename and otherwise refers to its directory, which may be an owned session checkpoint directory. Aliases follow existing PathUtil mapping rather than assuming every configured external plugin search path. `names:false` provides bounded previews when full roots exceed the 512 KiB receipt cap.

```json
{"op":"asset.roots","args":{"names":false}}
{"op":"asset.search","args":{"root":"usersample","query":"bass warm","extensions":["wav"],"recursive":true,"maximumEntries":100000,"limit":128}}
```

`asset.search` requires one discovered alias, absolute directory or known prefixed directory such as `usersample:drums`. Unknown bare relative names are refused. Empty query browses candidates. Native matching joins case-insensitive basename tokens with AND; double quotes keep phrases together. Literal suffixes accept `wav`, `.wav`, `*.wav`, compound suffixes or `*`. Both last and complete suffixes match, so `repair.take.wav` now matches WAV filters in studio and the GUI browser. Directories ignore suffix filters. `kind` is files (default), directories or all. Recursive search defaults true; hidden entries and following descendant directory symlinks default false in studio. A symlinked root or yielded file can still resolve outside the chosen directory; this is not a filesystem sandbox.

Matches contain an observed index, path/name/relative path, kind, readability, symlink flag, modification time and size metadata. Decimal bytesText retains exact sizes; numeric bytes is null beyond exact JSON integer range. No file content is opened, decoded or byte-hashed. Query length is at most 4096 Unicode code points; extension lists contain 1..64 strings of at most 64 code points. maximumEntries bounds yielded native iterator entries to 1..1000000 (default100000), including directories and nonmatches. Pages use offset and 1..1024 rows (default128), with a 512 KiB item-array budget. names:false previews long paths/names at 256 UTF-16 units without splitting surrogate pairs; short strings remain exact. Additional query/root/receipt fields fall outside the item-array budget, which is not a whole-receipt memory bound.

Continue using nextOffset, unchanged root/query/filters/entry budget and searchGeneration. The process-local token fingerprints initial root metadata, settings and all observed ordered entry metadata, independent of page/text mode/completion timestamp. Each page rescans the scoped bounded traversal in native iterator order. Changed order/metadata refuses the token and requires restarting without it. Identical metadata can hide different bytes: this is not an atomic snapshot, persistent cursor or content digest. total and hasMore describe observed matches. Check scanComplete, entryLimitReached, unavailableRoots and unreadableDirectories independently: hasMore:false can accompany an incomplete capped scan. Narrow the directory or choose another budget and restart. Readability reporting is best effort, not a comprehensive permissions audit.

Discover missing references with project.inspect section resources, select a root, search, then pass the candidate path and current native owner coordinates to its supported checked repair operation. A vanished or unsuitable candidate may fail; rediscover native state and search again before retrying. Existing native loading, settings preservation, history and owned checkpoint retention remain authoritative. Matching filenames and historical replayed receipts do not establish equivalent audio. Lookup does not create project revisions.

QtCore FileSearch now serves both studio and GUI FileSearchJob. The GUI retains its existing worker/future lifecycle and unlimited recursive/follow-symlink defaults. Studio captures weak native project/engine identities and actual base definitions under brief native model guards, traverses outside model changes with Qt event pumping suspended and checks project generation/filename/base changes before returning. Cancellation polls between entries and hashing chunks. Path capture, tokenization, filesystem iteration/stat, JSON and allocations have no hard deadline or measured CPU/RSS/latency guarantee. Arbitrary callback edits are not rolled back by a failed read.

FileSearchTest authors ten cases/23 scenarios, including the actual browser worker without Engine/window. StudioAssetsTest authors sixteen cases/43 scenarios for native aliases, matching/kinds, bounds/paging/metadata changes, incomplete scans, project/base changes, cancellation/exceptions/queued-event exclusion, no mkdir, root previews and receipt/history neutrality. Four scripted Chat/Responses routes use actual dependency/root/search outputs to repair missing samples, including a disappearing candidate, rediscovery/retry, undo/redo and checkpoint reopening after source removal. These are mechanical regression fixtures, not real-provider musical-production evidence. All native fixtures remain uncompiled/unrun.

Source diagnostics pass across 524 changed/new C++ files and 164 native operations. Ten supplemental tools and universal catalog routing retain 128 declarations. FileSearchJob Task alias/layout, GUI worker/future/cancellation and full C++/Qt/MOC/export/link/ABI/application/plugin rebuild need native verification. GUI/legacy/plugin/DSP compatibility, actual PCM/hardware, real-provider interpretation/composition/listening/revision and complete DAW acceptance remain unproven.

### Native project dependency inventory continuation (2026-10-15, source only)

`project.inspect` now accepts `section:"resources"` for a project-wide declared dependency inventory. It covers native instrument/effect `FileResource` entries in Song/PatternStore tracks and Mixer effect chains, sample clip sources, rack LFO waves and unavailable effect placeholders. Each row gives native owner coordinates, resource key/name/path, required-for-restore and unresolved flags, stored selector state where applicable, a native repair operation hint or null, and filesystem metadata. Sample/wave rows include retained source and source-frame/rate information. `nativeBackingPinned` means an in-process backing token is retained during inspection; it does not prove decoded or audible content. `fileObservations`, `declaredUnresolvedCount` and `requiredForRestoreCount` describe the entire scan rather than only its returned page.

Start with a bounded page:

```json
{"op":"project.inspect","args":{"section":"resources","limit":128,"names":false}}
```

While `hasMore` is true, pass the returned `nextOffset` and unchanged `resourceGeneration`. The token is independent of offset, limit and text mode. A changed observation requires restarting without the token. Short text stays exact with `names:false`; long text has a preview, UTF-16 length and truncation flag. Names/paths/keys preview at 256 code units without splitting a surrogate pair; file types expose up to sixteen 64-unit previews. Pages allow 1..1024 rows and stop before the 512 KiB item-array budget. A single oversized full-text row requires preview mode. Use the row's native owner selectors and exact key with its repair operation; metadata such as `nativeId`, `kind` or `scope` is not an operation argument bundle. Effect resources without a checked repair operation have null hints.

Filesystem status is separate from native state. A readable file can still belong to an intentionally unresolved silent AFP/sample clip; an absent original file can coexist with retained native content. Empty references are reported explicitly. File presence/readability/size and retained backing flags do not establish valid decoding, byte identity, complete companion sets, portability, plugin compatibility or audible output. The scan performs no plugin refresh/serialization, asset-byte hashing, decoding or plugin loading. Its SHA-256 token fingerprints observed declared metadata, full text/types/resolved paths, stored selectors, filesystem status/size, native owner handles, project/Mixer replacement generations and filename under a process-local scope. UTF-16 hashing uses 8192-byte views. Raw backing pointers are never serialized or hashed; within-request backing changes are compared through retained identities. Same-size byte changes and opaque/same-declaration changes between requests may leave the token unchanged. `completedAt` is outside the token and does not imply an atomic filesystem snapshot.

The scan captures native rosters/weak actors and sample/wave backing before invoking plugin getters. Returned stages and final weak roster, selector, sample/wave, root/generation/filename checks precede publication. The final getter pass has no intervening cooperative work. Compact project summaries advertise the section without expanding or scanning all dependencies. Native agent guidance explains paging and the distinction between filesystem observations, intentional unresolved state and retained content. The project section enum also now includes `warnings`; previously the warning handler existed but its schema excluded that value. Source assertions now compare the declared section set with the supported handler routes.

`StudioProjectResourceTest` adds 19 authored cases / 48 scenarios for eight owner scopes, pending/later-appearing sources, native read-only counters/state, count/token paging, changed observations, same-size byte non-verification, oversized text/types, retirement of native owners/getters/selectors, metadata/backing changes, malformed pages, event suppression, cancellation and standard/nonstandard failures, Runtime historical receipts/history/reopen, and both scripted Chat/Responses resource discovery/paging. There are no conditional skips in the new suite. Every native fixture remains uncompiled/unrun. Source checks cover 516 changed/new C++ files and 162 operations only; scripted routes do not establish real-provider or autonomous production behavior.

Coverage remains declared resources, not complete dependency closure. Opaque/undeclared plugin binaries/assets, companion-member completeness and parameter-owned/unregistered controller waves remain excluded except explicit unavailable-effect placeholders. Required flags follow native declarations and selectors use stored/manual values. Complete weak roster/metadata/backing capture stays resident, every page scans again, and initial capture/native getters/filesystem calls/allocation have no hard deadline; output limits do not bound all native input, full-string conversion or total metadata memory. CPU/RSS/latency remains unmeasured. Arbitrary destruction inside executing native constructors/callees/model mutexes/Qt destructors, dangling selector returns, foreign/raw ownership bypass, mutating getters and opaque side effects remain outside universal guarantees; failed reads do not roll back arbitrary callback edits. New core code and the shared LFO accessor require a native rebuild and C++/Qt/MOC/link/ABI/platform/legacy/plugin/GUI/backend/DSP regressions. Rendered PCM, hardware, real-provider evaluation/production and complete DAW acceptance remain outstanding.

### Native missing sample-clip repair continuation (2026-10-15, source only)

Native SampleClip now retains a missing source filename, optional source frame window and original sample rate alongside its empty playback buffer. Project/clipboard settings preserve position, length, offset, name, controls, auto-resize policy, gain and reversal. Explicit `unresolved="1"` state stays silent even when a file later appears at that location; legacy missing sources become retained recoverable state. A usable embedded backup still restores content when an unmarked external source is absent. Malformed source bounds/rates and invalid embedded audio remain native failures. Unknown source windows/rates stay unknown rather than acquiring invented frame counts; sample inspection exposes the pending path/flag, nullable bounds and zero unknown rate.

`sample.load` uses checked native decoding and now accepts `preserveSettings` and an empty path. Preservation defaults to true for pending clips and false otherwise. Repair retains arrangement geometry, gain, reversal and known source bounds; a shorter replacement is refused before adoption. An unknown window uses the decoded extent. Explicit replacement resets source playback state/offset according to native clip resize policy. Empty path clears the source and offset while retaining the clip; a manually sized clip retains its length. Successful repair and ordinary native buffer publication clear pending state. Rate-changing repair preserves raw frame coordinates and arrangement geometry; it does not claim time-stretch or musical equivalence.

Resource work uses owner-thread/nonreentrant admission, native changes/events guards, weak clip/track/container/project anchors and retained buffer identity. Returned decoding, reference verification, native model/geometry calls and notifications revalidate context. Serialization owns its Sample/buffer and checks exposed controls, geometry and pending metadata around native serialization/encoding. Pending length updates preserve serialized length and cannot create playback handles. Direct copy/clone constructors retain pending value state. Serialized selections and track settings restoration now compare a dedicated fatal diagnostic generation, allowing typed retained-reference warnings while detecting new ordinary native errors; selection serialization still compares the complete diagnostic generation. The fatal generation survives diagnostic clearing and remains process-local.

Checkpoint resource matching now permits explicit unbound pending AFP and sample-clip references, covered by the native XML digest. It still requires exact stored-file manifest correspondence and refuses conflicting embedded payloads or accepted source bindings. Portable resource bundles continue to reject missing dependencies. Structured sample loading pins native actors, project/Mixer generations and project filename and validates exposed track/sample/buffer/placement state around dirty publication before returning final inspection. The GUI file-dialog path uses weak view/clip/project identities and checked repair, and existing tooltip/name presentation retains the missing filename.

`SampleClipResourceTest` adds 19 authored cases / 37 scenarios without module-conditional skips: missing/marked/later sources, native/structured repair with both resize policies, short replacement refusal and explicit replacement, project/checkpoint/bundle behavior, direct/cross-track/serialized-selection/whole-track copies, accepted binding refusal, malformed metadata, returned decoder retirement, dirty-time mutation, borrowed paths, retained embedded encoding, reentry, unknown bounds, native buffer publication, fatal generations, ordinary file/embedded round trips, legacy embedded backup, structured clear, and Runtime load/repair/undo/redo/corrupt-load rollback/replay/reopen with original-source removal. All native cases remain uncompiled/unrun. Source checks pass for 513 changed/new C++ files and 162 operations only.

Full application/plugin rebuild is required for SampleClip/Song layout and behavior changes. Other instrument/controller files and binary/plugin/effect dependencies remain separate incomplete work. Older LMMS builds ignore the pending marker. No automatic search/substitution or fingerprint for missing bytes is claimed. Later tempo/resize actions can apply the retained native auto-resize policy again, and pending tempo offsets still follow native frame-offset conversion. Pending source-window edits/reversal require decoded content; mutable native Sample access can bypass policy. Arbitrary destruction inside executing native constructors/callees/control helpers/model mutexes/Qt destructors, foreign Qt mutation, global teardown and recording/waveform lifecycle remain outside universal guarantees. Direct failure can leave partial state or dirty/error/journal/file effects; Runtime rollback is mandatory and unproven. Snapshot/traversal/storage CPU/RSS/latency, audio-thread/automation costs and cooperative-budget effectiveness are unmeasured. Native C++/Qt/MOC/link/ABI, GUI/legacy/journal/clone/bundle/automation/recording/backend/DSP regressions, rendered PCM, hardware, real-provider production and full DAW acceptance remain outstanding.

### Native unresolved AFP checkpoint and relink continuation (2026-10-15, source only)

AFP now serializes intentional silent sample references with `unresolved="1"` alongside `src`. Loading that state restores an empty native buffer and retains the filename even if a different file has appeared there. A checked `instrument.resource.set` load or clear removes the marker; merely creating a file at the old path does not change preserved composition state. Legacy missing `src` in an empty AFP becomes this editable state. Empty filenames and conflicting embedded content are refused. Resource inspection exposes `unresolved`, and resource publication compares that flag across dirty notification. The flag describes native pending state, not a universal filesystem availability test for every plugin.

Native checkpoints preserve unbound pending AFP references as absolute XML paths. Their checkpoint digest covers the marker and reference; their `assets` array contains fingerprints only for files actually copied. Manifest matching permits only the explicit AFP/src form and still requires exact correspondence for all stored resource paths. An ordinary missing reference, unsupported resource marker, empty/relative pending checkpoint path or conflicting embedded payload does not satisfy that contract. A pending marker cannot discard an accepted source binding during copying or native settings cloning, including when the declared path already names the retained file. AFP rechecks its owner after accepted-resource verification returns. Portable `withResources` bundles continue to refuse unresolved dependencies; ordinary project files can preserve editable incomplete state.

Song diagnostics retain the existing messages, repetition counts, generation and paging. `collectUnresolvedResource` records a recoverable retained-sample diagnostic; ordinary `collectError` always latches fatal state until `clearErrors`, even if its text matches a recoverable message. Strict project/preset/checkpoint restoration now tests fatal state, allowing missing AFP references to survive repair, undo, redo and restart without ignoring decode failures, unavailable controllers/plugins or other native errors. GUI error reporting still shows retained-reference diagnostics. Diagnostic messages remain cumulative until clearing/loading; repair does not erase an earlier warning.

Nine added cases / nineteen scenarios cover recoverable/fatal classification, absent and later-appearing files, malformed pending settings, mixed pending/copied checkpoint resources, bundle refusal, manifest inconsistencies, accepted copying/cloning bindings, dirty-time pending changes, and actual native-module legacy project load, checkpoint, repair, undo/redo, corrupt embedded-load rollback, replay/reopen and reopening repaired content after its original file is removed. `AudioFileProcessorLifecycleTest` now has 27 authored cases / 44 scenarios, including three module-discovery conditional scenarios. All native cases remain uncompiled/unrun. Source checks cover 512 changed/new C++ files and 162 operations; they do not establish native compilation or successful checkpoint/recovery behavior.

Only AFP pending samples are implemented here. Other missing instrument/controller/sample-clip files and plugin/effect binaries still require their own preservation and repair work. Older LMMS builds do not honor the new silent marker, so its cross-version behavior needs compatibility evidence. No missing bytes or nonexistent asset digest is claimed, and no automatic search/substitution occurs. Filenames remain in native projects, inspection and history. Song/FileResource layout and behavioral changes require a complete application/plugin rebuild. Native C++/Qt/MOC/link/ABI, legacy/preset/clone/GUI/automation/bundle/checkpoint/journal/DSP/backend regressions, CPU/RSS/latency, actual PCM, hardware, real-provider autonomous production and full DAW acceptance remain unverified.

### Native AudioFileProcessor lifecycle continuation (2026-10-15, source only)

AudioFileProcessor settings and checked file changes now use owner-thread, nonreentrant native change/event guards. They copy borrowed filenames before cooperative work, retain weak instrument/track/selection/container/project anchors and the expected native SampleBuffer, and check those anchors after returned decoding, renaming, model changes and notifications. Guard cleanup uses the surviving weak instrument. Failed replacement preserves an existing decoded resource; invalid embedded input preserves an unresolved reference. Loading may still leave partial native state on failure, so Runtime checkpoint recovery remains required.

Saving owns a native Sample and buffer, the unresolved filename and snapshots of all eight linked control states. It checks sample scalar state and those controls around native serialization and embedded encoding, preserving the original document context for linked controls. Model slots use lighter weak context checks after returned getters, setters and signals, retaining direct/automation dispatch without adding settings admission or cooperative polling. Nonfinite amplification/points and frame overflow are refused; point multiplication uses double precision before native integer conversion. These checks do not audit `playNote`, `beatLen`, concurrent automation or DSP lifetime behavior, and their allocation/refcount/validation costs remain unmeasured.

`Plugin::FileResource` gains an optional shared backing identity, supplied by AFP's retained native buffer. This is an in-process pin, never a JSON or serialized resource identifier. Resource inspection checks the plugin after its getter returns and weak selectors after value reads. Structured `instrument.resource.set` pins the selected actors and project/Mixer generations before work, checks returned loading and selector activation, and compares exposed native track state plus resource metadata, selector and backing identities across dirty notification. It rebuilds the returned inventory before success. Plugins without backing tokens still expose only their declared state; opaque changes remain outside these comparisons. The GUI file-dialog path checks weak view/model/track/project/waveform identities after returned dialog, load and dirty stages.

`AudioFileProcessorLifecycleTest` adds 18 authored cases / 25 scenarios using production AFP and real view implementations: file/embedded/missing and legacy settings, borrowed filenames, callbacks that retire actors or throw, admission/reentry/thread/event policy, serialization mutation and retained encoding buffers, resource getter/loader/result-inspection retirement, successful native structured load/clear, selector retirement, same-path backing mutation during dirty notification, and Runtime rollback/replay/reopen for standard/nonstandard publication errors. Two Runtime scenarios require discovery of the built AFP module. Every native case is uncompiled/unrun. Source checks pass for 512 changed/new C++ files and 162 operations; this is not C++/Qt/MOC/link/ABI or recovery evidence.

The FileResource and AFP layout changes require a complete application/plugin rebuild. QDomElement copies retain shared document state; linked snapshots do not freeze opaque controllers. Arbitrary destruction inside an executing native callee/constructor/getter/model mutex/plugin/Qt destructor, dangling getter selector returns, foreign mutation, global teardown and unsupported ownership bypass remain outside universal guarantees. Direct failures can retain buffer/control/settings/dirty/error/journal/file effects. Whole sample/DOM storage, repeated inventory/snapshot traversal, controller capture, CPU/RSS/latency and inherited cooperative budgets remain unmeasured, with no hard native decode/allocation/plugin deadline. Native GUI/automation/legacy/project/preset/embedded/bundle/asset/checkpoint/journal/backend/DSP regressions, actual PCM, hardware, real-provider autonomous production and complete DAW acceptance remain outstanding.

### Native Hydrogen import continuation (2026-10-15, source only)

Hydrogen import now uses per-instance filenames and the native import work scope for direct and dispatched calls. It explicitly accepts the Song destination; PatternStore/custom targets are refused rather than silently importing into a different container. Cooperative local XML reading and a complete scalar/reference plan precede model creation. Instrument IDs, pattern aliases, references, keys, numeric values and timeline accumulation are checked; unrelated formats decline without spurious Hydrogen warnings. Legacy byte-escape scanning now checks bounds and always advances, preserves ASCII entities for the XML parser, recognizes declaration/BOM handling and enforces the Qt input range. Whole DOM parsing remains blocking.

Every source pattern, including empty patterns, gets a retained native PatternTrack before instrument rows are created. Sequence references resolve to those identities instead of guessed Song track indices. Native AudioFileProcessor rows use direct filenames or the legacy first layer/component, resolving relative paths against the source directory. Imported cell and arrangement lengths retain source ticks; simultaneous groups advance by their longest source pattern. Hydrogen documents simultaneous patterns and longest-pattern column duration in its [official manual source](https://github.com/hydrogen-music/documentation/blob/main/manual.html). That informs the group plan; it does not verify the entire file schema or native playback. Empty sequence groups use a documented one-bar inference, and optional BPM is represented by native tempo automation. Weak actor/clip/instrument/column checks and exact-created reverse cleanup preserve caller-owned additions and restore pattern selection when the captured context survives.

Silent AudioFileProcessor instances now retain unresolved sample filenames in native resources and project XML. A failed replacement of an already decoded sample preserves its original resource. Successful or embedded loading clears the unresolved marker. Hydrogen keeps silent rows editable and reports missing references. `project.import` now includes native warnings on success, making first-layer, forward-playback, note-off/choke, gain and unmapped metadata approximations visible. Missing-dependency bundling, asset-store checkpoint acceptance and relinking remain unverified; a retained filename does not establish decoded or audible content.

`HydrogenImportTest` adds 22 authored cases / 51 scenarios: malformed preflight, direct/layer/component/empty samples, retained original cells and selection, empty/sub-bar patterns and simultaneous groups, tempo binding, per-instance files, missing-reference save/reload, decoded replacement refusal, approximation warnings, cancellation, actor/importer retirement, callback-owned tracks, nested/foreign work, legacy/Unicode XML, and native module Runtime warning/history/replay/reopen plus standard/nonstandard publication rollback. Three Runtime scenarios require discovery of the built module; all native scenarios are uncompiled/unrun. Source checks cover 510 changed/new C++ files and 162 operations and do not prove C++/Qt/MOC/ABI correctness or successful native recovery.

The legacy first-layer/component, forward playback, ordinary-note choke approximation, octave offset and volume clamp remain limitations. Velocity/random layer selection, component routing, reverse/loop/envelope/effect/probability/lead-lag behavior, author/notes/license presentation and newer schemas are incomplete or unverified. Sample lookup does not implement Hydrogen drumkit search paths. PatternStore preview rounds to bars, while arrangement clips retain exact source lengths; shorter simultaneous-pattern repetition, fractional tempo and pitch/audio fidelity need golden comparisons. Upstream schema/source files were not retrieved here, so no broad modern-format compatibility claim is made. Direct failure can retain callback/model/dirty/error/journal/MIDI/file effects; Runtime recovery is required and unproven. Returned-stage checks do not universally protect arbitrary executing constructors/callees/getters/mutex/plugin/Qt destructors, dangling returns, throwing cleanup, global teardown, foreign mutation or ownership reuse. Memory, repeated scans/legacy buffer edits and CPU/RSS/latency are unmeasured. Full native application/AFP/plugin rebuild and GUI/legacy/project/preset/bundle/asset/checkpoint/journal/DSP/backend/MIDI regressions remain necessary. Native PCM, hardware, provider production and full DAW acceptance remain unproven.

### Native MIDI import continuation (2026-10-15, source only)

MIDI import now uses a protected native import work scope. A direct call owns the project/journal/model/event session; a dispatched call can attach only to the exact active filter, destination and pattern. Per-filter admission refuses same/other importer reentry. Weak cleanup restores the working flag, and direct entry now shares project-transition refusal and the prior journal policy. The native file-open diagnostic can operate on a local `QFile`; MIDI decoding uses that independent input, preserving its lifetime if the importer retires during cooperative reads.

`MidiImportState` tracks weak created tracks/owners, clips, selected instruments and target models and checks returned native stages. It explicitly creates and owns the initial empty PatternStore column, refuses a callback-created column before that creation, and uses native track/cell factories. Failure visits only its own surviving registered creations in reverse, with cancellation suspended and project-generation/parent/container checks. Callback-created tracks and columns are retained. This replaces raw container snapshots and “delete everything new” cleanup; direct failure still does not promise project rollback.

Native PortSMF decoding, channel/program/CC mapping, SoundFont/Patman defaults, tempo/meter automation and pattern geometry remain the implementation. Song note splitting copies native Note values before callback-capable work, retains source pointer/lifetime identities, and checks the source roster and selected scalar values before retirement. Weak clip/instrument references protect later returned stages. Progress dialog ownership is weak, and final native state is checked before commit.

`MidiImportTest` adds 10 authored cases / 19 scenarios: prior journal policy; early/partial custom destination retirement; track/clip/instrument retirement in Song and PatternStore; later metadata notification retiring an earlier track; callback-owned track/column preservation; direct nested import/project refusal; importer retirement during local input reading; split-source removal/edit; and standard/nonstandard partial-work failure with retry. The suite now has 37 cases / 110 scenarios, including existing SoundFont-conditional cases. All remain uncompiled/unrun. Source checks cover 507 changed/new C++ files and 162 operations; they do not establish native C++/Qt/MOC/ABI correctness or executed recovery.

These are returned-stage and owner-thread bounds. They do not universally protect deletion inside an executing constructor/callee/getter, clip/model mutex scope, plugin/DSP routine or Qt destructor; dangling factory returns, throwing cleanup, global teardown, foreign mutation, raw ownership bypass and same-address reuse remain limited. Note copies retain detuning ownership without freezing every opaque control. Whole input/parser/model/note snapshots remain resident; repeated actor scans and note validation have unmeasured CPU/RSS/allocation/latency costs. Native copying, sorting and plugin/resource work have cooperative budgets without hard deadlines. Direct failure can retain callback changes and dirty/error/journal/MIDI-assignment/file effects; Runtime checkpoint recovery remains required and unverified. Initial-column timing, direct journalling/admission and base class layout/API changes require a full application/plugin rebuild and GUI/legacy/SMF/RIFF/format-2/SMPTE/SoundFont/Patman/PatternStore/controller/checkpoint/journal/DSP/MIDI regressions. Hydrogen's internal global-state, target, XML and layer/sequence risks remain. Native PCM, hardware, real-provider production and full DAW acceptance remain unproven.

### Native import dispatch continuation (2026-10-15, source only)

`ImportFilter::import` now owns a guarded native dispatch session. It retains weak Song/destination/PatternStore/Mixer/hidden-track identities, original native/custom track roster prefixes, the selected pattern column, project/mixer replacement generations and filename. It holds the native model-change guard, suppresses cooperative event dispatch, restores journalling through the captured journal identity and refuses nested import, clear, load, save or project creation. Paths are copied before work; copied descriptor names receive independent UTF-8 filename buffers.

Candidate cleanup uses weak QObject identity and preserves explicit parent adoption. Dispatch checks returned factory, filter and destructor stages. A declining or non-filter candidate must leave the exposed project snapshot and destination roster unchanged before another filter is attempted; successful content is checked again after candidate destruction. This does not provide automatic rollback for direct callers. Structured `project.import` pins the destination/column before cooperative work, then validates native context, exposed state and dirty state around publication and inspection. Runtime remains responsible for native checkpoint recovery after failure. GUI import dialogs retain weak owners, failed drops are ignored, and file-browser model-change ownership uses RAII; all GUI behavior remains untested.

`ImportFilterLifecycleTest` adds 21 authored cases / 46 scenarios, including journal/fallback behavior, factory/filter decline mutations, standard/nonstandard exceptions, null/non-filter results, self-retirement/adoption, destination and original-row lifetime, roster/column replacement, nested transitions, borrowed/UTF-8 paths, cancellation, event/thread admission, reused mixer master and destructor edits. Four scenarios use the actual MIDI import plugin, when discovered, to exercise Runtime recovery/replay/reopen after publication exceptions, visible edits and project replacement. Other factories inject deterministic native dispatch faults through a private test access helper. The native plugin build dependency is registered, but every native scenario remains uncompiled/unrun. Source diagnostics cover 507 changed/new C++ files and 162 operations.

Fixture descriptor constants were also renamed in 17 earlier files (18 address references) and the new fixture. Sixteen earlier files used the unqualified name in plugin-derived constructors, where inherited `Plugin::descriptor()` hides the namespace constant; one was renamed for consistency. The checker now rejects that specific initializer pattern. Earlier source checks missed the lookup problem; passing source checks still do not establish C++ compilation.

This outer boundary does not repair MIDI import's internal raw actor/note references or rollback-container lifetime. Hydrogen import still has global filename/instrument state, an ignored destination and unchecked XML references/indexing; layer and sequence behavior remain unverified. Direct plugin `tryImport`/`readSong` calls bypass this session. Arbitrary executing-plugin/constructor/destructor/global teardown, dangling factory returns, throwing cleanup, foreign QObject mutation, library unloading and raw/same-address ownership changes are not universally protected. Finite snapshots exclude opaque and selected non-model contents; direct failures may retain partial project, journal, dirty/error, assignment or file effects. Changed fallback/admission/event behavior requires full legacy/import/plugin/GUI/project/checkpoint/journal/DSP/MIDI compatibility testing and a native Qt/MOC/ABI rebuild. Native PCM, hardware, real-provider production and CPU/RSS/latency evidence remain absent; full DAW acceptance is unproven.

### Native project creation continuation (2026-10-15, source only)

Empty, default and template creation now share a private weak owner guard. It admits owner-thread work, refuses overlapping project transitions, suspends cooperative event dispatch, holds the native model-change guard and restores the prior loading and journal policy on every exit. Default creation retains user-template precedence, factory-template fallback and native Triple Oscillator/Kicker construction when neither template exists. Created track and instrument identities are checked after later construction stages. Missing native builtins and failed default templates are reported; an invalid startup template does not recursively create another project. Template loading receives a one-shot internal admission permit, consumed before callbacks.

Final filename, length and modified notifications validate the exposed project snapshot and the expected blank filename and clean state. Structured `project.new` additionally checks errors, Song/Mixer replacement generations, filename, dirty state and the exposed snapshot around dirty notification and inspection. GUI new/template callers retain weak owners across their confirmation dialog and catch creation exceptions. These GUI paths and failure presentation remain untested. Failed or cancelled template creation now leaves the native failure visible instead of recursively substituting another project.

`ProjectCreationLifecycleTest` adds 20 authored cases / 25 scenarios: journal policy; actual builtin creation and checkpoint reopening; isolated user/factory template precedence; invalid and missing templates; nested replacement and load-permit consumption; standard/nonstandard clear and template-header failures; earlier track retirement; borrowed filenames; final notification edits and dirty-state reassertion; queued-work suppression; cancellation and thread refusal; Runtime recovery/replay/reopen. Its actual builtin cases depend on the native Triple Oscillator and Kicker modules. All cases are uncompiled/unrun. Source checks cover 506 changed/new C++ files and 162 operations; they do not establish compilation, Qt/MOC/ABI correctness or native execution.

This review also corrected 17 private boolean `Song::setModified` calls across 15 earlier fixture files. A test-only friend helper preserves the private application API. Earlier source checks missed this access error; the new targeted check prevents that specific direct-call pattern, not general C++ access/type errors.

Direct creation can retain partial state after failure; Runtime remains responsible for native checkpoint rollback. The finite snapshot does not cover every opaque plugin property or non-model value. Arbitrary executing-callee, constructor/destructor, global teardown, foreign QObject and unsynchronized raw mutation safety is unproven. Removing headless creation event pumping and changing unavailable-default/template failure behavior require full GUI/startup/legacy/template/plugin/project/checkpoint/journal regressions. Native rebuild and all native fixtures, DSP/PCM, hardware, real-provider production and CPU/RSS/latency measurements remain outstanding. Complete DAW acceptance remains unproven.

### Native mixer continuation (2026-10-15, source only)

Mixer topology, solo and restoration flags now use weak owner cleanup instead of raw rollback references. Channel continuations retain weak mixer/effect-chain identities and validate the current roster row after callbacks. Creation, selector ranges, reset/delete/move, assignment remapping, solo selection/masking/release and same-object solo journal restoration check returned native stages. Channel model members detach their QObject parent before destruction, preventing a destruction observer from making the retiring mixer delete member storage again. Routes detach all ownership views before their amount model is destroyed; returned route updates/creation retain weak routes and endpoints. Loading adds cooperative checks around validation, clear/allocation, channel models/effects/sends and final notifications while retaining native project representations and solo-release behavior.

Process-local Song and Mixer replacement counters distinguish reused master/channel objects from their earlier project context. Song advances its counter on admitted clearing and valid file loading before filename publication; Mixer advances on native clear. Structured mixer operations capture selected channel/endpoint identities before cooperative work, resolve surviving indices, validate context between edits, and check ordered channel/effect/route identities, selected metadata and stored values after project dirty notification. These counters are not serialized project IDs or a substitute for Runtime revision/checkpoint history. They currently protect mixer capability continuity; other native operations still need their own context audit.

`MixerLifecycleTest` contributes 20 authored cases / 31 scenarios: native route/solo roundtrip; model/effect/reset/solo/journal callback retirement; route amount, direct retirement and amount-model destruction; channel member destruction; selector/move notifications; standard/nonstandard cleanup; reentry/cancellation; invalid graph preservation; target removal/reordering; reused master and nested project replacement; changed post-dirty topology; and Runtime mixer-load failure rollback/replay/reopen. All native fixtures are uncompiled/unrun. Source checks do not prove C++ type/ABI correctness, destruction safety, native graph/audio behavior or successful recovery.

Direct operations may retain partial native changes on failure; Runtime remains responsible for checkpoint rollback and stopping when recovery fails. Post-notification checks cover visible mixer state, not every opaque control, binding or plugin property. Raw channel deletion outside mixer ownership, unsynchronized direct route deletion, arbitrary destruction inside executing constructors/destructors/getters/plugin/DSP code, foreign QObject mutation, global teardown and same-address reuse remain unsupported as universal guarantees. Snapshot/traversal allocation and CPU/RSS/realtime latency are unmeasured. Mixer, route, control-model and Song layout/destruction changes require a full native rebuild, sanitizers and Qt/MOC/ABI/GUI/plugin/DSP/MIDI/legacy/project/checkpoint/journal regressions. Native PCM, hardware, provider workflows and complete DAW acceptance remain unproven.

### Native MIDI port continuation (2026-10-15, source only)

MidiPort output locks now retain shared ownership of their mutex storage. A returned backend callback can retire a port before an enclosing lock unwinds; the lock releases retained storage rather than a destroyed QObject member. Move assignment unlocks the old mutex before releasing its owner. Native client teardown and transfer entries use the same lock type. This changes port layout and adds shared-owner reference-count work on MIDI output paths; native latency and lock-order measurements remain required.

Weak transition admission covers settings, name/source/mode changes, per-direction subscriptions and output release. Serializers check port/client survival around cooperative work and after all eleven model fields. Mode updates check each notification and use weak signal-block cleanup on failure. Subscription names are copied before callbacks, and discovery refreshes iterate local maps across native getter/subscription calls. Source/mode/cleanup paths check returned backend calls before touching port members or ledger state. Recursive release is refused, ordinary output is suppressed during release/retirement, and uncertain note/pedal cleanup is retained through the existing retirement hook. Retarget rollback uses weak models. Queued transfer notifications check the surviving port and selected client after each signal. Structured/legacy subscription names, unavailable-name intent and legacy missing-base-velocity behavior remain represented natively.

`MidiPortLifecycleTest` has 16 authored cases / 49 scenarios: exact/unavailable endpoint roundtrip; eleven loading and eleven saving field retirements; three mode signals; nine backend callback boundaries including refresh subscriptions; uncertain cleanup retirement; retained-lock move/unlock after deletion; nested mode/release/settings refusal; standard/nonstandard backend faults; borrowed endpoint names; retarget model death; legacy velocity; actual MidiController port retirement; early/late cancellation; and Runtime MIDI load failure recovery/replay/reopen. All native cases are uncompiled/unrun. The client/processor fixtures record software attempts and do not establish hardware delivery.

These checks do not make routing/model/backend changes atomic. Returned faults can retain partial state, with Runtime responsible for checkpoint recovery where applicable. Backend-owned mutexes/workers, arbitrary destruction inside executing constructors/destructors/native callbacks/getters/processors, foreign QObject mutation, same-address reuse and global engine/client shutdown remain separate lifetime contracts. Full native C++/Qt/MOC/ABI/GUI/legacy/checkpoint/transfer/plugin/DSP/MIDI/backend regressions, hardware/PCM/provider workflows and CPU/RSS/latency evidence remain outstanding. Mixer restoration/clear/allocation/route/solo continuations still need work. Complete DAW acceptance remains unproven.

### Native controller settings continuation (2026-10-15, source only)

Base, LFO, MIDI and Peak controller serializers now hold native audio/model ownership and event suppression with a shared settings guard. It refuses nested settings transitions, retirement and foreign-thread entry, checks weak controller survival around cooperative work, and clears admission only on a surviving receiver. Protected base-field helpers let builtin serializers retain the guard across their own fields. LFO serialization/restoration checks each model boundary; waveform loading copies the path and decodes into a local shared buffer before assigning it to a surviving controller, then checks the publication notification. Public waveform replacement is refused during settings transitions. MIDI guards returned base/port/name boundaries; Peak save also checks its effect identity.

Controller XML creation now retains weak candidate, parent and Song identities. Failed creation cleans up only a new, surviving, unadopted LFO/MIDI controller whose original parent identity is unchanged and which has no connections or rack membership. Shared Dummy and effect-owned Peak controllers are retained; explicit rack adoption or detachment is preserved. Song rack restoration checks prior and newly restored weak entries and verifies that registration survived callbacks. Structured waveform setting checks rack membership after decode and selection, verifies sample/selector identity after project dirty notification, and returns the current controller index. The optional GUI waveform chooser checks dialog, model and Song lifetimes before and after its modal/file operations.

`ControllerSettingsLifecycleTest` has 16 authored cases / 32 scenarios for native LFO model/wave roundtrip and finite controller samples; six loading and six saving field retirements; LFO/MIDI rename death; nested transitions; standard/nonstandard cleanup; decode/publication death; failed decode preservation; pending factory cleanup/adoption/Dummy/detached-parent ownership; returned MIDI port failure; selection/dirty retirement; rack registration failure with Runtime recovery; and waveform failure replay/reopen. All native fixtures remain uncompiled/unrun. Native controller-buffer samples and deterministic WAV decoding are not rendered audio or hardware evidence.

Direct settings changes may leave partial state after a fault, while Runtime retains its existing checkpoint rollback responsibility. Custom controller serializers must protect their own fields. MidiPort internals still contain raw lifetime and mutex/signal continuations; caller checks do not make destruction inside those callees safe. Mixer restoration/clear/allocation/route/solo internals also need further work. Peak effect/legacy mapping changes have no actual effect fixture in this turn. Controller layout/protected API changes require a full native rebuild and ABI/Qt/MOC/GUI/plugin/legacy/checkpoint/link/automation/DSP/MIDI regressions. Arbitrary executing-callee or global-engine destruction, same-address mutation, hardware acknowledgment and hard deadlines are not established. Performance and full native/provider music-production acceptance remain unproven.

### Native project load continuation (2026-10-15, source only)

Song file loading now copies its input path before cooperative callbacks, validates project/template input before publishing the filename or clearing the project, and rejects nested or foreign-thread loading. Save, clear, new and template entry points refuse replacement while a file load is active. Weak Song cleanup restores loading flags and restores journalling only while the original journal identity remains current. Returned stages check Song, PatternStore, Mixer, hidden automation track and journal identity, including header/model restoration, container/mixer/controller/tuning/timeline stages, linked finalization, `projectLoaded`, and final length/modified notifications. Structured `project.load` uses cooperative cancellation and event suppression with weak checks around dispatch and project dirty notification.

Project clearing has separate weak admission and context checks; native track/controller teardown walks stop after observable owner death. Headless clear no longer dispatches posted work during replacement; the GUI path retains its posted-event handling. TrackContainer restoration uses weak owner/Song state, nested-load/save admission, cooperative checks and a weak shared progress dialog with exception cleanup. Only track child elements create tracks; comments and extension metadata do not accidentally create instruments. Unsupported track types produce a native warning, which the structured load path treats as an incomplete restoration. Loading-track counting now traverses the actual DOM container list without expanding its bound with nested child counts. The track factory holds weak pending candidates after construction and across returned update/restoration callbacks, avoiding cleanup of an already-retired candidate. Failed template loading retains the previous project filename.

`ProjectLoadLifecycleTest` has 15 authored cases / 26 scenarios for XML/compressed/template loading, preflight preservation, nested replacement, borrowed paths, failed templates, standard/nonstandard header/loaded/clear failures, queued-work suppression, track/container retirement, four factory-update faults, nested container restoration, Runtime rollback/replay/reopen, cancellation and unavailable track recovery. All are uncompiled/unrun. The earlier project-save fixture now compares the actual saved hidden clip count, preserving LMMS's default global curves instead of assuming a single clip.

Direct native loading remains destructive and can leave partial models and filename changes after a fault. Runtime retains responsibility for mandatory checkpoint rollback and stops when recovery fails; these guards do not make loading atomic. Existing Mixer/controller/LFO/MIDI and other native load callees still need their own continuation audits. Arbitrary destruction inside executing constructors/callees/hooks/destructors and global shutdown is not made safe. GUI/modal/progress, default-project creation, plugin/legacy/project/checkpoint/journal/DSP/MIDI compatibility, native compilation and execution, PCM/hardware/provider workflows and performance remain unverified. Full commission acceptance is unproven.

### Native project save publication continuation (2026-10-15, source only)

Native project saves now capture a project source baseline before serialization and validate it after the native serializers and before both backup publication and the final atomic file commit. This covers ordered Song and PatternStore tracks, Song's canonical hidden automation track, exposed parameter identities and linked stored state, controller identities and persisted names, mixer channel/effect/send/route identities and selected metadata, shared scale/keymap slots, and persisted timeline/current-pattern settings. The hidden automation track remains outside the normal track roster, as required by LMMS; only Song's canonical hidden track receives that exception. This also corrects the preceding track-save guard's rejection of canonical hidden tracks.

TrackContainer copies weak track identities under a short read lock and releases it before invoking native serializers, avoiding the old read-lock/write-lock deadlock when a serializer removes a track. Song has owner-thread and nested-save admission, weak save-flag cleanup, returned serializer failure diagnostics, and load/clear refusal during serialization. Mixer serialization holds topology admission and checks weak channel/send anchors between callbacks. Controller serialization uses a weak rack roster; scale/keymap serializers retain local shared ownership. Structured `project.save` keeps its existing non-project-mutating classification, runs with cooperative cancellation and event suppression, and returns `committed: true` with the actual path only after native file publication. It performs no source callbacks afterward. GUI save continuation also checks Song lifetime.

`ProjectSaveLifecycleTest` has 14 authored cases / 28 scenarios for empty MMP/MMPZ and canonical hidden tracks, seven changes by later serializers, track/controller removal, nested save/load/clear, standard/nonstandard faults, five final work callback faults, container death, mixer admission/recovery, controller rename, builtin instruments with pattern/hidden checkpoint reopen, and Runtime receipt replay/reopen. All native fixtures are uncompiled and unrun. The source checker verifies source structure, registration, delimiters and whitespace only.

This baseline is not an immutable copy of every project property. Opaque plugin state, non-model note/curve/sample contents, controller internal fields, in-place scale/keymap contents, GUI/preserved metadata, and external files are not comprehensively captured. Refused writes can retain live serializer changes and already-published backups/resources/folders. No automatic project rollback or receipt/crash durability is promised by this save operation. Model traversals, repeated per-track/global groups and mixer/rack scans need native performance measurement. Full build, Qt/MOC/ABI, GUI, legacy/project/bundle/checkpoint/preset/journal/plugin/DSP/MIDI compatibility, PCM, hardware and provider workflows remain unverified; complete DAW acceptance is unproven.

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
