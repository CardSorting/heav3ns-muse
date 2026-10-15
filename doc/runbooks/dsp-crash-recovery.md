# Runbook: DSP Crash Recovery & Plugin Isolation

<!-- docs-scope: current-guide -->

Operational procedure for isolating, diagnosing, and mitigating instrument or effect plugin crashes in the headless HEAV3NS-MUSE runtime.

---

## 1. Symptoms & Triage

A DSP plugin crash typically presents as:
- Immediate `SIGSEGV` or `SIGFPE` during `song.play()` or `song.export()`.
- Abrupt termination of the `lmms --studio` headless process with broken socket connection on the client SDK.
- Audio silence or NaN/Inf floating-point values propagated through the `Mixer`.

---

## 2. Isolation Workflow

### Step 1: Isolate the Faulty Plugin
Reproduce the issue by loading the project manifest and inspecting which instrument track triggers the fault:

```sh
# Run inside LLDB with immediate backtrace on crash
lldb -o "run" -o "bt" -- ./build/lmms --studio --socket /tmp/muse-triage.sock
```

### Step 2: Disable Rogue Plugin Headless
If a specific plugin (e.g., an external VST or experimental LADSPA effect) is crashing the host, bypass it via the studio protocol:

```python
import muse
client = muse.StudioClient(socket_path="/tmp/muse-triage.sock")

# Inspect loaded tracks
tracks = client.list_tracks()
print("Active tracks:", tracks)

# Bypass or remove the crashing track
client.remove_track(track_index=2)
```

### Step 3: Audit DSP Numerical Safety
Inspect the plugin's C++ audio generation loop:
1. **Divide-by-Zero Checks**: Check frequency, resonance, and filter cutoff calculations.
2. **Buffer Overruns**: Ensure write indices do not exceed `AudioEngine::framesPerPeriod()`.
3. **Denormal / NaN Protection**: Ensure audio buffers flush denormals to zero (`_mm_setcsr(_mm_getcsr() | 0x8040)`).
