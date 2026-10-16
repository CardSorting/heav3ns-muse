"""Verify musical preference memory against real headless LMMS process restarts.

This harness uses existing studio operations. It does not implement a memory
store, infer preferences, contact a provider, or substitute a musical engine.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import platform
import time

from studio_process_test import Studio


INITIAL_MEMORY = {
    "favoriteInstruments": ["tripleoscillator", "audiofileprocessor"],
    "soundPalette": [{"name": "Warm lead", "instrument": "tripleoscillator",
                      "hints": {"attackMs": 12, "detuneCents": 3.5}}],
    "scalePreferences": {"name": "19-EDO", "rootMidi": 60, "divisions": 19,
                         "stepsCents": [0, 63.1578947368, 126.3157894737]},
    "compositionalRules": ["Leave space around the lead", "Keep bass below the melody"],
    "stylisticHeuristics": {"density": "sparse", "swing": 0.12, "avoidClipping": True},
    "notes": "Préfère des couleurs chaudes — 音楽 🎹",
    "retiredRule": "Replace this preference explicitly later",
}
REPLACEMENT_MEMORY = {
    "favoriteInstruments": ["tripleoscillator"],
    "soundPalette": [{"name": "Glass pad", "instrument": "tripleoscillator",
                      "hints": {"attackMs": 180, "detuneCents": -2.25}}],
    "scalePreferences": {"name": "12-EDO", "rootMidi": 57, "divisions": 12},
    "compositionalRules": ["Use three-note voicings"],
    "stylisticHeuristics": {"density": "open", "swing": 0, "avoidClipping": True},
    "notes": "A revised palette; the old rule must be absent",
}
OTHER_MEMORY = {"favoriteInstruments": ["patman"], "notes": "Separate session, separate taste"}


class MemoryStudio(Studio):
    def receive(self, timeout=None):
        if timeout is None:
            timeout = 90 if not hasattr(self, "ready") else 30
        return super().receive(timeout)


def project_signature(studio, identifier):
    state = studio.call(identifier, "project.inspect")["result"]
    return {"tempo": state["tempo"], "tracks": [
        {"name": track["name"], "notes": [clip.get("notes", []) for clip in track["clips"]]}
        for track in state["tracks"]]}


def verify_memory(studio, expected, identifier, observations):
    observed = studio.call(identifier, "session.inspect")["result"]["memory"]
    assert observed == expected, (identifier, observed, expected)
    # Independently read the native committed manifest; never write fixture state
    # into it. This must already match when the protocol acknowledges a mutation.
    manifest = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    assert manifest["memory"] == expected, (identifier, "committed manifest differs")
    observations.append({"case": identifier, "observedMemory": observed,
                         "manifestMatches": True, "revision": manifest["revision"]})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lmms", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.lmms.is_file():
        parser.error("A compiled native LMMS executable is required")
    if not __debug__:
        parser.error("Assertions must be enabled for native process verification")
    binary = args.lmms.resolve()
    root = args.output.resolve() / str(time.time_ns())
    root.mkdir(parents=True)
    studios, observations = [], []
    started = time.monotonic()

    def start(session):
        instance = MemoryStudio(binary, root / session, root)
        studios.append(instance)
        return instance

    try:
        first = start("session-a")
        verify_memory(first, {}, "a-initially-empty", observations)
        first.call("tempo", "song.set", {"tempo": 96})
        first.call("track", "track.add", {"type": "instrument", "name": "Palette reference"})
        first.call("instrument", "instrument.load", {"track": 0, "instrument": "tripleoscillator"})
        first.call("clip", "clip.add", {"track": 0, "position": 0})
        first.call("note", "note.add", {"track": 0, "clip": 0, "position": 0,
                                        "length": 48, "key": 60, "volume": 70})
        expected_project = project_signature(first, "a-project-before-memory")
        original = first.call("palette-original", "session.memory.set", {"memory": copy.deepcopy(INITIAL_MEMORY)})
        verify_memory(first, INITIAL_MEMORY, "a-acknowledged-initial-palette", observations)
        first.shutdown()
        assert first.process.returncode == 0

        other = start("session-b")
        verify_memory(other, {}, "b-does-not-inherit-a", observations)
        other.call("palette-original", "session.memory.set", {"memory": OTHER_MEMORY})
        verify_memory(other, OTHER_MEMORY, "b-acknowledged-independent-palette", observations)
        other.shutdown()

        resumed = start("session-a")
        verify_memory(resumed, INITIAL_MEMORY, "a-first-clean-restart", observations)
        assert project_signature(resumed, "a-project-after-clean-restart") == expected_project
        replay = resumed.call("palette-original", "session.memory.set", {"memory": INITIAL_MEMORY})
        assert replay.get("replayed") is True
        assert {key: value for key, value in replay.items() if key != "replayed"} == original
        verify_memory(resumed, INITIAL_MEMORY, "a-replayed-original-request", observations)
        for index, arguments in enumerate(({"memory": []}, {}, {"memory": {}, "extra": True})):
            resumed.call(f"invalid-memory-{index}", "session.memory.set", arguments, ok=False)
            verify_memory(resumed, INITIAL_MEMORY, f"a-invalid-memory-{index}-preserved", observations)
        resumed.call("palette-replace", "session.memory.set", {"memory": REPLACEMENT_MEMORY})
        verify_memory(resumed, REPLACEMENT_MEMORY, "a-acknowledged-replacement", observations)
        assert "retiredRule" not in REPLACEMENT_MEMORY
        resumed.call("palette-original", "session.memory.set", {"memory": OTHER_MEMORY}, ok=False)
        verify_memory(resumed, REPLACEMENT_MEMORY, "a-conflicting-id-preserved", observations)
        resumed.call("temporary-tempo", "song.set", {"tempo": 101})
        resumed.call("undo-project-only", "session.undo")
        verify_memory(resumed, REPLACEMENT_MEMORY, "a-native-undo-preserves-current-memory", observations)
        assert project_signature(resumed, "a-project-after-undo") == expected_project
        resumed.kill()  # Deliberate abrupt termination after the acknowledged commit.
        assert resumed.process.returncode != 0

        recovered = start("session-a")
        verify_memory(recovered, REPLACEMENT_MEMORY, "a-after-abrupt-restart", observations)
        assert project_signature(recovered, "a-project-after-abrupt-restart") == expected_project
        # An old acknowledged request returns its old response, never reapplies
        # old preference state on top of a newer acknowledged replacement.
        replay = recovered.call("palette-original", "session.memory.set", {"memory": INITIAL_MEMORY})
        assert replay.get("replayed") is True and replay["result"]["memory"] == INITIAL_MEMORY
        verify_memory(recovered, REPLACEMENT_MEMORY, "a-stale-replay-does-not-overwrite-current-memory", observations)
        recovered.shutdown()

        other_resumed = start("session-b")
        verify_memory(other_resumed, OTHER_MEMORY, "b-retains-independent-palette-after-a-recovery", observations)
        other_resumed.shutdown()

        final = start("session-a")
        verify_memory(final, REPLACEMENT_MEMORY, "a-second-clean-restart", observations)
        final.call("clear-memory", "session.memory.set", {"memory": {}})
        verify_memory(final, {}, "a-explicit-clear", observations)
        final.shutdown()

        cleared = start("session-a")
        verify_memory(cleared, {}, "a-clear-survives-clean-restart", observations)
        assert project_signature(cleared, "a-project-after-memory-clear") == expected_project
        cleared.shutdown()
        assert all(s.process.returncode == 0 for s in studios if s is not resumed)
        summary = {"status": "passed", "gate": "GATE-018", "applicationType": "QCoreApplication",
                   "platform": f"{platform.system()} {platform.machine()}",
                   "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                   "processStarts": len(studios), "independentSessions": 2,
                   "gracefulExits": len(studios) - 1, "abruptExitCode": resumed.process.returncode,
                   "memoryAssertions": len(observations), "projectPreserved": True,
                   "durationSeconds": round(time.monotonic() - started, 3), "observations": observations,
                   "limitations": "Local headless persistence only; no inferred taste, real-provider reasoning, physical hardware, power-loss or cross-platform acceptance."}
        (root / "verification.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        # Public-safe observations contain fixture values and hashes, no local
        # paths, configuration, raw diagnostics or private provider state.
        print(json.dumps(summary, indent=2, ensure_ascii=False))
    finally:
        for index, instance in enumerate(studios):
            if instance.process.poll() is None:
                instance.kill()
            (root / f"trace-{index}.json").write_text(json.dumps(instance.trace, indent=2, ensure_ascii=False), encoding="utf-8")
            (root / f"stderr-{index}.log").write_text("\n".join(instance.errors), encoding="utf-8")


if __name__ == "__main__":
    main()
