# Playbook 02: Zero-Hesitation Developer Ergonomics

<!-- docs-scope: current-guide -->

Design doctrine governing the external developer and agent experience for HEAV3NS-MUSE. The objective is **zero hesitation**: interactions must be intuitive, predictable, self-documenting, and provide immediate actionable feedback.

---

## 1. Ergonomic Design Principles

1. **Familiar Standard Shapes**: Use naming conventions and patterns that music producers and ML engineers already know (e.g., `track.create`, `clip.insert`, `note.add`, `tempo.set`).
2. **Zero-Configuration Defaults**: When initializing a client SDK instance (`StudioClient()`), the client automatically locates the active studio session (checking `/tmp/muse.sock`, environment variables `MUSE_SOCKET` / `MUSE_PORT`, or falling back to `127.0.0.1:7777`).
3. **Structured & Actionable Errors**: Never return a generic `"error": "failed"`. Every error must return a standard error object with `code`, `message`, and an explicit `recovery_action`.
4. **Interactive Self-Discovery**: The studio protocol exposes `studio.help` and `studio.capabilities` returning machine-readable parameter schemas and allowed value ranges so agents do not need to guess.

---

## 2. Standardized Error Payload Architecture

When a studio command fails, the JSON-RPC error payload follows this deterministic contract:

```json
{
  "jsonrpc": "2.0",
  "id": "req-104",
  "error": {
    "code": -32001,
    "message": "Track index 8 out of bounds",
    "data": {
      "category": "INVALID_TRACK",
      "valid_range": [0, 3],
      "active_tracks": 4,
      "recovery_action": "Call track.list to inspect existing track identifiers or call track.create to append a new track."
    }
  }
}
```

---

## 3. Python SDK Ergonomics

The Python client SDK provides both high-level fluent helpers and raw JSON-RPC dispatch:

```python
import muse

# 1. Zero-config connection
client = muse.StudioClient()

# 2. Fluent, readable sequencing
track = client.create_track("Sub Bass", synth="TripleOscillator")
clip = track.create_clip(start_tick=0, length_ticks=192)
clip.add_note(pitch=36, velocity=100, tick=0, duration=96)

# 3. Synchronous offline render
output_wav = client.export_wav("/tmp/preview.wav", duration_bars=4)
print(f"Rendered: {output_wav}")
```

---

## 4. Verification & Auditing Ergonomics

When verifying user flows or SDK APIs:
- Always test with `python3 tests/studio_process_test.py --lmms build/lmms`.
- Ensure tests verify both successful calls and error recovery paths.
- Keep cognitive load minimal: avoid deeply nested configuration structures where flat keyword arguments suffice.
