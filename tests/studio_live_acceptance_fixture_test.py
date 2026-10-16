"""Test acceptance instrumentation with real LMMS and a deterministic HTTP fixture.

A pass validates instrumentation, not GATE-014 or real-provider musical quality.
"""
import argparse
import json
import os
from pathlib import Path
import re
import time

from studio_live_acceptance import main as acceptance_main, require
from studio_process_test import ProviderFixture


class AcceptanceFixture(ProviderFixture):
    def response(self, body):
        content = body["messages"][-1]["content"]
        if isinstance(content, list):
            return super().response(body)  # Decode the actually submitted native PCM.
        observation = json.loads(content.split("\n", 1)[1])
        intent = observation["intent"]
        seed, source_digest = re.search(r"audio_listen on (.*?) with sourceSha256 ([0-9a-f]{64})", intent).groups()
        project, audio = re.search(r"After editing, save (.*?) and render (.*?) as stereo", intent).groups()
        steps = [
            self.call("fixture-listen", "audio_listen", {"path": seed, "sourceSha256": source_digest,
                      "prompt": "Instrumentation fixture: report receipt only, no musical-quality claim"}),
            self.call("fixture-edit", "note_edit", {"track": 0, "clip": 0, "note": 0, "key": 62}),
            self.call("fixture-save", "studio_execute", {"op": "project.save", "args": {"path": project}}),
            self.call("fixture-render", "audio_render", {"path": audio, "bitDepth": 16}),
            self.call("fixture-finish", "agent_finish", {"status": "complete",
                      "summary": "Deterministic instrumentation test only; no aesthetic acceptance.",
                      "evidence": ["fixture-listen", "fixture-edit", "fixture-save", "fixture-render"]}),
        ]
        require(self.phase < len(steps), "Fixture received an unexpected extra reasoning round")
        call = steps[self.phase]
        self.phase += 1
        return {"role": "assistant", "content": None, "tool_calls": [call]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lmms", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve() / str(time.time_ns())
    output.mkdir(parents=True, mode=0o700)
    fixture = AcceptanceFixture(output / "unused.wav")
    name = "MUSE_ACCEPTANCE_INSTRUMENTATION_FIXTURE"
    previous = os.environ.get(name)
    os.environ[name] = json.dumps(fixture.configuration)
    try:
        result = acceptance_main(["--lmms", str(args.lmms), "--output", str(output),
                                  "--reasoning-config-env", name, "--audio-config-env", name])
        require(result == 0 and not fixture.errors, "Acceptance instrumentation failed against the HTTP fixture")
        require(fixture.phase == 5 and len(fixture.audio_observations) == 1,
                "The fixture did not observe every stage and real submitted audio")
        report = {"status": "fixture-instrumentation-passed", "realProviderVerified": False,
                  "gateCompleted": False, "reasoningRounds": fixture.phase,
                  "submittedAudio": fixture.audio_observations}
        (output / "fixture-verification.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report, indent=2))
    finally:
        fixture.close()
        if previous is None:
            os.environ.pop(name, None)
        else:
            os.environ[name] = previous


if __name__ == "__main__":
    main()
