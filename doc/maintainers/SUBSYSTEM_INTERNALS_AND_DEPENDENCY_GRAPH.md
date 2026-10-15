# Subsystem Internals & Dependency DAG

<!-- docs-scope: domain-reference -->

Comprehensive architectural reference documenting the internal subsystem relationships, threading models, memory allocation invariants, and boundary rules of HEAV3NS-MUSE.

---

## 1. Top-Level Architectural Dependency DAG

```mermaid
flowchart TD
    Client[AI Agent / Python SDK / CLI Client] -->|JSON-RPC 2.0| Wire{IPC Transport}
    Wire -->|Unix Domain Socket| SockHandler[StudioSocketHandler]
    Wire -->|Loopback TCP 127.0.0.1| SockHandler
    
    SockHandler -->|Parse Request| Router[StudioCommandRouter]
    
    subgraph MainThread ["Main Thread (QCoreApplication Event Loop)"]
        Router -->|Synchronous Mutation| Song[Engine::getSong()]
        Song --> Tracks[TrackContainer / InstrumentTrack]
        Song --> Patterns[PatternStore / Pattern / Note]
        Song --> Automation[AutomationPattern]
        Song --> MixerModel[Mixer / MixerChannel]
    end

    subgraph AudioThread ["Real-Time Audio Worker Thread (Zero Allocation)"]
        AudioEngine[AudioEngine::renderNextBuffer]
        AudioEngine --> MixerDSP[Mixer::renderChannels]
        MixerDSP --> SynthDSP[Instrument::playNotes / renderAudio]
        SynthDSP --> EffectsDSP[Effect::processAudio]
        EffectsDSP --> RingBuffer[Lock-Free SPSC Ringbuffer]
    end

    MainThread -->|Lock-Free MIDI / Param Queue| AudioThread
    AudioThread -->|Rendered Audio Blocks| HardwareOut{Audio Backend}
    
    HardwareOut --> CoreAudio[AudioCoreAudio (macOS)]
    HardwareOut --> ALSA[AudioAlsa (Linux)]
    HardwareOut --> Pulse[AudioPulseAudio (Linux)]
    HardwareOut --> Jack[AudioJack (Pro Audio)]
    HardwareOut --> Dummy[AudioDummy (Headless CI)]
```

---

## 2. Real-Time Audio Threading Model & Invariants

Maintaining professional audio stability requires absolute isolation between the real-time audio thread and the command dispatch thread:

### Invariant 1: Zero Allocation in the Audio Thread
The audio rendering callback (`renderNextBuffer`) must execute within a strict deadline (e.g., 1.45ms for a 64-sample buffer at 44.1kHz). The audio thread:
- **MUST NOT** invoke `malloc()`, `free()`, `new`, `delete`, or any dynamic container reallocations.
- **MUST NOT** acquire non-realtime mutexes (`std::mutex`, `QMutex`) that can cause priority inversion.
- **MUST NOT** perform filesystem I/O, network I/O, or printf logging.

### Invariant 2: Lock-Free Communication
Parameters, note events, and MIDI triggers dispatched from the JSON-RPC main thread to the DSP thread travel through single-producer single-consumer (SPSC) lock-free ringbuffers.

### Invariant 3: Main-Thread State Mutation Authority
Structural modifications to the song graph—such as adding tracks, instantiating new instrument plugins, or resizing patterns—are executed strictly on the main `QCoreApplication` thread during synchronized cycle boundaries.

---

## 3. Memory & Buffer Allocation Invariants

| Component | Allocation Strategy | Invariant Boundary |
| :--- | :--- | :--- |
| **AudioEngine Buffers** | Pre-allocated at startup | Fixed buffer sizes (`64` to `2048` frames). Dynamically resizing buffers requires pausing the audio engine. |
| **Synth Voices** | Static voice pool | Synthesizers pre-allocate polyphony voice structures (e.g., 32 or 64 voices) during plugin instantiation. |
| **Automation Vectors** | Pre-reserved capacity | Automation curve points reserve memory during pattern creation to prevent reallocation during playback. |
| **Mel Spectrograms** | Paged memory arena | FFT and Mel filterbank memory is allocated in a reusable scratchpad during Stage 1 perception. |

---

## 4. Subsystem Lifecycle & State Transitions

```mermaid
stateDiagram-v2
    [*] --> Uninitialized
    Uninitialized --> Initialized: Engine::init() & QCoreApplication
    Initialized --> Configured: Load Project or Create Tracks
    Configured --> Playing: song.play()
    Playing --> Paused: song.pause()
    Paused --> Playing: song.play()
    Playing --> Rendering: song.export() (Offline DSP Loop)
    Rendering --> Configured: Export Finished
    Configured --> Stopped: song.stop()
    Stopped --> [*]: Engine::destroy()
```

---

## 5. Public Boundary & Export Hygiene

1. **Protocol Decoupling**: The JSON-RPC schemas in `src/core/studio_process.cpp` must expose stable, semantic capability operations (`track.create`, `clip.insert`, `note.add`) rather than leaking internal C++ pointer addresses or memory offsets.
2. **Sanitized Output**: Error responses must provide high-level error categories (`INVALID_PARAMETER`, `TRACK_NOT_FOUND`) alongside clear remediation guidance.
3. **SDK Independence**: The Python SDK (`muse/`) is self-contained and zero-dependency, relying solely on Python standard libraries (`socket`, `json`, `subprocess`).
