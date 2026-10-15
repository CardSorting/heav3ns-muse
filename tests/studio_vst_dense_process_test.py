"""Verify a large native VST layout through the actual LMMS studio process.

This is test instrumentation. Native models, persistence, worker calls and DSP
remain inside LMMS. A successful run does not establish third-party compatibility
or autonomous musical judgment.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import sys
import time
import uuid
import xml.etree.ElementTree as ET

sys.dont_write_bytecode = True
from studio_process_test import Studio
from studio_vst_process_test import NativeSession, SELECTOR, make_sample


PARAMETERS = 65537
MIDDLE = PARAMETERS // 2
LAST = PARAMETERS - 1
WATCHED = {0, 1, MIDDLE, LAST}
NATIVE_NAME = re.compile(r"(?:^|>)(\d+): (Gain:x|Fault:x|P\d+)$")


class StreamingTrace:
    """Retain every native response without retaining all pages in Python RAM."""
    def __init__(self, path):
        self.stream = path.open("w", encoding="utf-8")

    def append(self, event):
        json.dump(event, self.stream, ensure_ascii=False, separators=(",", ":"))
        self.stream.write("\n")
        self.stream.flush()

    def close(self):
        self.stream.close()


def native_index(item):
    matched = NATIVE_NAME.search(item["name"])
    if not matched:
        return None  # Native effect bypass/wet/auto-quit controls have other names.
    index = int(matched[1])
    expected_name = "Gain:x" if index == 0 else "Fault:x" if index == 1 else f"P{index}"
    assert matched[2] == expected_name and 0 <= index < PARAMETERS, item
    return index


def expected_value(index, changes):
    return changes.get(index, .25 if index == 0 else 0.)


def signature(studio):
    state = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    return {key: state[key] for key in ("history", "cursor", "revision")}


class DenseSession(NativeSession):
    def __init__(self, studio, evidence, phase, positions=None):
        super().__init__(studio, evidence, phase)
        self.positions = dict(positions or {})

    def lookup(self, index):
        # Re-read the page after each restore. Never reuse the old model ID.
        result = self.call("parameter.list", {**SELECTOR, "offset": self.positions[index], "limit": 1, "choices": False})
        assert len(result["parameters"]) == 1, result
        item = result["parameters"][0]
        assert native_index(item) == index, (index, item)
        return item

    def control(self, name):
        assert name in {"Gain:x", "Fault:x"}, name
        return self.lookup(0 if name == "Gain:x" else 1)

    def check_values(self, changes):
        for index in WATCHED:
            item = self.lookup(index)
            expected = expected_value(index, changes)
            assert abs(item["value"] - expected) < 1e-6 and abs(item["manualValue"] - expected) < 1e-6, (index, item, expected)

    def check_automation(self):
        item = self.lookup(LAST)
        result = self.call("automation.global.inspect", {"parameter": item["id"], "position": 0})
        assert result["value"] == .875 and len(result["points"]) == 1, result
        assert [target["parameter"] for target in result["targets"]] == [item["id"]], result

    def resources(self):
        sample = self.call("studio.resources.inspect")
        assert sample["format"] == 1 and sample["scope"] == "lmms-process", sample
        assert sample["process"] == str(self.studio.process.pid), sample
        return sample

    def scan_all(self, changes):
        before = signature(self.studio)
        resource_before = self.resources()
        revision = self.studio.revision
        offset, pages, encoded_bytes, maximum_page_bytes = 0, 0, 0, 0
        total = None
        handles, indices = set(), set()
        identity_digest = hashlib.sha256()
        started = time.monotonic()
        while True:
            identifier = f"{self.phase}-inventory-{offset}"
            arguments = {**SELECTOR, "offset": offset, "limit": 1024, "choices": False}
            response = self.studio.call(identifier, "parameter.list", arguments, revision=revision)
            result = response["result"]
            assert result["offset"] == offset and response["revision"] == revision, response
            if total is None:
                total = result["total"]
            assert result["total"] == total and total >= PARAMETERS, result
            items = result["parameters"]
            size = len(json.dumps(items, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))
            assert 0 < len(items) <= 1024 and size <= 512 * 1024, (offset, len(items), size)
            encoded_bytes += size
            maximum_page_bytes = max(maximum_page_bytes, size)
            for relative, item in enumerate(items):
                assert item["id"] not in handles, item
                handles.add(item["id"])
                assert item["location"] == {"channel": 0, "effect": 0, "scope": "effect"}, item
                index = native_index(item)
                if index is None:
                    continue
                assert index not in indices, item
                indices.add(index)
                expected = expected_value(index, changes)
                assert abs(item["value"] - expected) < 1e-6 and abs(item["manualValue"] - expected) < 1e-6, (index, item, expected)
                identity_digest.update(f"{index}:{item['id']};".encode("ascii"))
                if index in WATCHED:
                    self.positions[index] = offset + relative
            if pages == 0:
                # Replay while the receipt is still within the bounded recent ledger.
                state = json.loads((self.studio.session / "session.json").read_text(encoding="utf-8"))
                receipt = next(item for item in state["requests"] if item["id"] == identifier)
                archive = receipt["archive"]
                stored = (self.studio.session / archive["path"]).read_bytes()
                assert len(stored) == archive["bytes"] and hashlib.sha256(stored).hexdigest() == archive["sha256"]
                replay = self.studio.call(identifier, "parameter.list", arguments, revision=revision)
                assert replay.get("replayed") is True and replay["result"] == result, replay
            pages += 1
            assert result["nextOffset"] == offset + len(items), result
            offset = result["nextOffset"]
            if not result["hasMore"]:
                break
        enumeration_elapsed = time.monotonic() - started
        assert offset == total and len(handles) == total and indices == set(range(PARAMETERS))
        assert pages > 1 and encoded_bytes > 16 * 1024 * 1024, (pages, encoded_bytes)
        for index in WATCHED:
            result = self.call("effect.inspect", {**SELECTOR, "parameterOffset": self.positions[index], "parameterLimit": 1, "choices": False})
            item = result["effects"][0]["parameters"][0]
            assert native_index(item) == index and item["id"] == self.lookup(index)["id"], item
        resource_after = self.resources()
        elapsed = time.monotonic() - started
        assert signature(self.studio) == before
        assert resource_before["instance"] == resource_after["instance"]
        assert resource_after["monotonicMs"] >= resource_before["monotonicMs"]
        cpu_keys = ("cpuUserMs", "cpuSystemMs")
        cpu_ms = None
        if all(sample[key] is not None for sample in (resource_before, resource_after) for key in cpu_keys):
            cpu_ms = sum(resource_after[key] - resource_before[key] for key in cpu_keys)
            assert cpu_ms >= 0
        return {"nativeParameters": len(indices), "totalControls": total, "pages": pages,
                "aggregateParameterBytes": encoded_bytes, "maximumPageBytes": maximum_page_bytes,
                "wallSeconds": elapsed, "enumerationWallSeconds": enumeration_elapsed, "lmmsCpuMs": cpu_ms,
                "nativeMeasurementIntervalMs": resource_after["monotonicMs"] - resource_before["monotonicMs"],
                "resourcesBefore": resource_before, "resourcesAfter": resource_after,
                "nativeIdentitySha256": identity_digest.hexdigest(), "firstPageReceiptReplayed": True}


def verify_project_and_make_legacy(project, legacy, changes):
    tree = ET.parse(project)
    nodes = [node for node in tree.iter() if node.get("numparams") == str(PARAMETERS) and "nativepreset" in node.attrib]
    assert len(nodes) == 1, len(nodes)
    settings = nodes[0]
    preset = base64.b64decode(settings.attrib["nativepreset"], validate=True)
    assert preset[:4] == b"CcnK" and preset[8:12] == b"FxCk" and len(preset) == 56 + PARAMETERS * 4
    assert struct.unpack_from(">I", preset, 24)[0] == PARAMETERS
    for index, (value,) in enumerate(struct.iter_unpack(">f", preset[56:])):
        assert abs(value - expected_value(index, changes)) < 1e-6, (index, value)
        legacy_value = settings.attrib[f"param{index}"].rsplit(":", 1)[1]
        assert abs(float(legacy_value) - expected_value(index, changes)) < 1e-6, (index, legacy_value)
    del settings.attrib["nativepreset"]
    tree.write(legacy, encoding="utf-8", xml_declaration=True)
    return {"nativePresetBytes": len(preset), "legacyParameters": PARAMETERS,
            "projectBytes": project.stat().st_size, "legacyProjectBytes": legacy.stat().st_size}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("lmms", "fixture", "worker-root", "native-effect", "vstbase", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    binary, fixture = args.lmms.resolve(), args.fixture.resolve()
    assert binary.is_file() and fixture.is_file(), "Built LMMS and native fixture 3 are required"
    evidence = args.output.resolve() / f"dense-{uuid.uuid4().hex}"
    plugin_directory = evidence / "workspace/plugins"
    plugin_directory.mkdir(parents=True)
    for module in (args.native_effect.resolve(), args.vstbase.resolve()):
        assert module.is_file(), module
        shutil.copy2(module, plugin_directory / module.name)
    os.environ["LMMS_PLUGIN_DIR"] = str(args.worker_root.resolve())
    os.environ["LMMS_REMOTE_PLUGIN_TIMEOUT_MS"] = "10000"
    session_directory = evidence / "session"
    sample, project, legacy = (evidence / name for name in ("calibration.wav", "dense.mmp", "dense-legacy.mmp"))
    make_sample(sample)
    studios, traces = [], []
    result = {"status": "failed", "scope": "actual LMMS process with native 65,537-parameter VST fixture",
              "thirdPartyCompatibilityVerified": False, "autonomousMusicalWorkflowVerified": False,
              "resourceScope": "LMMS process only; VST worker and Python harness memory/CPU excluded"}
    started = time.monotonic()

    def launch(phase, positions=None):
        studio = Studio(binary, session_directory, evidence)
        studios.append(studio)
        trace = StreamingTrace(evidence / f"trace-{phase}.jsonl")
        traces.append(trace)
        for event in studio.trace:
            trace.append(event)
        studio.trace = trace
        return DenseSession(studio, evidence, phase, positions)

    try:
        session = launch("initial")
        session.call("project.new", {"empty": True})
        session.call("track.add", {"type": "sample", "name": "Dense native VST calibration"})
        session.call("clip.add", {"track": 0, "position": 0})
        session.call("sample.load", {"track": 0, "clip": 0, "path": str(sample)})
        dry = session.render("dry")
        session.call("effect.add", {"channel": 0, "plugin": "vsteffect", "attributes": {"file": str(fixture)}})
        result["initialInventory"] = session.scan_all({})
        baseline = {0: .5, MIDDLE: .375, LAST: .875}
        for index, value in baseline.items():
            session.call("parameter.set", {"parameter": session.lookup(index)["id"], "value": value})
        session.call("automation.global.point", {"parameter": session.lookup(LAST)["id"], "position": 0, "value": .875})
        session.check_values(baseline)
        session.check_automation()
        result["initialPcmError"] = session.verify_audio("edited", dry, .5)
        edit_args = {"parameter": session.lookup(MIDDLE)["id"], "value": .625}
        session.studio.call("dense-middle-edit", "parameter.set", edit_args)
        session.call("session.undo")
        session.check_values(baseline)
        assert session.studio.call("dense-middle-edit", "parameter.set", edit_args).get("replayed") is True
        session.check_values(baseline)
        session.call("session.redo")
        baseline[MIDDLE] = .625
        session.check_values(baseline)
        session.check_automation()
        result["redoPcmError"] = session.verify_audio("redo", dry, .5)
        session.call("project.save", {"path": str(project)})
        result["projectState"] = verify_project_and_make_legacy(project, legacy, baseline)
        session.set_gain(.25)
        session.call("project.load", {"path": str(legacy)})
        session.check_values(baseline)
        session.check_automation()
        result["legacyRestorePcmError"] = session.verify_audio("legacy", dry, .5)
        positions = session.positions
        session.studio.shutdown()

        session = launch("reopened", positions)
        session.check_values(baseline)
        session.check_automation()
        result["reopenPcmError"] = session.verify_audio("reopen", dry, .5)
        session.call("parameter.set", {"parameter": session.lookup(MIDDLE)["id"], "value": .75})
        session.studio.kill()  # Only after the native mutation has been acknowledged.

        session = launch("recovered", positions)
        recovered_values = {**baseline, MIDDLE: .75}
        result["recoveredInventory"] = session.scan_all(recovered_values)
        session.check_automation()
        result["recoveredPcmError"] = session.verify_audio("recovered", dry, .5)
        session.set_gain(.75)
        result["continuedPcmError"] = session.verify_audio("continued", dry, .75)
        session.call("project.load", {"path": str(project)})
        session.check_values(baseline)
        session.check_automation()
        result["projectReloadPcmError"] = session.verify_audio("project", dry, .5)
        result["finalResources"] = session.resources()
        session.studio.shutdown()
        result.update(status="passed", display="absent", fixture=str(fixture), project=str(project),
                      wallTimeSeconds=time.monotonic() - started)
    except BaseException as error:
        result["error"] = f"{type(error).__name__}: {error}"
        raise
    finally:
        for index, studio in enumerate(studios):
            if studio.process.poll() is None:
                studio.kill()
            (evidence / f"stderr-{index}.log").write_text("\n".join(studio.errors), encoding="utf-8")
        for trace in traces:
            trace.close()
        (evidence / "verification.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(evidence)


if __name__ == "__main__":
    main()
