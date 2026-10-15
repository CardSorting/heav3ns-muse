"""Run native VST state/recovery through the real headless LMMS executable.

The Python code supplies test input and assertions. LMMS owns the project,
checkpoint history, VST worker, DSP and render. No inference quality is tested.
"""
from __future__ import annotations

import argparse
import array
import json
import os
from pathlib import Path
import shutil
import sys
import time
import uuid
import wave
import xml.etree.ElementTree as ET

# Import instrumentation without leaving bytecode beside the source tests.
sys.dont_write_bytecode = True
from studio_process_test import Studio


SELECTOR = {"channel": 0, "effect": 0}


class NativeSession:
    def __init__(self, studio, evidence, phase):
        self.studio = studio
        self.evidence = evidence
        self.phase = phase
        self.counter = 0

    def call(self, operation, args=None, *, ok=True):
        self.counter += 1
        response = self.studio.call(f"{self.phase}-{self.counter}-{operation}", operation, args, ok=ok)
        return response["result"] if ok else response

    def control(self, name):
        matches, offset = [], 0
        while True:
            page = self.call("parameter.list", {**SELECTOR, "offset": offset, "limit": 1024})
            matches.extend(item for item in page["parameters"] if item["name"].endswith(name))
            if not page["hasMore"]:
                break
            assert page["nextOffset"] > offset, page
            offset = page["nextOffset"]
        assert len(matches) == 1, matches
        return matches[0]

    def gain(self, expected):
        actual = self.control("Gain:x")["value"]
        assert abs(actual - expected) < 1e-6, (actual, expected)

    def set_gain(self, value):
        self.call("parameter.set", {"parameter": self.control("Gain:x")["id"], "value": value})
        self.gain(value)

    def program(self, index, expected):
        selected = self.call("plugin.program.select", {**SELECTOR, "program": index})
        assert selected["current"] == index, selected
        self.gain(expected)

    def verify_automation(self):
        # Native object handles belong to the current project revision. Rediscover
        # after restore and verify the actual connection, not the old integer ID.
        handle = self.control("Fault:x")["id"]
        curve = self.call("automation.global.inspect", {"parameter": handle, "position": 0})
        assert curve["value"] == 0 and len(curve["points"]) == 1, curve
        assert [target["parameter"] for target in curve["targets"]] == [handle], curve

    def render(self, name):
        path = self.evidence / f"{self.phase}-{name}.wav"
        self.call("audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16, "timeoutMs": 60000})
        with wave.open(str(path), "rb") as stream:
            assert (stream.getnchannels(), stream.getsampwidth(), stream.getframerate()) == (2, 2, 44100)
            samples = array.array("h", stream.readframes(stream.getnframes()))
        if sys.byteorder != "little":
            samples.byteswap()
        assert len(samples) >= 44100 and max(abs(value) for value in samples) > 300
        return samples

    def verify_audio(self, name, dry, gain):
        wet = self.render(name)
        assert len(wet) == len(dry), (len(wet), len(dry))
        # Leave the initial native control smoothing interval out of comparison.
        difference = max(abs(actual - original * gain) for actual, original in zip(wet[8192:], dry[8192:]))
        assert difference <= 3, (name, difference)
        return difference


def make_sample(path):
    # A deterministic PCM calibration signal, not a composition generator.
    samples = array.array("h", [8192, -4096] * 44100)
    if sys.byteorder != "little":
        samples.byteswap()
    with wave.open(str(path), "wb") as stream:
        stream.setnchannels(2)
        stream.setsampwidth(2)
        stream.setframerate(44100)
        stream.writeframes(samples.tobytes())


def verify_bank(session, kind, current_gain):
    page = session.call("plugin.program.list", {**SELECTOR, "offset": 0, "limit": 1})
    if kind == 2:
        assert page["total"] == 0 and page["current"] == -1
        session.gain(current_gain)
    else:
        assert page["total"] == 700 and page["current"] == 512, page
        session.program(0, .2)
        session.program(699, .8)
        session.program(512, current_gain)


def verify_parameter_pages(session):
    before = json.loads((session.studio.session / "session.json").read_text(encoding="utf-8"))
    ids, offset, pages = [], 0, 0
    while True:
        result = session.call("parameter.list", {**SELECTOR, "offset": offset, "limit": 2, "choices": False})
        assert result["offset"] == offset and len(result["parameters"]) <= 2, result
        ids.extend(item["id"] for item in result["parameters"])
        pages += 1
        if not result["hasMore"]:
            break
        assert result["nextOffset"] > offset, result
        offset = result["nextOffset"]
    assert len(ids) == result["total"] and len(set(ids)) == len(ids) and pages > 1
    nested, offset = [], 0
    while True:
        result = session.call("effect.inspect", {**SELECTOR, "parameterOffset": offset, "parameterLimit": 2, "choices": False})
        effect = result["effects"][0]
        nested.extend(item["id"] for item in effect["parameters"])
        page = effect["parameterPage"]
        if not page["hasMore"]:
            break
        assert page["nextOffset"] > offset, page
        offset = page["nextOffset"]
    assert nested == ids, (nested, ids)
    after = json.loads((session.studio.session / "session.json").read_text(encoding="utf-8"))
    assert before["history"] == after["history"] and before["revision"] == after["revision"]
    return {"parameters": len(ids), "pages": pages, "nativeIdsMatchNestedInspection": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lmms", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--kind", type=int, choices=(0, 1, 2), required=True)
    parser.add_argument("--worker-root", type=Path, required=True)
    parser.add_argument("--native-effect", type=Path, required=True)
    parser.add_argument("--vstbase", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary, fixture = args.lmms.resolve(), args.fixture.resolve()
    assert binary.is_file() and fixture.is_file(), "A built LMMS executable and native VST fixture are required"
    evidence = args.output.resolve() / f"kind-{args.kind}-{uuid.uuid4().hex}"
    evidence.mkdir(parents=True)
    # Native worker executables and plugin modules have different output
    # directories with multi-configuration generators. Use LMMS's existing
    # workspace plugin search path for exact copies of this build's modules.
    plugin_directory = evidence / "workspace/plugins"
    plugin_directory.mkdir(parents=True)
    for module in (args.native_effect.resolve(), args.vstbase.resolve()):
        assert module.is_file(), module
        shutil.copy2(module, plugin_directory / module.name)
    os.environ["LMMS_PLUGIN_DIR"] = str(args.worker_root.resolve())
    os.environ["LMMS_REMOTE_PLUGIN_TIMEOUT_MS"] = "10000"
    session_directory = evidence / "session"
    sample, project = evidence / "calibration.wav", evidence / "native-vst.mmp"
    fxp, fxb = evidence / "program.data", evidence / "bank.fxp"
    make_sample(sample)
    studios, comparisons = [], {}
    started = time.monotonic()
    result = {"status": "failed", "scope": "native LMMS process, VST fixture and durable project recovery"}
    try:
        first = Studio(binary, session_directory, evidence)
        studios.append(first)
        session = NativeSession(first, evidence, "initial")
        session.call("project.new", {"empty": True})
        session.call("track.add", {"type": "sample", "name": "VST recovery calibration"})
        session.call("clip.add", {"track": 0, "position": 0})
        session.call("sample.load", {"track": 0, "clip": 0, "path": str(sample)})
        dry = session.render("dry")
        # Classifying a short DLL or an attacker-controlled PE offset must not
        # crash the DAW or attach an inert effect as a successful native load.
        short_binary, offset_binary = evidence / "short.dll", evidence / "bad-offset.dll"
        short_binary.write_bytes(b"MZ")
        offset_binary.write_bytes(b"MZ" + b"\0" * 58 + b"\xff" * 4)
        for attributes in ({}, {"file": str(evidence / "missing.dll")},
                           {"file": str(short_binary)}, {"file": str(offset_binary)}):
            before = json.loads((session_directory / "session.json").read_text(encoding="utf-8"))
            session.call("effect.add", {"channel": 0, "plugin": "vsteffect", "attributes": attributes}, ok=False)
            after = json.loads((session_directory / "session.json").read_text(encoding="utf-8"))
            assert after["history"] == before["history"] and after["cursor"] == before["cursor"]
            assert session.call("effect.inspect", {"channel": 0})["effects"] == []
        session.call("effect.add", {"channel": 0, "plugin": "vsteffect", "attributes": {"file": str(fixture)}})
        inventory = session.call("effect.inspect", {"channel": 0})["effects"]
        assert len(inventory) == 1 and inventory[0]["nativePresetsSupported"] and inventory[0]["enabled"], inventory
        comparisons["parameterPages"] = verify_parameter_pages(session)
        if args.kind != 2:
            session.set_gain(.2)
            session.call("plugin.program.select", {**SELECTOR, "program": 699})
            session.set_gain(.8)
            session.call("plugin.program.select", {**SELECTOR, "program": 512})
        session.set_gain(.5)
        # A native automation target must survive serialized worker recreation.
        fault = session.control("Fault:x")["id"]
        session.call("automation.global.point", {"parameter": fault, "position": 0, "value": 0})
        session.call("plugin.preset.save", {**SELECTOR, "path": str(fxp), "format": "fxp"})
        session.call("plugin.preset.save", {**SELECTOR, "path": str(fxb), "format": "fxb"})
        assert session.call("plugin.preset.inspect", {**SELECTOR, "path": str(fxp)})["format"] == "fxp"
        assert session.call("plugin.preset.inspect", {**SELECTOR, "path": str(fxb)})["format"] == "fxb"
        if args.kind != 2:
            session.program(0, .2)
            session.set_gain(.3)
            session.program(699, .8)
            session.set_gain(.7)
            session.program(512, .5)
            gain_handle = session.control("Gain:x")["id"]
            session.call("plugin.preset.load", {**SELECTOR, "path": str(fxb)})
            assert session.control("Gain:x")["id"] == gain_handle
            verify_bank(session, args.kind, .5)
        session.set_gain(.25)
        gain_handle = session.control("Gain:x")["id"]
        imported_args = {**SELECTOR, "path": str(fxp)}
        first.call("native-preset-import", "plugin.preset.load", imported_args)
        session.gain(.5)
        assert session.control("Gain:x")["id"] == gain_handle
        assert session.control("Fault:x")["id"] == fault
        comparisons["import"] = session.verify_audio("import", dry, .5)
        session.call("session.undo")
        session.gain(.25)
        comparisons["undo"] = session.verify_audio("undo", dry, .25)
        assert first.call("native-preset-import", "plugin.preset.load", imported_args).get("replayed") is True
        session.gain(.25)
        session.call("session.redo")
        session.gain(.5)
        comparisons["redo"] = session.verify_audio("redo", dry, .5)
        verify_bank(session, args.kind, .5)
        session.verify_automation()
        session.call("effect.add", {"channel": 0, "plugin": "vsteffect", "attributes": {"file": str(offset_binary)}}, ok=False)
        assert len(session.call("effect.inspect", {"channel": 0})["effects"]) == 1
        session.gain(.5)
        session.verify_automation()
        comparisons["refusedPluginLoad"] = session.verify_audio("refused-plugin", dry, .5)
        session.call("project.save", {"path": str(project)})
        # Corrupt persisted plugin bytes: project loading must fail and restore
        # the previous native checkpoint, not accept a changed decoded payload.
        tree = ET.parse(project)
        encoding = "chunk" if args.kind == 1 else "nativepreset"
        nodes = [node for node in tree.iter() if encoding in node.attrib]
        assert len(nodes) == 1, (encoding, len(nodes))
        nodes[0].set(encoding, nodes[0].get(encoding) + "!")
        damaged = evidence / "corrupt-plugin-state.mmp"
        tree.write(damaged, encoding="utf-8", xml_declaration=True)
        rejected = session.call("project.load", {"path": str(damaged)}, ok=False)
        assert "Base64" in rejected.get("error", ""), rejected
        session.gain(.5)
        comparisons["rollback"] = session.verify_audio("rollback", dry, .5)
        # A failed native chunk capture must also restore the last acknowledged
        # state instead of publishing a parameter-only checkpoint.
        if args.kind == 1:
            rejected = session.call("parameter.set", {"parameter": session.control("Fault:x")["id"], "value": 1}, ok=False)
            assert "chunk" in rejected.get("error", "").lower(), rejected
            assert session.control("Fault:x")["value"] == 0
            session.gain(.5)
        first.shutdown()

        resumed = Studio(binary, session_directory, evidence)
        studios.append(resumed)
        session = NativeSession(resumed, evidence, "reopened")
        verify_bank(session, args.kind, .5)
        session.verify_automation()
        comparisons["reopen"] = session.verify_audio("state", dry, .5)
        session.set_gain(.6)
        resumed.kill()  # Abrupt exit only after the mutation was acknowledged.

        recovered = Studio(binary, session_directory, evidence)
        studios.append(recovered)
        session = NativeSession(recovered, evidence, "recovered")
        verify_bank(session, args.kind, .6)
        session.verify_automation()
        comparisons["crashRecovery"] = session.verify_audio("state", dry, .6)
        session.set_gain(.75)
        comparisons["continuedEditing"] = session.verify_audio("continued", dry, .75)
        session.call("project.load", {"path": str(project)})
        verify_bank(session, args.kind, .5)
        comparisons["projectReload"] = session.verify_audio("project", dry, .5)
        session.verify_automation()
        recovered.shutdown()
        result.update(status="passed", kind=args.kind, display="absent", comparisonsPcm=comparisons,
                      fixture=str(fixture), project=str(project), wallTimeSeconds=time.monotonic() - started,
                      thirdPartyCompatibilityVerified=False, autonomousMusicalWorkflowVerified=False)
    finally:
        for index, studio in enumerate(studios):
            if studio.process.poll() is None:
                studio.kill()
            (evidence / f"trace-{index}.json").write_text(json.dumps(studio.trace, indent=2), encoding="utf-8")
            (evidence / f"stderr-{index}.log").write_text("\n".join(studio.errors), encoding="utf-8")
        (evidence / "verification.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(evidence)


if __name__ == "__main__":
    main()
