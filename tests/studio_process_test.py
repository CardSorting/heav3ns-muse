"""Exercise the LMMS executable itself without a display or GUI automation.

This is test instrumentation, not an agent runtime or substitute audio engine.
It fails if a full native binary is absent or if any asserted capability fails.
"""
from __future__ import annotations

import argparse
import array
import base64
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import math
import os
from pathlib import Path
import queue
import signal
import struct
import subprocess
import sys
import threading
import time
import wave
import xml.etree.ElementTree as ET


def asset_path(studio, value):
    return studio.session / "checkpoints" / value.removeprefix("local:") if value.startswith("local:") else Path(value)


def assert_owned_asset(studio, restored, original):
    owned, source = asset_path(studio, restored), asset_path(studio, original)
    assert owned.resolve().parent == (studio.session / "checkpoints/assets").resolve()
    actual = hashlib.sha256(owned.read_bytes()).hexdigest()
    assert owned.name.split(".")[0] == actual
    assert actual == hashlib.sha256(source.read_bytes()).hexdigest()


def assert_restored_media(studio, restored, original):
    assert_owned_asset(studio, restored["path"], original["path"])
    assert {key: value for key, value in restored.items() if key != "path"} == {
        key: value for key, value in original.items() if key != "path"
    }


class Studio:
    def __init__(self, binary: Path, session: Path, evidence: Path, startup_options=(), *, isolate_interrupts=False):
        self.session = session
        self.trace = []
        self.responses: queue.Queue = queue.Queue()
        self.errors = []
        config = evidence / "config.xml"
        (evidence / "workspace").mkdir(exist_ok=True)
        if not config.exists():
            root = ET.Element("lmms", version="1.3.0", configversion="999")
            ET.SubElement(root, "app", language="en", disablebackup="0")
            ET.SubElement(root, "paths", workingdir=str(evidence / "workspace"))
            ET.ElementTree(root).write(config, encoding="utf-8", xml_declaration=True)
        environment = os.environ.copy()
        environment.pop("DISPLAY", None)
        environment.pop("WAYLAND_DISPLAY", None)
        # No offscreen platform is needed: this mode must use QCoreApplication.
        environment.pop("QT_QPA_PLATFORM", None)
        self.process = subprocess.Popen(
            [str(binary), "studio", str(session), "--config", str(config), "--allowroot", *startup_options],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", env=environment,
            creationflags=subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" and isolate_interrupts else 0,
        )
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.stderr_reader = threading.Thread(target=self._read_errors, daemon=True)
        self.reader.start()
        self.stderr_reader.start()
        try:
            ready = self.receive()
            assert ready.get("event") == "ready", ready
            assert ready.get("applicationType") == "QCoreApplication", ready
            self.ready = ready
            self.revision = ready["revision"]
        except BaseException:
            self.kill()
            (evidence / "startup-failure.json").write_text(json.dumps(self.trace, indent=2), encoding="utf-8")
            (evidence / "startup-stderr.log").write_text("\n".join(self.errors), encoding="utf-8")
            raise

    def _read(self):
        for line in self.process.stdout:
            try:
                self.responses.put(json.loads(line))
            except json.JSONDecodeError:
                self.responses.put({"protocolError": line.rstrip()})
        self.responses.put({"eof": True, "exitCode": self.process.poll()})

    def _read_errors(self):
        for line in self.process.stderr:
            self.errors.append(line.rstrip())

    def receive(self, timeout=330):
        value = self.responses.get(timeout=timeout)
        self.trace.append({"response": value})
        assert "protocolError" not in value, value
        assert "eof" not in value, value
        return value

    def call(self, identifier, operation, args=None, *, ok=True, revision=None):
        request = {"id": identifier, "op": operation, "args": args or {}}
        if revision is not None:
            request["expectedRevision"] = revision
        self.trace.append({"request": request})
        self.process.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
        self.process.stdin.flush()
        response = self.receive()
        while response.get("event") == "agent":
            self.revision = response["revision"]
            response = self.receive()
        assert response.get("id") == identifier, response
        assert response.get("ok") is ok, response
        self.revision = response.get("revision", self.revision)
        return response

    def inspect_parameters(self, identifier, operation, args=None):
        """Collect real pages for subsystem assertions that need the complete inventory."""
        assert operation in {"parameter.list", "instrument.inspect"}
        arguments = dict(args or {})
        response = self.call(identifier, operation, arguments)
        result = dict(response["result"])
        parameters = list(result["parameters"])
        page = result["parameterPage"] if operation == "instrument.inspect" else result
        total = page["total"]
        while page["hasMore"]:
            offset = page["nextOffset"]
            assert offset > page["offset"], page
            following = self.call(f"{identifier}-page-{offset}", operation,
                                  {**arguments, "offset": offset}, revision=response["revision"])
            next_result = following["result"]
            page = next_result["parameterPage"] if operation == "instrument.inspect" else next_result
            assert page["total"] == total and page["offset"] == offset, page
            parameters.extend(next_result["parameters"])
        assert len({item["id"] for item in parameters}) == len(parameters)
        # The trace retains each actual response; this aggregate is test data.
        result["parameters"] = parameters
        result["testCollectedAllPages"] = True
        return {**response, "result": result}

    def chat_text(self, text):
        self.trace.append({"conversation": text})
        self.process.stdin.write(text + "\n")
        self.process.stdin.flush()
        response = self.receive()
        while response.get("event") == "agent":
            response = self.receive()
        assert response.get("ok") is True, response
        assert response["result"]["job"]["intent"] == text, response
        return response

    def wait_job(self, expected_status):
        while True:
            event = self.receive()
            assert event.get("event") == "agent", event
            self.revision = event["revision"]
            status = event["job"]["status"]
            if status not in {"running", "queued"}:
                assert status == expected_status, event
                return event

    def shutdown(self):
        self.call("shutdown-" + str(time.monotonic_ns()), "session.shutdown")
        self.process.stdin.close()
        assert self.process.wait(timeout=30) == 0
        self.reader.join(timeout=5)
        self.stderr_reader.join(timeout=5)

    def kill(self):
        self.process.kill()
        self.process.wait(timeout=30)
        self.reader.join(timeout=5)
        self.stderr_reader.join(timeout=5)


class ProviderFixture:
    """Deterministic protocol test double; never production musical reasoning.

    LMMS still owns all project edits, instrument synthesis, rendering and recovery.
    Fixture judgments prove audio submission, not listening quality or autonomy.
    """

    def __init__(self, output: Path):
        self.output = output
        self.phase = 0
        self.hold = False
        self.started = threading.Event()
        self.release = threading.Event()
        self.requests = []
        self.audio_observations = []
        self.errors = []
        fixture = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def do_POST(self):
                try:
                    length = int(self.headers["Content-Length"])
                    assert 0 < length <= 32 * 1024 * 1024
                    body = json.loads(self.rfile.read(length))
                    fixture.requests.append(body)
                    message = fixture.response(body)
                    encoded = json.dumps({"choices": [{
                        "finish_reason": "tool_calls" if message.get("tool_calls") else "stop",
                        "message": message,
                    }]}).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(len(encoded)))
                    self.end_headers()
                    self.wfile.write(encoded)
                except (BrokenPipeError, ConnectionResetError):
                    # Expected when a cancelled inference discards its late response.
                    pass
                except BaseException as error:
                    fixture.errors.append(repr(error))
                    self.send_error(500)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    @property
    def configuration(self):
        return {
            "protocol": "chat", "endpoint": f"http://127.0.0.1:{self.server.server_port}/chat",
            "model": "deterministic-test-fixture", "timeoutMs": 10000,
        }

    @staticmethod
    def call(identifier, name, args):
        return {"id": identifier, "type": "function", "function": {
            "name": name, "arguments": json.dumps(args),
        }}

    def response(self, body):
        content = body["messages"][-1]["content"]
        if isinstance(content, list):
            assert body["modalities"] == ["text"]
            waveform = base64.b64decode(content[1]["input_audio"]["data"], validate=True)
            with wave.open(io.BytesIO(waveform), "rb") as audio:
                assert audio.getsampwidth() == 2 and audio.getnframes() > 0
                self.audio_observations.append({
                    "frames": audio.getnframes(), "sampleRate": audio.getframerate(),
                    "channels": audio.getnchannels(), "bytes": len(waveform),
                })
            return {"role": "assistant", "content": "Fixture received actual audio; musical quality remains unverified."}
        names = {tool["function"]["name"] for tool in body["tools"]}
        assert {"note_add", "audio_render", "audio_listen", "agent_context_compact",
                "agent_history_list", "agent_history_read", "conversation_history", "agent_finish",
                "studio_capabilities", "studio_execute"} <= names
        assert len(body["tools"]) <= 128 and len(names) == len(body["tools"])
        assert body["store"] is False and body["parallel_tool_calls"] is False
        if self.hold:
            self.started.set()
            assert self.release.wait(timeout=15), "Cancellation test did not release the response"
            calls = [self.call("late-tempo", "song_set", {"tempo": 200})]
        else:
            production = [
                [self.call("chat-tempo", "studio_execute", {"op": "song.set", "args": {"tempo": 122}})],
                [self.call("chat-render", "studio_execute", {"op": "audio.render", "args": {"path": str(self.output), "bitDepth": 16}})],
                [self.call("chat-measure", "audio_analyze", {"path": str(self.output)}),
                 self.call("chat-listen", "audio_listen", {"path": str(self.output), "prompt": "Evaluate the rendered excerpt", "durationSeconds": 1})],
                [self.call("chat-finish", "agent_finish", {
                    "status": "complete", "summary": "Fixture task executed native edits, rendering and audio submission.",
                    "evidence": ["chat-tempo", "chat-render", "chat-measure", "chat-listen"],
                })],
            ]
            calls = production[self.phase]
            self.phase += 1
        return {"role": "assistant", "content": None, "tool_calls": calls}

    def close(self):
        self.release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)


def verify_native_samples(studio, evidence):
    """A known PCM fixture tests LMMS decoding, resampling, trims and mixing."""
    source = evidence / "sample-source.wav"
    rate = 22050
    pcm = array.array("h", [4096, 4096] * rate + [16384, 16384] * rate)
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(source), "wb") as encoded:
        encoded.setnchannels(2)
        encoded.setsampwidth(2)
        encoded.setframerate(rate)
        encoded.writeframes(pcm.tobytes())
    studio.call("sample-isolate", "track.edit", {"track": 0, "muted": True})
    studio.call("sample-track", "track.add", {"type": "sample", "name": "Native sample"})
    studio.call("sample-clip", "clip.add", {"track": 1, "position": 0, "name": "Trimmed source"})
    loaded = studio.call("sample-load", "sample.load", {"track": 1, "clip": 0, "path": str(source)})["result"]
    assert loaded["sourceFrames"] == rate * 2 and loaded["sampleRate"] == rate
    begin, end = rate // 4, rate // 4 + rate // 2
    studio.call("sample-trim", "sample.edit", {"track": 1, "clip": 0, "startFrame": begin, "endFrame": end})

    def render(identifier):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as decoded:
            assert decoded.getframerate() == 44100 and decoded.getnchannels() == 2
            actual = array.array("h", decoded.readframes(decoded.getnframes()))
            if sys.byteorder != "little":
                actual.byteswap()
            return actual

    baseline = render("sample-baseline")
    studio.call("sample-gain", "sample.edit", {"track": 1, "clip": 0, "gain": 0.5})
    half = render("sample-half")
    # Compare actual independently decoded render samples, avoiding stream startup.
    assert max(abs(value) for value in baseline[2048:32000]) > 300
    assert max(abs(expected * 0.5 - value) for expected, value in zip(baseline[2048:32000], half[2048:32000])) <= 3
    reversed_state = studio.call("sample-reverse", "sample.edit", {"track": 1, "clip": 0, "reversed": True})["result"]
    assert reversed_state["startFrame"] == rate * 2 - end
    assert reversed_state["endFrame"] == rate * 2 - begin
    reverse = render("sample-reversed")
    assert max(abs(expected - value) for expected, value in zip(half[2048:32000], reverse[2048:32000])) <= 3
    before = studio.call("sample-before-split", "project.inspect")["result"]["tracks"][1]["clips"][0]
    assert before["sample"] == reversed_state, "Rendering must preserve editable source settings"
    assert before["length"] == 48 and before["offset"] == 0
    studio.call("sample-split", "clip.split", {"track": 1, "clip": 0, "position": 24})
    split = render("sample-split-render")
    for begin_index, end_index in ((2048, 18000), (26000, 40000)):
        assert max(abs(expected - value) for expected, value in zip(half[begin_index:end_index], split[begin_index:end_index])) <= 3
    after = studio.call("sample-after-split", "project.inspect")["result"]["tracks"][1]["clips"]
    assert len(after) == 2 and after[1]["position"] == 24 and after[1]["offset"] == -24
    assert all(clip["sample"] == reversed_state for clip in after)

    studio.call("sample-copy-track", "track.add", {"type": "sample", "name": "Copied native sample"})
    for index, clip in enumerate(after):
        studio.call(f"sample-copy-{index}", "clip.clone", {
            "track": 1, "clip": index, "targetTrack": 2, "position": clip["position"],
        })
    studio.call("sample-copy-isolate", "track.edit", {"track": 1, "muted": True})
    copied_render = render("sample-copied-render")
    for begin_index, end_index in ((2048, 18000), (26000, 40000)):
        assert max(abs(expected - value) for expected, value in zip(split[begin_index:end_index], copied_render[begin_index:end_index])) <= 3
    studio.call("sample-copy-remove", "track.remove", {"track": 2})
    studio.call("sample-copy-restore", "track.edit", {"track": 1, "muted": False})

    parameters = studio.inspect_parameters("sample-parameters", "parameter.list")["result"]["parameters"]
    volumes = [p for p in parameters if p["location"] == {"container": "song", "track": 1}
               and p["name"].endswith("Volume")]
    assert len(volumes) == 1, "Discover the native sample volume model by its owning track"
    volume = volumes[0]["id"]
    studio.call("automation-track", "track.add", {"type": "automation", "name": "Native volume curve"})
    studio.call("automation-clip", "clip.add", {"track": 2, "position": 0})
    studio.call("automation-connect", "automation.connect", {"track": 2, "clip": 0, "parameter": volume})
    for tick, value in ((0, 50), (12, 100), (24, 25), (36, 100)):
        studio.call(f"automation-point-{tick}", "automation.point", {
            "track": 2, "clip": 0, "position": tick, "value": value, "progression": 0,
        })
    automated = render("sample-arrangement-automation")
    regions = ((2048, 8000, 0.5), (15000, 19000, 1), (26000, 30000, 0.25), (36000, 40000, 1))
    for start, stop, gain in regions:
        assert len(automated) >= stop and len(split) >= stop
        assert max(abs(expected * gain - value)
                   for expected, value in zip(split[start:stop], automated[start:stop])) <= 4
    arrangement = studio.call("automation-observation", "automation.inspect", {"track": 2, "clip": 0})["result"]
    studio.call("automation-mute", "track.edit", {"track": 2, "muted": True})
    absent = studio.call("global-before", "automation.global.inspect", {"parameter": volume})["result"]
    assert not absent["points"], "Inspection must not create a hidden global curve"
    for tick, value in ((0, 50), (12, 100), (24, 25), (36, 100)):
        studio.call(f"global-point-{tick}", "automation.global.point", {
            "parameter": volume, "position": tick, "value": value, "progression": 0,
        })
    global_render = render("sample-global-automation")
    for start, stop, gain in regions:
        assert len(global_render) >= stop
        assert max(abs(expected * gain - value)
                   for expected, value in zip(split[start:stop], global_render[start:stop])) <= 4
    global_curve = studio.call("global-observation", "automation.global.inspect", {"parameter": volume})["result"]
    studio.call("sample-failed-load", "sample.load", {"track": 1, "clip": 0, "path": str(evidence / "absent.wav")}, ok=False)
    studio.call("sample-mute", "track.edit", {"track": 1, "muted": True})
    studio.call("lead-restore", "track.edit", {"track": 0, "muted": False})
    return {"fixtureSampleRate": rate, "outputSampleRate": 44100, "sourceWindow": reversed_state,
            "arrangementCurve": arrangement, "globalCurve": global_curve,
            "verification": "native decoded renders; gain, reversal, split and arrangement/global automation"}


def verify_native_detuning(studio, evidence):
    """Measure pitch scaling in LMMS's actual bundled instrument output."""
    def render_and_measure(identifier):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as decoded:
            assert decoded.getnchannels() == 2 and decoded.getsampwidth() == 2
            rate = decoded.getframerate()
            pcm = array.array("h", decoded.readframes(decoded.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
        # The first note lasts 0.5 seconds. Avoid its attack and release.
        samples = pcm[int(rate * 0.08) * 2:int(rate * 0.4) * 2:2]
        assert samples and max(abs(value) for value in samples) > 300
        crossings = [i for i in range(len(samples) - 1) if samples[i] <= 0 < samples[i + 1]]
        assert len(crossings) >= 8, "The native instrument must produce measurable periodic audio"
        return rate * (len(crossings) - 1) / (crossings[-1] - crossings[0])

    baseline = render_and_measure("detuning-baseline")
    studio.call("detuning-octave", "note.detuning.point", {
        "track": 0, "clip": 0, "note": 0, "position": 0, "value": 12, "progression": 1,
    })
    octave = render_and_measure("detuning-octave-render")
    assert abs(octave / baseline - 2) < 0.06, "Native detuning must double the rendered pitch"
    curve = studio.call("detuning-observation", "note.detuning.inspect", {
        "track": 0, "clip": 0, "note": 0, "position": 12,
    })["result"]
    assert curve["hasAutomation"] and curve["value"] == 12
    return {"baselineCrossingFrequency": baseline, "octaveCrossingFrequency": octave,
            "curve": curve, "verification": "actual native oscillator WAV pitch ratio; technical measurement"}


def verify_native_resources(studio, evidence):
    """Exercise bundled plugins, checked decoding and native arpeggio DSP."""
    source = evidence / "sample-source.wav"
    waveform = evidence / "oscillator-cycle.wav"
    corrupt = evidence / "corrupt-resource.wav"
    corrupt.write_bytes(b"invalid waveform")
    # One calibration cycle is a decoder fixture. LMMS still synthesizes notes.
    cycle = array.array("h", [16384, 16384] * 128 + [-16384, -16384] * 128)
    if sys.byteorder != "little":
        cycle.byteswap()
    with wave.open(str(waveform), "wb") as encoded:
        encoded.setnchannels(2)
        encoded.setsampwidth(2)
        encoded.setframerate(22050)
        encoded.writeframes(cycle.tobytes())

    def render(identifier):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as decoded:
            assert decoded.getframerate() == 44100 and decoded.getnchannels() == 2
            assert decoded.getsampwidth() == 2
            pcm = array.array("h", decoded.readframes(decoded.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
        assert len(pcm) >= 32000
        return pcm

    def inspect(identifier, track):
        return studio.inspect_parameters(identifier, "instrument.inspect", {"track": track})["result"]

    def named_parameter(instrument, name):
        matches = [p for p in instrument["parameters"] if p["name"].endswith(name)]
        assert len(matches) == 1, (name, matches)
        return matches[0]

    def resource(instrument, key):
        matches = [item for item in instrument["resources"] if item["key"] == key]
        assert len(matches) == 1, (key, matches)
        return matches[0]

    original = inspect("resource-synth-discovery", 0)
    assert len(original["resources"]) == 3
    selector = resource(original, "oscillator1")["selectionParameter"]
    selection = studio.call("resource-selector", "parameter.inspect", {"parameter": selector})["result"]
    assert selection["kind"] == "integer" and len(selection["choices"]) == 8
    assert named_parameter(original, "Osc 1 anti-aliasing")["kind"] == "boolean"
    baseline = render("resource-synth-baseline")
    studio.call("resource-synth-wave", "instrument.resource.set", {
        "track": 0, "resource": "oscillator1", "path": str(waveform), "select": True,
    })
    user_wave = render("resource-synth-user-wave")
    waveform_difference = sum(abs(a - b) for a, b in zip(baseline[2048:32000], user_wave[2048:32000])) / (32000 - 2048)
    assert max(abs(value) for value in user_wave[2048:32000]) > 300
    assert waveform_difference > 20, "The loaded native waveform must change actual synthesis"
    for identifier, key, path in (("resource-unknown", "unknown", waveform),
                                  ("resource-corrupt", "oscillator1", corrupt)):
        studio.call(identifier, "instrument.resource.set", {
            "track": 0, "resource": key, "path": str(path), "select": True,
        }, ok=False)
    # Runtime rollback may replace native model handles. Discover them again.
    intact = inspect("resource-synth-after-failure", 0)
    synth_resource = resource(intact, "oscillator1")
    assert_owned_asset(studio, synth_resource["path"], str(waveform))
    assert synth_resource["selected"]
    preserved_wave = render("resource-synth-preserved")
    assert max(abs(a - b) for a, b in zip(user_wave[2048:32000], preserved_wave[2048:32000])) <= 3

    studio.call("resource-isolate-sampler", "track.edit", {"track": 0, "muted": True})
    studio.call("resource-sampler-track", "track.add", {"type": "instrument", "name": "Native sampler"})
    studio.call("resource-sampler-plugin", "instrument.load", {"track": 3, "instrument": "audiofileprocessor"})
    studio.call("resource-sampler-clip", "clip.add", {"track": 3, "position": 0})
    studio.call("resource-sampler-note", "note.add", {"track": 3, "clip": 0, "position": 0,
                "length": 48, "key": 69, "volume": 100})
    studio.call("resource-sampler-sample", "instrument.resource.set", {
        "track": 3, "resource": "sample", "path": str(source),
    })
    sampler = inspect("resource-sampler-discovery", 3)
    assert "wav" in resource(sampler, "sample")["fileTypes"]
    assert len(named_parameter(sampler, "Loop mode")["choices"]) == 3
    assert sampler["name"] == "Native sampler", "Loading a resource must preserve a user-named track"
    sampler_baseline = render("resource-sampler-baseline")
    assert max(abs(value) for value in sampler_baseline[2048:32000]) > 300
    studio.call("resource-sampler-gain", "parameter.set", {
        "parameter": named_parameter(sampler, "Amplify")["id"], "value": 50,
    })
    sampler_half = render("resource-sampler-half")
    assert max(abs(expected * 0.5 - value)
               for expected, value in zip(sampler_baseline[2048:32000], sampler_half[2048:32000])) <= 3
    studio.call("resource-sampler-corrupt", "instrument.resource.set", {
        "track": 3, "resource": "sample", "path": str(corrupt),
    }, ok=False)
    sampler_preserved = render("resource-sampler-preserved")
    assert max(abs(a - b) for a, b in zip(sampler_half[2048:32000], sampler_preserved[2048:32000])) <= 3
    studio.call("resource-sampler-clear", "instrument.resource.set", {"track": 3, "resource": "sample", "path": ""})
    silent = render("resource-sampler-cleared")
    assert max(abs(value) for value in silent) == 0, "Clearing the native sample must remove its audio"
    studio.call("resource-sampler-restore", "instrument.resource.set", {"track": 3, "resource": "sample", "path": str(source)})
    studio.call("resource-sampler-mute", "track.edit", {"track": 3, "muted": True})
    studio.call("resource-lead-restore", "track.edit", {"track": 0, "muted": False})

    arp = inspect("resource-arp-discovery", 0)
    major = next(table for table in arp["nativeChordTables"] if table["semitones"] == [0, 4, 7])
    arp_type = named_parameter(arp, "Arpeggio type")
    assert arp_type["choices"][major["index"]]["name"] == major["name"]
    studio.call("resource-arp-chord", "parameter.set", {"parameter": arp_type["id"], "value": major["index"]})
    studio.call("resource-arp-enable", "parameter.set", {"parameter": named_parameter(arp, "Arpeggio enabled")["id"], "value": 1})
    arpeggiated = render("resource-native-arpeggio")
    arpeggio_difference = sum(abs(a - b) for a, b in zip(user_wave[2048:32000], arpeggiated[2048:32000])) / (32000 - 2048)
    assert max(abs(value) for value in arpeggiated[2048:32000]) > 300
    assert arpeggio_difference > 20, "Native arpeggio selection must change rendered notes"
    assert inspect("resource-arp-observation", 0)["arpeggioEnabled"]

    controller = studio.call("resource-lfo-create", "controller.add", {"type": "lfo", "name": "Native waveform"})["result"]["controller"]
    lfo = studio.call("resource-lfo-wave", "controller.waveform.set", {
        "controller": controller, "path": str(waveform), "select": True,
    })["result"]
    assert lfo["userWave"]["frames"] == 256 and lfo["userWave"]["sampleRate"] == 22050
    accepted_wave, accepted_sample = waveform.read_bytes(), source.read_bytes()
    try:
        waveform.write_bytes(b"Original waveform replaced after native loading")
        source.unlink()
        copied = studio.call("resource-clone-sampler", "track.clone", {"track": 3})["result"]["track"]
        studio.call("resource-clone-isolate", "track.edit", {"track": 0, "muted": True})
        studio.call("resource-clone-sampler-unmute", "track.edit", {"track": copied, "muted": False})
        cloned_sampler = render("resource-cloned-sampler-render")
        assert max(abs(a - b) for a, b in zip(sampler_half[2048:32000], cloned_sampler[2048:32000])) <= 3
        studio.call("resource-clone-sampler-remove", "track.remove", {"track": copied})
        studio.call("resource-clone-lead-unmute", "track.edit", {"track": 0, "muted": False})
        copied = studio.call("resource-clone-oscillator", "track.clone", {"track": 0})["result"]["track"]
        studio.call("resource-clone-source-mute", "track.edit", {"track": 0, "muted": True})
        cloned_oscillator = render("resource-cloned-oscillator-render")
        assert max(abs(a - b) for a, b in zip(arpeggiated[2048:32000], cloned_oscillator[2048:32000])) <= 3
        studio.call("resource-clone-oscillator-remove", "track.remove", {"track": copied})
        studio.call("resource-clone-source-unmute", "track.edit", {"track": 0, "muted": False})
        bundled = studio.call("resource-accepted-bundle", "project.save", {
            "path": str(evidence / "native-sample-portable.mmp"), "withResources": True,
        })["result"]
        bundle_path = Path(bundled["path"])
        project = ET.parse(bundle_path)
        for tag, attribute, expected in (
                ("sampleclip", "src", accepted_sample),
                ("audiofileprocessor", "src", accepted_sample),
                ("tripleoscillator", "userwavefile0", accepted_wave),
                ("lfocontroller", "userwavefile", accepted_wave)):
            nodes = project.findall(".//" + tag)
            assert nodes, tag
            for node in nodes:
                reference = node.get(attribute, "")
                assert reference.startswith("local:resources/"), (tag, reference)
                assert (bundle_path.parent / reference.removeprefix("local:")).read_bytes() == expected
        accepted_render = render("resource-accepted-input-render")
        assert max(abs(a - b) for a, b in zip(arpeggiated[2048:32000], accepted_render[2048:32000])) <= 3
    finally:
        waveform.write_bytes(accepted_wave)
        source.write_bytes(accepted_sample)
    studio.call("resource-lfo-corrupt", "controller.waveform.set", {"controller": controller, "path": str(corrupt)}, ok=False)
    rack = studio.call("resource-lfo-observation", "controller.list")["result"]["controllers"]
    assert_restored_media(studio, rack[controller]["userWave"], lfo["userWave"])
    return {"waveformPath": str(waveform), "samplePath": str(source), "waveformSelection": synth_resource["selectionValue"],
            "waveformMeanDifferencePcm": waveform_difference, "arpeggioMeanDifferencePcm": arpeggio_difference,
            "arpeggioChord": major, "samplerAmplify": 50, "controller": controller, "lfoWave": lfo["userWave"],
            "lfoSelection": lfo["wave"], "acceptedInputBundle": str(bundle_path),
            "verification": "actual native TripleOscillator/AFP/arpeggio WAVs; accepted resource clones and bundle, recovery and failure preservation"}


def verify_native_tuning(studio, evidence):
    """Measure native tuning in bundled oscillator audio, including a silent hole."""
    studio.call("tuning-scale", "tuning.scale.set", {"slot": 4, "name": "Native pitch calibration", "intervals": "3/2\n2/1"})
    mapping = "-1\n0\nx\n1"
    studio.call("tuning-map", "tuning.keymap.set", {"slot": 4, "name": "Native signed mapping", "mapping": mapping,
                "middleKey": 60, "baseKey": 61, "baseFrequency": 300, "octaveDegree": 2})
    studio.call("tuning-isolate", "track.edit", {"track": 0, "muted": True})
    studio.call("tuning-track", "track.add", {"type": "instrument", "name": "Native tuning calibration"})
    studio.call("tuning-plugin", "instrument.load", {"track": 4, "instrument": "tripleoscillator"})
    studio.call("tuning-clip", "clip.add", {"track": 4, "position": 0})
    studio.call("tuning-note", "note.add", {"track": 4, "clip": 0, "position": 0, "length": 48, "key": 64, "volume": 100})
    instrument = studio.inspect_parameters("tuning-instrument", "instrument.inspect", {"track": 4})["result"]
    for name, value in (("Selected scale", 4), ("Selected keyboard mapping", 4), ("Microtuner on / off", 1)):
        matches = [p for p in instrument["parameters"] if p["name"].endswith(name)]
        assert len(matches) == 1, (name, matches)
        studio.call("tuning-parameter-" + str(value) + "-" + name, "parameter.set", {"parameter": matches[0]["id"], "value": value})

    def render(identifier, *, silent=False):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as decoded:
            assert decoded.getnchannels() == 2 and decoded.getsampwidth() == 2
            rate = decoded.getframerate()
            assert rate == 44100
            pcm = array.array("h", decoded.readframes(decoded.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
        assert len(pcm) >= 32000
        if silent:
            assert max(abs(value) for value in pcm) == 0, "An unmapped native note must emit neither instrument audio nor residual buffers"
            return 0
        samples = pcm[int(rate * 0.08) * 2:int(rate * 0.4) * 2:2]
        assert samples and max(abs(value) for value in samples) > 300
        crossings = [i for i in range(len(samples) - 1) if samples[i] <= 0 < samples[i + 1]]
        assert len(crossings) >= 8
        return rate * (len(crossings) - 1) / (crossings[-1] - crossings[0])

    preview = studio.call("tuning-initial-frequency", "instrument.tuning.inspect", {"track": 4, "key": 64})["result"]
    assert preview["keys"][0]["frequency"] == 450
    ordinary = render("tuning-ordinary-period")
    studio.call("tuning-formal-period", "tuning.keymap.set", {"slot": 4, "name": "Native signed mapping", "mapping": mapping, "octaveDegree": 4})
    preview = studio.call("tuning-formal-frequency", "instrument.tuning.inspect", {"track": 4, "key": 64})["result"]
    assert preview["keys"][0]["frequency"] == 900
    formal = render("tuning-double-period")
    assert abs(formal / ordinary - 2) < 0.06, "Native formal octave must double actual oscillator output frequency"
    studio.call("tuning-hole-key", "note.edit", {"track": 4, "clip": 0, "note": 0, "key": 62})
    hole = studio.call("tuning-hole-frequency", "instrument.tuning.inspect", {"track": 4, "key": 62})["result"]["keys"][0]
    assert hole["mapped"] is False and hole["frequency"] == 0
    render("tuning-unmapped-key", silent=True)
    studio.call("tuning-negative-key", "note.edit", {"track": 4, "clip": 0, "note": 0, "key": 60})
    negative = studio.call("tuning-negative-frequency", "instrument.tuning.inspect", {"track": 4, "key": 60})["result"]["keys"][0]
    assert negative["mapped"] is True and negative["frequency"] == 225
    signed = render("tuning-negative-degree")
    assert abs(signed / formal - 0.25) < 0.03, "Mapped degree -1 must remain audible at its native ratio"

    scale_path, map_path = evidence / "calibration.scl", evidence / "calibration.kbm"
    studio.call("tuning-scale-export", "tuning.scale.save", {"slot": 4, "path": str(scale_path)})
    studio.call("tuning-map-export", "tuning.keymap.save", {"slot": 4, "path": str(map_path)})
    assert scale_path.is_file() and map_path.is_file()
    scale = studio.call("tuning-scale-observation", "tuning.scale.inspect", {"slot": 4})["result"]
    keymap = studio.call("tuning-map-observation", "tuning.keymap.inspect", {"slot": 4})["result"]
    assert keymap["entries"] == [-1, 0, None, 1] and keymap["octaveDegree"] == 4
    copied_scale = studio.call("tuning-scale-import", "tuning.scale.load", {"slot": 5, "path": str(scale_path)})["result"]
    copied_map = studio.call("tuning-map-import", "tuning.keymap.load", {"slot": 5, "path": str(map_path)})["result"]
    assert copied_scale["intervals"] == scale["intervals"]
    for field in ("entries", "octaveDegree", "baseFrequency", "name"):
        assert copied_map[field] == keymap[field]
    bad_scale = evidence / "truncated-calibration.scl"
    bad_scale.write_text("Incomplete\n3\n2/1\n", encoding="utf-8")
    studio.call("tuning-invalid-import", "tuning.scale.load", {"slot": 4, "path": str(bad_scale)}, ok=False)
    after_failure = studio.call("tuning-after-invalid-import", "tuning.scale.inspect", {"slot": 4})["result"]
    assert after_failure == scale
    studio.call("tuning-mute-calibration", "track.edit", {"track": 4, "muted": True})
    studio.call("tuning-restore-lead", "track.edit", {"track": 0, "muted": False})
    return {"scale": scale, "keymap": keymap, "ordinaryCrossingFrequency": ordinary, "formalCrossingFrequency": formal,
            "negativeCrossingFrequency": signed, "unmappedKey": hole, "negativeKey": negative,
            "verification": "actual native oscillator WAV pitch ratios and unmapped silence; native Scala IO and rollback"}


def measure_native_soundfont_wav(path, *, silent=False, stereo=False):
    """Measure exported PCM independently of the native SoundFont implementation."""
    with wave.open(str(path), "rb") as decoded:
        assert decoded.getnchannels() == 2 and decoded.getsampwidth() == 2
        rate = decoded.getframerate()
        assert rate == 44100
        pcm = array.array("h", decoded.readframes(decoded.getnframes()))
        if sys.byteorder != "little":
            pcm.byteswap()
    assert len(pcm) >= 40000
    if silent:
        assert max(abs(value) for value in pcm) == 0, "Cleared or unmapped native SF2 notes must produce silence"
        return 0
    start, stop = int(rate * 0.08) * 2, int(rate * 0.4) * 2
    left, right = pcm[start:stop:2], pcm[start + 1:stop:2]
    left_rms = math.sqrt(sum(value * value for value in left) / len(left))
    right_rms = math.sqrt(sum(value * value for value in right) / len(right))
    excerpt = right if stereo and right_rms > left_rms else left
    assert max(abs(value) for value in excerpt) > 300
    crossings = [i for i in range(len(excerpt) - 1) if excerpt[i] <= 0 < excerpt[i + 1]]
    assert len(crossings) > 10
    pitch = rate * (len(crossings) - 1) / (crossings[-1] - crossings[0])
    return (pitch, left_rms, right_rms) if stereo else pitch


def verify_native_soundbanks(studio, evidence):
    """Use real native bank loading and rendering, never a replacement synthesizer."""
    source = Path(__file__).parent / "fixtures" / "native-calibration.sf2"
    bank_path = evidence / "音 native-bank.sf2"
    bank_path.write_bytes(source.read_bytes())
    studio.call("bank-isolate", "track.edit", {"track": 0, "muted": True})
    studio.call("bank-track", "track.add", {"type": "instrument"})
    studio.call("bank-player", "instrument.load", {"track": 5, "instrument": "sf2player"})
    studio.call("bank-name", "track.edit", {"track": 5, "name": "Native bank calibration"})
    studio.call("bank-file", "instrument.resource.set", {"track": 5, "resource": "soundfont", "path": str(bank_path)})
    observation = studio.inspect_parameters("bank-inspection", "instrument.inspect", {"track": 5})["result"]
    assert observation["name"] == "Native bank calibration" and observation["programCount"] == 2
    available = studio.call("bank-programs", "instrument.program.list", {"track": 5})["result"]
    assert [(item["bank"], item["program"], item["name"]) for item in available["programs"]] == [
        (0, 0, "Native base"), (1024, 7, "Native octave")]
    page = studio.call("bank-page", "instrument.program.list", {"track": 5, "offset": 1, "limit": 1})["result"]
    assert page["programs"] == available["programs"][1:] and not page["hasMore"]
    filtered = studio.call("bank-filter", "instrument.program.list", {"track": 5, "bank": 1024, "query": "OCTAVE"})["result"]
    assert filtered["programs"] == available["programs"][1:]
    studio.call("bank-clip", "clip.add", {"track": 5, "position": 0})
    studio.call("bank-note", "note.add", {"track": 5, "clip": 0, "position": 0, "length": 48, "key": 69, "volume": 100})

    def render(identifier, *, silent=False, stereo=False):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        return measure_native_soundfont_wav(path, silent=silent, stereo=stereo)

    base = render("bank-base-audio")
    assert abs(base - 440) < 3
    selected = studio.call("bank-octave", "instrument.program.select", {"track": 5, "bank": 1024, "program": 7})["result"]
    assert selected["name"] == "Native octave" and selected["available"]
    octave = render("bank-octave-audio")
    assert abs(octave / base - 2) < 0.02
    studio.call("bank-base-program", "instrument.program.select", {"track": 5, "bank": 0, "program": 0})

    parameters = studio.inspect_parameters("bank-tuning-parameters", "instrument.inspect", {"track": 5})["result"]["parameters"]

    def set_parameter(name, value, identifier):
        matches = [item for item in parameters if item["name"].endswith(name)]
        assert len(matches) == 1, (name, matches)
        studio.call(identifier, "parameter.set", {"parameter": matches[0]["id"], "value": value})

    set_parameter("Selected scale", 4, "bank-scale")
    set_parameter("Selected keyboard mapping", 4, "bank-map")
    set_parameter("Microtuner on / off", 1, "bank-tuner-enable")
    studio.call("bank-reference-note", "note.edit", {"track": 5, "clip": 0, "note": 0, "key": 61})
    tuned = render("bank-tuned-reference")
    assert abs(tuned - 300) < 3
    set_parameter("Base note", 57, "bank-base-shift")
    shifted = render("bank-tuned-shifted-base")
    assert abs(shifted - tuned) < 3, "Imported tuning must retain its pitch when SF2's MIDI base key shifts"
    set_parameter("Import key range", 0, "bank-user-reference")
    user_reference = studio.call("bank-user-pitch", "instrument.tuning.inspect", {"track": 5, "key": 61})["result"]["keys"][0]
    assert user_reference["frequency"] == 1200
    user_tuned = render("bank-tuned-user-reference")
    assert abs(user_tuned / tuned - 4) < 0.04
    studio.call("bank-unmapped-note", "note.edit", {"track": 5, "clip": 0, "note": 0, "key": 62})
    render("bank-unmapped-silence", silent=True)
    set_parameter("Microtuner on / off", 0, "bank-tuner-disable")
    set_parameter("Base note", 69, "bank-base-restore")
    studio.call("bank-note-restore", "note.edit", {"track": 5, "clip": 0, "note": 0, "key": 69})

    corrupt = evidence / "invalid-native-bank.sf2"
    corrupt.write_bytes(b"This is not a SoundFont")
    studio.call("bank-reject-file", "instrument.resource.set", {"track": 5, "resource": "soundfont", "path": str(corrupt)}, ok=False)
    after_failure = studio.call("bank-observe-after-file-failure", "instrument.program.list", {"track": 5})["result"]
    assert after_failure["programs"] == available["programs"] and after_failure["selection"]["program"] == 0
    studio.call("bank-reject-program", "instrument.program.select", {"track": 5, "bank": 1024, "program": 8}, ok=False)
    survived = render("bank-audio-after-rejected-program")
    assert abs(survived - base) < 3
    studio.call("bank-clear", "instrument.resource.set", {"track": 5, "resource": "soundfont", "path": ""})
    cleared = studio.call("bank-empty-programs", "instrument.program.list", {"track": 5})["result"]
    assert cleared["total"] == 0 and not cleared["selection"]["available"]
    render("bank-cleared-silence", silent=True)
    studio.call("bank-restore-file", "instrument.resource.set", {"track": 5, "resource": "soundfont", "path": str(bank_path)})
    selected = studio.call("bank-restore-program", "instrument.program.select", {"track": 5, "bank": 1024, "program": 7})["result"]
    layered_path = evidence / "native-layered.sf2"
    layered_path.write_bytes(source.with_name("native-layered.sf2").read_bytes())
    studio.call("bank-layered-file", "instrument.resource.set", {"track": 5, "resource": "soundfont", "path": str(layered_path)})
    studio.call("bank-layered-expression", "note.detuning.point", {"track": 5, "clip": 0, "note": 0, "position": 0, "value": 12})
    studio.call("bank-layered-right", "note.edit", {"track": 5, "clip": 0, "note": 0, "panning": 100, "volume": 50})
    right_pitch, right_left, right_right = render("bank-layered-right-audio", stereo=True)
    assert abs(right_pitch - 880) < 6 and right_left < right_right / 100
    studio.call("bank-layered-left", "note.edit", {"track": 5, "clip": 0, "note": 0, "panning": -100})
    left_pitch, left_left, left_right = render("bank-layered-left-audio", stereo=True)
    assert abs(left_pitch - 880) < 6 and left_right < left_left / 100
    studio.call("bank-layered-reset-detuning", "note.detuning.clear", {"track": 5, "clip": 0, "note": 0})
    studio.call("bank-layered-reset-note", "note.edit", {"track": 5, "clip": 0, "note": 0, "panning": 0, "volume": 100})
    studio.call("bank-original-file", "instrument.resource.set", {"track": 5, "resource": "soundfont", "path": str(bank_path)})
    studio.call("bank-original-program", "instrument.program.select", {"track": 5, "bank": 1024, "program": 7})
    accepted_bank = bank_path.read_bytes()
    bank_path.write_bytes(b"Original replaced after native loading")
    try:
        bundled = studio.call("bank-owned-bundle", "project.save", {
            "path": str(evidence / "native-bank-portable.mmp"), "withResources": True,
        })["result"]
        bundle_path = Path(bundled["path"])
        bank_nodes = ET.parse(bundle_path).findall(".//sf2player")
        assert len(bank_nodes) == 1
        reference = bank_nodes[0].get("src")
        assert reference == "local:resources/" + bank_path.name
        bundled_bank = bundle_path.parent / reference.removeprefix("local:")
        assert bundled_bank.read_bytes() == accepted_bank
        assert abs(render("bank-preserved-source-audio") - 880) < 6
    finally:
        bank_path.write_bytes(accepted_bank)
    studio.call("bank-mute", "track.edit", {"track": 5, "muted": True})
    studio.call("gig-track", "track.add", {"type": "instrument"})
    studio.call("gig-player", "instrument.load", {"track": 6, "instrument": "gigplayer"})
    studio.call("gig-reject-file", "instrument.resource.set", {"track": 6, "resource": "bank", "path": str(corrupt)}, ok=False)
    studio.call("gig-clear", "instrument.resource.set", {"track": 6, "resource": "bank", "path": ""})
    gig = studio.inspect_parameters("gig-inspection", "instrument.inspect", {"track": 6})["result"]
    assert gig["programCount"] == 0 and gig["resources"][0]["path"] == ""
    studio.call("gig-mute", "track.edit", {"track": 6, "muted": True})
    studio.call("bank-restore-lead", "track.edit", {"track": 0, "muted": False})
    return {"path": str(bank_path), "sha256": hashlib.sha256(bank_path.read_bytes()).hexdigest(),
            "programs": available["programs"], "selection": selected, "baseFrequency": base, "octaveFrequency": octave,
            "tunedFrequency": tuned, "shiftedBaseFrequency": shifted, "userReferenceFrequency": user_tuned,
            "bundle": {"path": str(bundle_path), "assetSha256": hashlib.sha256(bundled_bank.read_bytes()).hexdigest()},
            "verification": "actual native SF2 WAV pitch/program/tuning comparisons, six-layer panning/detuning, accepted-source bundling, clearing and failure recovery",
            "gigVerification": "native construction, rejected invalid input and empty clearing only; valid GIG sample playback unverified"}


def measure_native_patch_wav(path):
    """Decode LMMS output independently; this measures PCM, not musical quality."""
    with wave.open(str(path), "rb") as decoded:
        rate = decoded.getframerate()
        assert rate == 44100 and decoded.getnchannels() == 2 and decoded.getsampwidth() == 2
        pcm = array.array("h", decoded.readframes(decoded.getnframes()))
        if sys.byteorder != "little":
            pcm.byteswap()
    assert len(pcm) >= int(rate * 1.4) * 2
    peak = max(abs(value) for value in pcm)
    if peak == 0:
        return {"frequency": 0, "peak": 0, "tailPeak": 0}
    excerpt = pcm[int(rate * 0.08) * 2:int(rate * 0.4) * 2:2]
    assert max(abs(value) for value in excerpt) > 300
    crossings = [index for index in range(len(excerpt) - 1) if excerpt[index] <= 0 < excerpt[index + 1]]
    assert len(crossings) > 10
    pitch = rate * (len(crossings) - 1) / (crossings[-1] - crossings[0])
    tail = pcm[int(rate * 1.1) * 2:int(rate * 1.4) * 2]
    return {"frequency": pitch, "peak": peak, "tailPeak": max(abs(value) for value in tail)}


def verify_native_patman(studio, evidence):
    """Verify GUS decoding and existing native Sample DSP through the real process."""
    studio.call("patman-isolate", "track.edit", {"track": 0, "muted": True})
    studio.call("patman-track", "track.add", {"type": "instrument"})
    studio.call("patman-player", "instrument.load", {"track": 7, "instrument": "patman"})
    studio.call("patman-name", "track.edit", {"track": 7, "name": "Native patch calibration"})
    studio.call("patman-clip", "clip.add", {"track": 7, "position": 0})
    studio.call("patman-note", "note.add", {"track": 7, "clip": 0, "position": 0, "length": 192, "key": 69, "volume": 100})

    def render(identifier):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        return measure_native_patch_wav(path)

    def set_parameter(name, value, identifier):
        # Failed edits and preset loading can replace model IDs; always rediscover.
        parameters = studio.inspect_parameters(identifier + "-discover", "instrument.inspect", {"track": 7})["result"]["parameters"]
        matches = [item for item in parameters if item["name"].endswith(name)]
        assert len(matches) == 1, (name, matches)
        studio.call(identifier, "parameter.set", {"parameter": matches[0]["id"], "value": value})

    formats = {}
    for encoding in ("s8", "u8", "s16", "u16"):
        source = Path(__file__).parent / "fixtures" / ("native-gus-" + encoding + ".pat")
        patch_path = evidence / ("\u97f3 native-gus-" + encoding + ".pat")
        patch_path.write_bytes(source.read_bytes())
        studio.call("patman-load-" + encoding, "instrument.resource.set", {"track": 7, "resource": "patch", "path": str(patch_path)})
        observation = studio.inspect_parameters("patman-inspect-" + encoding, "instrument.inspect", {"track": 7})["result"]
        assert observation["name"] == "Native patch calibration" and observation["resources"][0]["key"] == "patch"
        audio = render("patman-audio-" + encoding)
        assert abs(audio["frequency"] - 440) < 3 and audio["tailPeak"] > 300, "The native GUS sample must loop through the two-second note"
        formats[encoding] = {"path": str(patch_path), "sha256": hashlib.sha256(patch_path.read_bytes()).hexdigest(), **audio}
    for signed, unsigned in (("s8", "u8"), ("s16", "u16")):
        assert abs(formats[signed]["peak"] / formats[unsigned]["peak"] - 1) < 0.01
        assert abs(formats[signed]["frequency"] - formats[unsigned]["frequency"]) < 1

    set_parameter("Loop sample", 0, "patman-loop-off")
    unlooped = render("patman-unlooped-audio")
    assert abs(unlooped["frequency"] - 440) < 3 and unlooped["tailPeak"] == 0
    set_parameter("Loop sample", 1, "patman-loop-on")
    studio.call("patman-nearest-note", "note.edit", {"track": 7, "clip": 0, "note": 0, "key": 81})
    nearest = render("patman-nearest-sample-audio")
    # The second diagnostic sample declares root 880 Hz but contains original 660 Hz PCM.
    # Selecting only the first sample and transposing it would incorrectly produce 880 Hz.
    assert abs(nearest["frequency"] - 660) < 3
    studio.call("patman-tune-note", "note.edit", {"track": 7, "clip": 0, "note": 0, "key": 76})
    tuned = render("patman-tuned-audio")
    assert abs(tuned["frequency"] - 660 * (440 * 2 ** ((76 - 69) / 12)) / 880) < 3
    set_parameter("Tune sample", 0, "patman-tune-off")
    untuned = render("patman-untuned-audio")
    assert abs(untuned["frequency"] - 660) < 3 and abs(untuned["frequency"] - tuned["frequency"]) > 100
    set_parameter("Tune sample", 1, "patman-tune-on")
    studio.call("patman-final-note", "note.edit", {"track": 7, "clip": 0, "note": 0, "key": 81})
    preset = evidence / "native-patman.xpf"
    studio.call("patman-preset", "preset.save", {"track": 7, "path": str(preset)})

    current = formats["u16"]["path"]
    corrupt = evidence / "invalid-second-gus-sample.pat"
    contents = bytearray(Path(current).read_bytes())
    second = 335 + struct.unpack_from("<I", contents, 247)[0]
    struct.pack_into("<I", contents, second + 30, 0)
    corrupt.write_bytes(contents)
    studio.call("patman-reject-later-sample", "instrument.resource.set", {"track": 7, "resource": "patch", "path": str(corrupt)}, ok=False)
    preserved = studio.inspect_parameters("patman-preserved-resource", "instrument.inspect", {"track": 7})["result"]
    assert_owned_asset(studio, preserved["resources"][0]["path"], current)
    survived = render("patman-after-rejected-sample")
    assert abs(survived["frequency"] - 660) < 3
    studio.call("patman-clear", "instrument.resource.set", {"track": 7, "resource": "patch", "path": ""})
    assert render("patman-cleared-silence")["peak"] == 0
    studio.call("patman-preset-restore", "preset.load", {"track": 7, "path": str(preset)})
    restored = render("patman-preset-audio")
    assert abs(restored["frequency"] - 660) < 3
    studio.call("patman-restore-file", "instrument.resource.set", {"track": 7, "resource": "patch", "path": current})
    accepted_patch = Path(current).read_bytes()
    accepted_bundles = []
    for scenario in ("replaced", "removed"):
        try:
            if scenario == "replaced":
                Path(current).write_bytes(b"Original patch replaced after native acceptance")
            else:
                Path(current).unlink()
            saved = studio.call("patman-accepted-bundle-" + scenario, "project.save", {
                "path": str(evidence / ("native-patman-" + scenario + ".mmp")), "withResources": True,
            })["result"]
            bundle_path = Path(saved["path"])
            nodes = ET.parse(bundle_path).findall(".//patman")
            assert len(nodes) == 1
            reference = nodes[0].get("src", "")
            assert reference == "local:resources/" + Path(current).name
            bundled_patch = bundle_path.parent / reference.removeprefix("local:")
            assert bundled_patch.read_bytes() == accepted_patch
            actual = render("patman-accepted-audio-" + scenario)
            assert abs(actual["frequency"] - 660) < 3
            assert abs(actual["peak"] - restored["peak"]) <= 3
            assert abs(actual["tailPeak"] - restored["tailPeak"]) <= 3
            accepted_bundles.append({"scenario": scenario, "path": str(bundle_path),
                                     "assetSha256": hashlib.sha256(bundled_patch.read_bytes()).hexdigest(),
                                     "nativeAudio": actual})
        finally:
            Path(current).write_bytes(accepted_patch)
    studio.call("patman-mute", "track.edit", {"track": 7, "muted": True})
    studio.call("patman-restore-lead", "track.edit", {"track": 0, "muted": False})
    return {"formats": formats, "path": current, "nearestSample": nearest, "tuned": tuned, "untuned": untuned,
            "afterFailure": survived, "presetRestoration": restored, "acceptedInputBundles": accepted_bundles,
            "verification": "native GUS PCM encodings, multi-sample selection, loop/tune WAV behavior, accepted input bundles, failure/clear/preset and restart restoration"}


def measure_native_slicer_wav(path):
    with wave.open(str(path), "rb") as decoded:
        rate = decoded.getframerate()
        assert rate == 44100 and decoded.getnchannels() == 2 and decoded.getsampwidth() == 2
        pcm = array.array("h", decoded.readframes(decoded.getnframes()))
        if sys.byteorder != "little":
            pcm.byteswap()
    assert len(pcm) > int(rate * 1.4) * 2

    def region(start, end):
        return pcm[int(rate * start) * 2:int(rate * end) * 2:2]

    def pitch(start, end):
        samples = region(start, end)
        if max(abs(value) for value in samples) == 0:
            return 0
        assert max(abs(value) for value in samples) > 300
        crossings = [index for index in range(len(samples) - 1) if samples[index] <= 0 < samples[index + 1]]
        assert len(crossings) > 4
        return rate * (len(crossings) - 1) / (crossings[-1] - crossings[0])

    return {"firstPitch": pitch(0.04, 0.2), "secondPitch": pitch(0.58, 0.8),
            "quarterTailPeak": max(abs(value) for value in region(0.32, 0.44)),
            "tailPeak": max(abs(value) for value in region(1.2, 1.4)), "peak": max(abs(value) for value in pcm)}


def verify_native_slicer(studio, evidence):
    """Native slicing, source-rate playback and editable MIDI; WAV data is a test stimulus."""
    path = evidence / "\u97f3 native-slicer.wav"
    rate = 16000
    pcm = array.array("h")
    for frame in range(rate):
        frequency = 440 if frame < rate // 2 else 880
        value = round(12000 * math.sin(2 * math.pi * frequency * frame / rate))
        pcm.extend((value, value))
    if sys.byteorder != "little":
        pcm.byteswap()
    with wave.open(str(path), "wb") as fixture:
        fixture.setnchannels(2); fixture.setsampwidth(2); fixture.setframerate(rate); fixture.writeframes(pcm.tobytes())
    studio.call("slicer-isolate", "track.edit", {"track": 0, "muted": True})
    studio.call("slicer-track", "track.add", {"type": "instrument"})
    studio.call("slicer-player", "instrument.load", {"track": 8, "instrument": "slicert"})
    studio.call("slicer-name", "track.edit", {"track": 8, "name": "Native slice calibration"})
    studio.call("slicer-file", "instrument.resource.set", {"track": 8, "resource": "sample", "path": str(path)})
    automatic = studio.call("slicer-analysis", "instrument.slices.analyze", {"track": 8, "estimateTempo": True})["result"]
    assert automatic["sourceFrames"] == rate and automatic["sampleRate"] == rate and automatic["originalBpm"] == 120
    assert automatic["points"][0] == 0 and automatic["points"][-1] == 1
    assert all(a < b for a, b in zip(automatic["points"], automatic["points"][1:]))
    named = studio.inspect_parameters("slicer-named", "instrument.inspect", {"track": 8})["result"]
    assert named["name"] == "Native slice calibration" and named["slicingSupported"]
    studio.call("slicer-clip", "clip.add", {"track": 8, "position": 0})
    studio.call("slicer-note", "note.add", {"track": 8, "clip": 0, "position": 0, "length": 192, "key": 69, "volume": 100})

    def parameter(name, value, identifier):
        parameters = studio.inspect_parameters(identifier + "-discover", "instrument.inspect", {"track": 8})["result"]["parameters"]
        matches = [item for item in parameters if item["name"].endswith(name)]
        assert len(matches) == 1, (name, matches)
        return studio.call(identifier, "parameter.set", {"parameter": matches[0]["id"], "value": value})

    def render(identifier):
        audio = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(audio), "sampleRate": 44100, "bitDepth": 16})
        return measure_native_slicer_wav(audio)

    parameter("FadeOut", 0, "slicer-zero-fade")
    whole = render("slicer-whole-audio")
    assert abs(whole["firstPitch"] - 440) < 3 and abs(whole["secondPitch"] - 880) < 3 and whole["tailPeak"] == 0
    layout = studio.call("slicer-manual", "instrument.slices.edit", {"track": 8, "points": [0, 0.5, 1]})["result"]
    assert layout["sliceKeys"] == [70, 71] and layout["addressableSlices"] == 2
    studio.call("slicer-first-note", "note.edit", {"track": 8, "clip": 0, "note": 0, "key": 70})
    first = render("slicer-first-slice-audio")
    assert abs(first["firstPitch"] - 440) < 3 and first["secondPitch"] == 0 and first["tailPeak"] == 0
    studio.call("slicer-second-note", "note.edit", {"track": 8, "clip": 0, "note": 0, "key": 71})
    second = render("slicer-second-slice-audio")
    assert abs(second["firstPitch"] - 880) < 3 and second["secondPitch"] == 0
    parameter("Original bpm", 60, "slicer-source-tempo")
    parameter("BPM sync", 1, "slicer-sync-on")
    studio.call("slicer-fast-note", "note.edit", {"track": 8, "clip": 0, "note": 0, "key": 70})
    fast = render("slicer-tempo-synced-audio")
    assert abs(fast["firstPitch"] - 880) < 3 and fast["quarterTailPeak"] == 0
    synced = studio.call("slicer-synced-midi", "instrument.slices.midi", {"track": 8})["result"]["notes"]
    assert [(note["position"], note["length"], note["key"]) for note in synced] == [(0, 24, 70), (24, 24, 71)]
    unsync = parameter("BPM sync", 0, "slicer-sync-off")
    preview = studio.call("slicer-midi-preview", "instrument.slices.midi", {"track": 8})
    assert preview["revision"] == unsync["revision"]
    assert [(note["position"], note["length"]) for note in preview["result"]["notes"]] == [(0, 48), (48, 48)]
    inserted = studio.call("slicer-midi-insert", "clip.fromSlices", {"track": 8, "clip": 0, "replace": True})["result"]
    assert inserted["inserted"] == 2
    sequence = render("slicer-generated-midi-audio")
    assert abs(sequence["firstPitch"] - 440) < 3 and abs(sequence["secondPitch"] - 880) < 3 and sequence["tailPeak"] == 0
    preset = evidence / "native-slicer.xpf"
    studio.call("slicer-preset", "preset.save", {"track": 8, "path": str(preset)})
    bad = evidence / "invalid-slicer.wav"; bad.write_bytes(b"Not decodable audio")
    studio.call("slicer-reject-file", "instrument.resource.set", {"track": 8, "resource": "sample", "path": str(bad)}, ok=False)
    preserved = studio.call("slicer-after-failure", "instrument.slices.inspect", {"track": 8})["result"]
    assert preserved["points"] == [0, 0.5, 1] and preserved["sourceFrames"] == rate
    studio.call("slicer-clear", "instrument.resource.set", {"track": 8, "resource": "sample", "path": ""})
    assert render("slicer-cleared-silence")["peak"] == 0
    studio.call("slicer-preset-restore", "preset.load", {"track": 8, "path": str(preset)})
    restored = render("slicer-preset-audio")
    assert abs(restored["firstPitch"] - 440) < 3 and abs(restored["secondPitch"] - 880) < 3
    studio.call("slicer-restore-file", "instrument.resource.set", {"track": 8, "resource": "sample", "path": str(path)})
    final_layout = studio.call("slicer-final-layout", "instrument.slices.edit", {"track": 8, "points": [0, 0.25, 0.5, 1]})["result"]
    studio.call("slicer-clear-notes", "note.clear", {"track": 8, "clip": 0})
    studio.call("slicer-final-note", "note.add", {"track": 8, "clip": 0, "position": 0, "length": 192, "key": 69, "volume": 100})
    studio.call("slicer-mute", "track.edit", {"track": 8, "muted": True})
    studio.call("slicer-restore-lead", "track.edit", {"track": 0, "muted": False})
    return {"path": str(path), "sha256": hashlib.sha256(path.read_bytes()).hexdigest(), "layout": final_layout,
            "whole": whole, "first": first, "second": second, "synced": fast, "midiSequence": sequence,
            "verification": "actual native SlicerT WAV source-rate, endpoints, zero fade, tempo sync, generated MIDI, presets and recovery"}


def measure_native_opl_wav(path):
    """Read real native output; low-level attenuation checks do not require pitch inference."""
    with wave.open(str(path), "rb") as decoded:
        rate = decoded.getframerate()
        assert rate == 44100 and decoded.getnchannels() == 2 and decoded.getsampwidth() == 2
        pcm = array.array("h", decoded.readframes(decoded.getnframes()))
        if sys.byteorder != "little":
            pcm.byteswap()
    assert len(pcm) >= int(rate * 1.4) * 2
    excerpt = pcm[int(rate * 0.08) * 2:int(rate * 0.4) * 2:2]
    rms = math.sqrt(sum((value / 32768.0) ** 2 for value in excerpt) / len(excerpt))
    crossings = [index for index in range(len(excerpt) - 1) if excerpt[index] <= 0 < excerpt[index + 1]]
    frequency = rate * (len(crossings) - 1) / (crossings[-1] - crossings[0]) if len(crossings) > 10 else None
    return {"rms": rms, "frequency": frequency, "peak": max(abs(value) for value in pcm),
            "tailPeak": max(abs(value) for value in pcm[int(rate * 1.1) * 2:int(rate * 1.4) * 2])}


def verify_native_opulenz(studio, evidence):
    """Exercise bundled OpulenZ models and emulator; register fixtures are test data."""
    import runpy
    fixture = runpy.run_path(str(Path(__file__).parent / "fixtures/generate_sbi.py"))
    path = evidence / "\u97f3 native-opl.sbi"
    carrier = fixture["patch_bytes"](*fixture["PATCHES"]["native-opl-carrier"])
    octave = fixture["patch_bytes"](*fixture["PATCHES"]["native-opl-octave"])
    path.write_bytes(carrier)
    studio.call("opulenz-isolate", "track.edit", {"track": 0, "muted": True})
    studio.call("opulenz-track", "track.add", {"type": "instrument"})
    studio.call("opulenz-player", "instrument.load", {"track": 9, "instrument": "opulenz"})
    studio.call("opulenz-name", "track.edit", {"track": 9, "name": "Native OPL calibration"})
    studio.call("opulenz-file", "instrument.resource.set", {"track": 9, "resource": "patch", "path": str(path)})
    native = studio.inspect_parameters("opulenz-discover", "instrument.inspect", {"track": 9})["result"]
    assert native["name"] == "Native OPL calibration" and native["resources"][0]["requiredForRestore"] is False
    assert native["programCount"] == 128 and native["programSelection"] == {}
    waves = [item for item in native["parameters"] if item["name"].endswith(("Op 1 waveform", "Op 2 waveform"))]
    assert len(waves) == 2 and all(len(item["choices"]) == 4 for item in waves)
    programs = studio.call("opulenz-programs", "instrument.program.list", {"track": 9, "bank": 0})["result"]
    assert programs["total"] == 128 and [item["program"] for item in programs["programs"]] == list(range(128))
    studio.call("opulenz-clip", "clip.add", {"track": 9, "position": 0})
    studio.call("opulenz-note", "note.add", {"track": 9, "clip": 0, "position": 0, "length": 96, "key": 69, "volume": 80})

    def parameter(name, value, identifier):
        parameters = studio.inspect_parameters(identifier + "-discover", "instrument.inspect", {"track": 9})["result"]["parameters"]
        matches = [item for item in parameters if item["name"].endswith(name)]
        assert len(matches) == 1, (name, matches)
        return studio.call(identifier, "parameter.set", {"parameter": matches[0]["id"], "value": value})

    def render(identifier):
        audio = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(audio), "sampleRate": 44100, "bitDepth": 16})
        return measure_native_opl_wav(audio)

    fundamental = render("opulenz-carrier-audio")
    assert fundamental["rms"] > 0.001 and abs(fundamental["frequency"] - 440) < 3 and fundamental["tailPeak"] == 0
    parameter("Op 2 level", 0, "opulenz-level-zero")
    attenuated = render("opulenz-attenuated-audio")
    assert attenuated["rms"] < fundamental["rms"] / 50
    parameter("Op 2 level", 63, "opulenz-level-restore")
    bad = evidence / "invalid-opl.sbi"
    broken = bytearray(carrier); broken[3] = 0; bad.write_bytes(broken)
    studio.call("opulenz-reject-header", "instrument.resource.set", {"track": 9, "resource": "patch", "path": str(bad)}, ok=False)
    survived = render("opulenz-after-failed-import")
    assert abs(survived["frequency"] - 440) < 3
    path.write_bytes(octave)
    studio.call("opulenz-octave-file", "instrument.resource.set", {"track": 9, "resource": "patch", "path": str(path)})
    doubled = render("opulenz-octave-audio")
    assert abs(doubled["frequency"] - 880) < 4
    preset = evidence / "native-opl.xpf"
    studio.call("opulenz-preset", "preset.save", {"track": 9, "path": str(preset)})
    retired = evidence / "retired-opl-import.sbi"
    assert path.resolve().is_relative_to(evidence.resolve()) and retired.resolve().is_relative_to(evidence.resolve())
    path.rename(retired)
    studio.call("opulenz-clear", "instrument.resource.set", {"track": 9, "resource": "patch", "path": ""})
    cleared = studio.inspect_parameters("opulenz-cleared-controls", "instrument.inspect", {"track": 9})["result"]
    assert cleared["resources"][0]["path"] == "" and cleared["programSelection"] == {}
    studio.call("opulenz-preset-restore", "preset.load", {"track": 9, "path": str(preset)})
    independent = render("opulenz-preset-without-import-audio")
    assert not path.exists() and abs(independent["frequency"] - 880) < 4
    selected = studio.call("opulenz-gm", "instrument.program.select", {"track": 9, "bank": 0, "program": 42})["result"]
    assert selected["program"] == 42 and selected["available"]
    gm = render("opulenz-gm-audio")
    assert gm["rms"] > 0.0001
    studio.call("opulenz-reject-gm-bank", "instrument.program.select", {"track": 9, "bank": 1, "program": 42}, ok=False)
    gm_preset = evidence / "native-opl-gm.xpf"
    studio.call("opulenz-gm-preset", "preset.save", {"track": 9, "path": str(gm_preset)})
    studio.call("opulenz-perturb-gm", "instrument.program.select", {"track": 9, "bank": 0, "program": 0})
    studio.call("opulenz-gm-preset-restore", "preset.load", {"track": 9, "path": str(gm_preset)})
    restored_gm = render("opulenz-restored-gm-audio")
    assert abs(restored_gm["rms"] - gm["rms"]) < gm["rms"] * 0.05
    studio.call("opulenz-final-source", "instrument.resource.set", {"track": 9, "resource": "patch", "path": str(retired)})
    parameter("Op 2 frequency multiplier", 3, "opulenz-final-edit")
    final_audio = render("opulenz-manual-multiplier-audio")
    assert abs(final_audio["frequency"] - 1320) < 5
    studio.call("opulenz-mute", "track.edit", {"track": 9, "muted": True})
    # Retain a distinct GM instance too, so restart verifies both native state forms.
    studio.call("opulenz-gm-track", "track.add", {"type": "instrument"})
    studio.call("opulenz-gm-player", "instrument.load", {"track": 10, "instrument": "opulenz"})
    studio.call("opulenz-gm-recovery-preset", "preset.load", {"track": 10, "path": str(gm_preset)})
    studio.call("opulenz-gm-clip", "clip.add", {"track": 10, "position": 0})
    studio.call("opulenz-gm-note", "note.add", {"track": 10, "clip": 0, "position": 0, "length": 96, "key": 69, "volume": 80})
    studio.call("opulenz-gm-mute", "track.edit", {"track": 10, "muted": True})
    studio.call("opulenz-restore-lead", "track.edit", {"track": 0, "muted": False})
    return {"path": str(retired), "originalImportPath": str(path), "sha256": hashlib.sha256(retired.read_bytes()).hexdigest(),
            "carrier": fundamental, "attenuated": attenuated, "afterFailure": survived, "octave": doubled,
            "independentPreset": independent, "gm": gm, "restoredGm": restored_gm, "manualMultiplier": final_audio,
            "verification": "actual native OPL WAV pitch/level, checked SBI import, GM models/presets and recovery"}


def verify_native_transport(studio):
    revision = studio.revision
    initial = studio.call("transport-state", "transport.inspect")["result"]
    assert initial["stopped"] is True and initial["backend"]["kind"] == "dummy", initial
    assert initial["backend"]["renderOnly"] is True and initial["backend"]["supportsCapture"] is False
    assert initial["backend"]["running"] is True
    assert initial["backend"]["processingFailure"] == "none" and initial["backend"]["nativeError"] == 0
    processing = studio.call("dsp-state", "audio.processing.inspect")["result"]
    assert processing["failed"] is False and processing["failure"] == "none"
    assert processing["failureCount"].isdigit() and processing["counterExhausted"] is False
    assert processing["resetEligible"] is True
    for key in ("failed", "failure", "failureCount", "resetEligible", "counterExhausted"):
        assert initial["backend"]["dsp"][key] == processing[key]
    for workers in (processing["workers"], initial["backend"]["dsp"]["workers"]):
        assert all(isinstance(workers[key], int) and workers[key] >= 0
                   for key in ("registered", "ready", "queued", "scanning"))
        assert workers["ready"] <= workers["registered"]
        assert workers["queued"] + workers["scanning"] <= workers["registered"]
        assert isinstance(workers["batchActive"], bool)
        if not workers["batchActive"]:
            assert workers["queued"] == 0 and workers["scanning"] == 0
    retry = studio.call("dsp-healthy-retry", "audio.processing.reset")["result"]
    assert retry["failed"] is False and retry["failureCount"] == processing["failureCount"]
    assert studio.call("dsp-healthy-retry", "audio.processing.reset").get("replayed") is True
    assert studio.revision == revision
    restored = studio.call("device-restore-noop", "audio.device.restore")["result"]
    assert restored["pending"] is False and restored["failed"] is False and restored["failure"] == "none"
    assert studio.call("device-restore-noop", "audio.device.restore").get("replayed") is True
    assert studio.revision == revision
    studio.call("transport-origin", "transport.seek", {"position": 0})
    played = studio.call("transport-play", "transport.play")["result"]
    assert played["playing"] is True and played["playMode"] == "song", played
    paused = studio.call("transport-pause", "transport.pause", {"paused": True})["result"]
    assert paused["paused"] is True and paused["playing"] is False
    assert studio.call("transport-play", "transport.play").get("replayed") is True
    assert studio.call("transport-still-paused", "transport.inspect")["result"]["paused"] is True
    studio.call("transport-paused-seek", "transport.seek", {"position": 193})
    time.sleep(0.05)
    stable = studio.call("transport-paused-state", "transport.inspect")["result"]
    assert stable["position"] == 193 and stable["paused"] is True, stable
    studio.call("transport-reject-mode", "transport.seek", {"playMode": "pattern", "position": 0}, ok=False)
    studio.call("transport-reject-negative", "transport.seek", {"position": -1}, ok=False)
    studio.call("transport-resume", "transport.pause", {"paused": False})
    deadline = time.monotonic() + 3
    progressing = None
    for attempt in range(60):
        progressing = studio.call(f"transport-progress-{attempt}", "transport.inspect")["result"]
        if progressing["position"] > 193:
            break
        assert time.monotonic() < deadline, progressing
        time.sleep(0.05)
    assert progressing["position"] > 193 and progressing["playing"] is True, progressing
    stopped = studio.call("transport-stop", "transport.stop")["result"]
    assert stopped["stopped"] is True and stopped["playMode"] == "none", stopped
    assert studio.revision == revision, "Transient playback must preserve the project revision"
    studio.call("transport-reset-origin", "transport.seek", {"position": 0})
    return {"initial": initial, "progressing": progressing, "stopped": stopped,
            "verification": "native executable transport and dummy clock; no physical sound output"}


def verify_native_mixer_solo(studio):
    initial = studio.call("solo-initial", "project.inspect")["result"]
    probe = studio.call("solo-probe-bus", "mixer.add", {"name": "Solo verification bus"})["result"]["channel"]
    studio.call("solo-master-mute", "mixer.edit", {"channel": 0, "muted": True})
    studio.call("solo-select", "mixer.edit", {"channel": 1, "solo": True})
    active = studio.call("solo-active", "project.inspect")["result"]
    assert active["soloChannel"] == 1 and active["channels"][1]["solo"] is True
    assert active["channels"][0]["muted"] is False and active["channels"][0]["muteBeforeSolo"] is True
    assert active["channels"][probe]["muted"] is True and active["channels"][probe]["muteBeforeSolo"] is False
    studio.call("solo-release", "mixer.edit", {"channel": 1, "solo": False})
    released = studio.call("solo-released", "project.inspect")["result"]
    assert released["soloChannel"] == -1 and released["channels"][0]["muted"] is True
    assert released["channels"][probe]["muted"] is False
    studio.call("solo-undo", "session.undo")
    assert studio.call("solo-after-undo", "project.inspect")["result"]["soloChannel"] == 1
    studio.call("solo-redo", "session.redo")
    assert studio.call("solo-select", "mixer.edit", {"channel": 1, "solo": True}).get("replayed") is True
    assert studio.call("solo-after-replay", "project.inspect")["result"]["soloChannel"] == -1
    studio.call("solo-restore-master", "mixer.edit", {"channel": 0, "muted": initial["channels"][0]["muted"]})
    return {"active": active, "released": released,
            "verification": "native process solo, mute restoration, checkpoint undo/redo and receipt replay"}


def verify_native_mixer_move(studio, evidence):
    probe = studio.call("mixer-move-probe", "mixer.add", {"name": "Reorder probe"})["result"]["channel"]

    def routing_parameter(identifier):
        controls = studio.inspect_parameters(identifier, "parameter.list", {"container": "song", "track": 0})["result"]["parameters"]
        matches = [control for control in controls if control["name"].endswith("Mixer channel")]
        assert len(matches) == 1, matches
        return matches[0]

    selector = routing_parameter("mixer-move-routing-selector")
    assert selector["maximum"] == probe, selector
    studio.call("mixer-move-route-lead", "parameter.set", {"parameter": selector["id"], "value": 1})
    studio.call("mixer-move-bus-volume", "mixer.edit", {"channel": 1, "volume": .625})

    def render(identifier):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as audio:
            assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
            pcm = array.array("h", audio.readframes(audio.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
        assert len(pcm) > 32000 and max(abs(value) for value in pcm) > 300
        return pcm

    initial = studio.call("mixer-move-initial", "project.inspect")["result"]
    # Consume the native fader smoothing caused by the level edit before
    # comparing the unchanged routing on either side of a structural move.
    render("mixer-move-settle")
    before = render("mixer-move-before")
    moved = studio.call("mixer-move-lead", "mixer.edit", {"channel": 1, "to": probe})["result"]
    assert moved["channel"] == probe
    state = studio.call("mixer-move-after", "project.inspect")["result"]
    expected_names = [initial["channels"][0]["name"]] + [item["name"] for item in initial["channels"][2:]] + [initial["channels"][1]["name"]]
    assert [item["name"] for item in state["channels"]] == expected_names
    assert state["tracks"][0]["mixerChannel"] == probe
    after = render("mixer-move-after-audio")
    assert len(before) == len(after)
    maximum_difference = max(abs(a - b) for a, b in zip(before, after))
    assert maximum_difference <= 3, maximum_difference
    studio.call("mixer-move-undo", "session.undo")
    assert studio.call("mixer-move-undo-state", "project.inspect")["result"]["tracks"][0]["mixerChannel"] == 1
    assert studio.call("mixer-move-lead", "mixer.edit", {"channel": 1, "to": probe}).get("replayed") is True
    studio.call("mixer-move-redo", "session.redo")
    studio.call("mixer-move-master-refused", "mixer.edit", {"channel": probe, "to": 0, "name": "Invalid"}, ok=False)
    intact = studio.call("mixer-move-intact", "project.inspect")["result"]
    assert [item["name"] for item in intact["channels"]] == expected_names
    assert intact["tracks"][0]["mixerChannel"] == probe
    project = evidence / "moved-mixer.mmp"
    studio.call("mixer-move-save", "project.save", {"path": str(project)})
    studio.call("mixer-move-load", "project.load", {"path": str(project)})
    restored = studio.call("mixer-move-restored", "project.inspect")["result"]
    assert restored["tracks"][0]["mixerChannel"] == probe
    assert [item["name"] for item in restored["channels"]] == expected_names
    studio.call("mixer-move-return", "mixer.edit", {"channel": probe, "to": 1, "volume": 1})
    selector = routing_parameter("mixer-move-restore-selector")
    studio.call("mixer-move-restore-route", "parameter.set", {"parameter": selector["id"], "value": 0})
    return {"initial": initial, "moved": state, "maximumDifferencePcm": maximum_difference, "project": str(project),
            "verification": "native process selector range, channel move, rendered routing, refusal, XML reload, undo/redo and replay"}


def verify_native_mixer_removal_and_reset(studio, evidence):
    initial = studio.call("mixer-lifecycle-initial", "project.inspect")["result"]
    assert initial["tracks"][0]["mixerChannel"] == 0

    def render(identifier, minimum_peak=300):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as audio:
            assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
            pcm = array.array("h", audio.readframes(audio.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
        assert len(pcm) > 32000 and max(abs(value) for value in pcm) > minimum_peak
        return pcm

    baseline = render("mixer-lifecycle-before")
    target = studio.call("mixer-lifecycle-target", "mixer.add", {"name": "Lifecycle target"})["result"]["channel"]
    survivor = studio.call("mixer-lifecycle-survivor", "mixer.add", {"name": "Lifecycle survivor"})["result"]["channel"]
    parameters = studio.inspect_parameters("mixer-lifecycle-controls", "parameter.list", {"container": "song", "track": 0})["result"]["parameters"]
    selectors = [item for item in parameters if item["name"].endswith("Mixer channel")]
    assert len(selectors) == 1 and selectors[0]["maximum"] == survivor
    studio.call("mixer-lifecycle-assign", "parameter.set", {"parameter": selectors[0]["id"], "value": target})
    studio.call("mixer-lifecycle-edit", "mixer.edit", {"channel": target, "volume": .5, "solo": True})
    studio.call("mixer-lifecycle-unroute", "mixer.unroute", {"from": target, "to": 0})
    studio.call("mixer-lifecycle-route", "mixer.route", {"from": target, "to": survivor, "amount": .25})
    sends = studio.inspect_parameters("mixer-lifecycle-send-controls", "parameter.list", {"from": target, "to": survivor})["result"]["parameters"]
    assert len(sends) == 1 and sends[0]["location"] == {"scope": "mixerRoute", "from": target, "to": survivor}
    studio.call("mixer-lifecycle-send-level", "parameter.set", {"parameter": sends[0]["id"], "value": .5})
    render("mixer-lifecycle-send-settle", minimum_peak=0)
    attenuated = render("mixer-lifecycle-send-audio", minimum_peak=0)
    assert len(attenuated) == len(baseline)
    send_difference = max(abs(wet - dry * .25) for wet, dry in zip(attenuated, baseline))
    assert send_difference <= 3, send_difference
    studio.call("mixer-lifecycle-reset", "mixer.reset", {"channel": target})
    reset = studio.call("mixer-lifecycle-reset-state", "project.inspect")["result"]
    assert reset["tracks"][0]["mixerChannel"] == target and reset["soloChannel"] == -1
    assert reset["channels"][target]["volume"] == 1 and reset["channels"][target]["muted"] is False
    assert reset["channels"][target]["effects"] == []
    target_routes = [route for route in reset["routes"] if route["from"] == target or route["to"] == target]
    assert len(target_routes) == 1 and target_routes[0]["to"] == 0 and target_routes[0]["amount"] == 1
    render("mixer-lifecycle-reset-settle")
    reset_audio = render("mixer-lifecycle-reset-audio")
    assert len(reset_audio) == len(baseline)
    reset_difference = max(abs(a - b) for a, b in zip(baseline, reset_audio))
    assert reset_difference <= 3, reset_difference
    removed = studio.call("mixer-lifecycle-remove", "mixer.remove", {"channel": target})["result"]
    assert removed["removed"] is True and removed["channels"] == len(initial["channels"]) + 1
    after = studio.call("mixer-lifecycle-after", "project.inspect")["result"]
    assert after["tracks"][0]["mixerChannel"] == 0 and after["channels"][target]["name"] == "Lifecycle survivor"
    removed_audio = render("mixer-lifecycle-remove-audio")
    assert len(removed_audio) == len(baseline)
    removed_difference = max(abs(a - b) for a, b in zip(baseline, removed_audio))
    assert removed_difference <= 3, removed_difference
    studio.call("mixer-lifecycle-undo", "session.undo")
    assert studio.call("mixer-lifecycle-undo-state", "project.inspect")["result"]["tracks"][0]["mixerChannel"] == target
    assert studio.call("mixer-lifecycle-remove", "mixer.remove", {"channel": target}).get("replayed") is True
    assert studio.call("mixer-lifecycle-replayed-state", "project.inspect")["result"]["tracks"][0]["mixerChannel"] == target
    studio.call("mixer-lifecycle-redo", "session.redo")
    studio.call("mixer-lifecycle-refuse-master", "mixer.remove", {"channel": 0}, ok=False)
    saved = evidence / "mixer-lifecycle.mmp"
    studio.call("mixer-lifecycle-save", "project.save", {"path": str(saved)})
    studio.call("mixer-lifecycle-load", "project.load", {"path": str(saved)})
    restored = studio.call("mixer-lifecycle-restored", "project.inspect")["result"]
    assert restored["tracks"][0]["mixerChannel"] == 0
    assert [item["name"] for item in restored["channels"]] == [item["name"] for item in after["channels"]]
    studio.call("mixer-lifecycle-cleanup", "mixer.remove", {"channel": target})
    final = studio.call("mixer-lifecycle-final", "project.inspect")["result"]
    assert [item["name"] for item in final["channels"]] == [item["name"] for item in initial["channels"]]
    return {"sendControlDifferencePcm": send_difference, "resetDifferencePcm": reset_difference, "removedDifferencePcm": removed_difference,
            "project": str(saved), "verification": "native process reset/removal, routing audio, selector reassignment, undo/redo, receipt replay and project reload"}


def verify_native_effect_chain(studio, evidence):
    def effects(identifier):
        return studio.call(identifier, "project.inspect")["result"]["channels"][0]["effects"]

    def render(identifier):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as audio:
            assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
            pcm = array.array("h", audio.readframes(audio.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
        assert len(pcm) > 32000 and max(abs(value) for value in pcm) > 300
        return pcm

    assert effects("effect-chain-initial") == []
    baseline = render("effect-chain-dry")
    for index, plugin in enumerate(("amplifier", "stereomatrix")):
        added = studio.call("effect-chain-add-" + plugin, "effect.add", {"channel": 0, "plugin": plugin})["result"]
        assert added["effect"] == index
    duplicate = studio.call("effect-chain-duplicate", "effect.add", {"channel": 0, "plugin": "amplifier"})["result"]
    assert duplicate["effect"] == 2
    repeated = studio.call("effect-chain-repeated-inspect", "effect.inspect", {"channel": 0})["result"]
    assert [item["plugin"] for item in repeated["effects"]] == ["amplifier", "stereomatrix", "amplifier"]
    repeated_controls = studio.inspect_parameters("effect-chain-duplicate-controls", "parameter.list", {"channel": 0, "effect": 2})["result"]["parameters"]
    assert all(item["location"] == {"channel": 0, "effect": 2, "scope": "effect"} for item in repeated_controls)
    volumes = [[item for item in repeated["effects"][index]["parameters"] if item["name"].endswith("Volume") and
                any(part["class"].endswith("AmplifierControls") for part in item["modelPath"])] for index in (0, 2)]
    assert len(volumes[0]) == len(volumes[1]) == 1 and volumes[0][0]["id"] != volumes[1][0]["id"]
    studio.call("effect-chain-duplicate-gain", "parameter.set", {"parameter": volumes[1][0]["id"], "value": 25})
    untouched = studio.call("effect-chain-original-gain", "parameter.inspect", {"parameter": volumes[0][0]["id"]})["result"]
    assert untouched["value"] == 100
    studio.call("effect-chain-remove-duplicate", "effect.remove", {"channel": 0, "effect": 2})
    studio.call("effect-chain-retired-control", "parameter.inspect", {"parameter": volumes[1][0]["id"]}, ok=False)
    native_chain = studio.call("effect-chain-inspect", "effect.inspect", {"channel": 0})["result"]
    assert native_chain["enabled"] is True and all(item["missing"] is False for item in native_chain["effects"])
    parameters = [parameter for item in native_chain["effects"] for parameter in item["parameters"]]
    bypass = native_chain["enabledParameter"]["id"]

    def control(class_name, name):
        matches = [item for item in parameters if item["name"].endswith(name) and
                   any(part["class"].endswith(class_name) for part in item["modelPath"])]
        assert len(matches) == 1, matches
        return matches[0]["id"]

    settings = (("AmplifierControls", "Volume", 50), ("AmplifierControls", "Right gain", 0),
                ("StereoMatrixControls", "Left to Left", 0), ("StereoMatrixControls", "Right to Right", 0),
                ("StereoMatrixControls", "Left to Right", 1), ("StereoMatrixControls", "Right to Left", 1))
    handles = []
    for index, (class_name, name, value) in enumerate(settings):
        handle = control(class_name, name)
        handles.append((handle, value))
        studio.call("effect-chain-control-" + str(index), "parameter.set", {"parameter": handle, "value": value})
    before = render("effect-chain-amplifier-then-swap")
    studio.call("effect-chain-bypass", "parameter.set", {"parameter": bypass, "value": 0})
    bypassed = render("effect-chain-bypassed")
    assert len(bypassed) == len(baseline)
    bypass_error = max(abs(a - b) for a, b in zip(bypassed[8192:], baseline[8192:]))
    assert bypass_error <= 3, bypass_error
    studio.call("effect-chain-enable", "parameter.set", {"parameter": bypass, "value": 1})
    move_args = {"channel": 0, "effect": 0, "to": 1}
    moved = studio.call("effect-chain-move", "effect.move", move_args)["result"]
    assert moved["effect"] == 1 and effects("effect-chain-moved-order") == ["stereomatrix", "amplifier"]
    for index, (handle, value) in enumerate(handles):
        retained = studio.call("effect-chain-retained-" + str(index), "parameter.inspect", {"parameter": handle})["result"]
        assert retained["value"] == value
    after = render("effect-chain-swap-then-amplifier")
    assert len(before) == len(after) == len(baseline)
    # Exclude the initial control smoothing interval. These native matrices do
    # not commute: attenuate/keep left then swap, versus swap then keep left.
    active = slice(4096, None)
    assert max(abs(value) for value in before[0::2][active]) <= 3
    assert max(abs(value) for value in after[1::2][active]) <= 3
    before_error = max(abs(wet - dry * .5) for wet, dry in zip(before[1::2][active], baseline[0::2][active]))
    after_error = max(abs(wet - dry * .5) for wet, dry in zip(after[0::2][active], baseline[1::2][active]))
    assert before_error <= 3 and after_error <= 3, (before_error, after_error)
    assert max(abs(value) for value in before[1::2][active]) > 300
    assert max(abs(value) for value in after[0::2][active]) > 300
    studio.call("effect-chain-undo-move", "session.undo")
    assert effects("effect-chain-undone-order") == ["amplifier", "stereomatrix"]
    assert studio.call("effect-chain-move", "effect.move", move_args).get("replayed") is True
    assert effects("effect-chain-replayed-order") == ["amplifier", "stereomatrix"]
    studio.call("effect-chain-redo-move", "session.redo")
    studio.call("effect-chain-ambiguous-move", "effect.move", dict(move_args, direction="down"), ok=False)
    assert effects("effect-chain-refused-order") == ["stereomatrix", "amplifier"]
    project = evidence / "effect-chain.mmp"
    studio.call("effect-chain-save", "project.save", {"path": str(project)})
    studio.call("effect-chain-load", "project.load", {"path": str(project)})
    assert effects("effect-chain-loaded-order") == ["stereomatrix", "amplifier"]
    restored = render("effect-chain-restored")
    assert len(restored) == len(after)
    restored_error = max(abs(a - b) for a, b in zip(after[8192:], restored[8192:]))
    assert restored_error <= 3, restored_error
    studio.call("effect-chain-remove", "effect.remove", {"channel": 0, "effect": 0})
    assert effects("effect-chain-removed-order") == ["amplifier"]
    studio.call("effect-chain-undo-remove", "session.undo")
    assert effects("effect-chain-restored-pair") == ["stereomatrix", "amplifier"]
    cleared = studio.call("effect-chain-clear", "effect.clear", {"channel": 0})["result"]
    assert cleared["removed"] == 2 and effects("effect-chain-cleared-order") == []
    dry = render("effect-chain-cleared-audio")
    assert len(dry) == len(baseline)
    cleared_error = max(abs(a - b) for a, b in zip(dry, baseline))
    assert cleared_error <= 3, cleared_error
    studio.call("effect-chain-undo-clear", "session.undo")
    assert effects("effect-chain-undo-clear-order") == ["stereomatrix", "amplifier"]
    assert studio.call("effect-chain-clear", "effect.clear", {"channel": 0}).get("replayed") is True
    assert effects("effect-chain-replay-clear-order") == ["stereomatrix", "amplifier"]
    studio.call("effect-chain-redo-clear", "session.redo")
    assert effects("effect-chain-final") == []
    return {"project": str(project), "bypassErrorPcm": bypass_error, "beforeErrorPcm": before_error, "afterErrorPcm": after_error,
            "restoredErrorPcm": restored_error, "clearedErrorPcm": cleared_error,
            "verification": "native Amplifier/Stereo Matrix order, repeated effect discovery, native bypass, control identity, stereo render, clear/removal, history, replay and XML reload"}


def verify_native_graphs(studio, evidence):
    original = evidence / "graphs-original.mmp"
    studio.call("graphs-preserve-original", "project.save", {"path": str(original)})
    studio.call("graphs-new", "project.new", {"empty": True})
    studio.call("graphs-track", "track.add", {"type": "instrument", "name": "Native graph verification"})
    studio.call("graphs-instrument", "instrument.load", {"track": 0, "instrument": "bitinvader"})
    studio.call("graphs-tempo", "song.set", {"tempo": 120, "masterVolume": 50})
    studio.call("graphs-clip", "clip.add", {"track": 0, "position": 0})
    studio.call("graphs-note", "note.add", {"track": 0, "clip": 0, "position": 0, "length": 144, "key": 57, "volume": 65})

    def inspect(identifier, selector):
        return studio.call(identifier, "graph.inspect", selector)["result"]["graph"]

    def render(identifier):
        path = evidence / (identifier + ".wav")
        studio.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as audio:
            assert audio.getnchannels() == 2 and audio.getsampwidth() == 2
            pcm = array.array("h", audio.readframes(audio.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
        assert len(pcm) > 32000
        return max(abs(value) for value in pcm)

    selector = {"track": 0, "graph": "wave"}
    discovered = studio.call("graphs-bitinvader-list", "graph.list", {"track": 0})["result"]["graphs"]
    assert [(item["key"], item["length"]) for item in discovered] == [("wave", 200)]
    revision = studio.revision
    before = inspect("graphs-before", selector)
    assert before["length"] == 200 and studio.revision == revision
    baseline_peak = render("graphs-bitinvader-sine")
    assert baseline_peak > 300
    zero_args = dict(selector, values=[0] * 200)
    studio.call("graphs-bitinvader-zero", "graph.set", zero_args)
    assert render("graphs-bitinvader-silent") <= 1
    studio.call("graphs-undo-zero", "session.undo")
    assert inspect("graphs-undone", selector)["values"] == before["values"]
    assert studio.call("graphs-bitinvader-zero", "graph.set", zero_args).get("replayed") is True
    assert inspect("graphs-replayed", selector)["values"] == before["values"]
    studio.call("graphs-redo-zero", "session.redo")
    assert inspect("graphs-redone", selector)["values"] == [0] * 200
    studio.call("graphs-bitinvader-sine-again", "graph.transform", dict(selector, action="sine"))

    # Shrinking the visible table must preserve its native serialized tail.
    params = studio.inspect_parameters("graphs-bitinvader-controls", "instrument.inspect", {"track": 0})["result"]["parameters"]
    length_id = next(item["id"] for item in params if item["name"].endswith("Sample length"))
    full = inspect("graphs-full-table", selector)["values"]
    studio.call("graphs-shrink-table", "parameter.set", {"parameter": length_id, "value": 4})
    studio.call("graphs-edit-prefix", "graph.set", dict(selector, values=[0.25] * 4))
    short_project = evidence / "graphs-short-table.mmp"
    studio.call("graphs-save-short", "project.save", {"path": str(short_project)})
    studio.call("graphs-load-short", "project.load", {"path": str(short_project)})
    params = studio.inspect_parameters("graphs-restored-controls", "instrument.inspect", {"track": 0})["result"]["parameters"]
    length_id = next(item["id"] for item in params if item["name"].endswith("Sample length"))
    studio.call("graphs-expand-table", "parameter.set", {"parameter": length_id, "value": 200})
    assert inspect("graphs-retained-tail", selector)["values"] == [0.25] * 4 + full[4:]
    studio.call("graphs-restore-full-sine", "graph.transform", dict(selector, action="sine"))

    waveform = evidence / "graphs-import.wav"
    with wave.open(str(waveform), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(44100)
        output.writeframes(struct.pack("<64h", *([8192] * 64)))
    imported = studio.call("graphs-import", "graph.load", dict(selector, path=str(waveform)))["result"]["graph"]
    assert all(abs(value - 0.25) < 0.0001 for value in imported["values"])
    embedded_project = evidence / "graphs-embedded.mmp"
    studio.call("graphs-save-import", "project.save", {"path": str(embedded_project)})
    waveform.unlink()
    studio.call("graphs-reload-import", "project.load", {"path": str(embedded_project)})
    assert inspect("graphs-import-after-source-removal", selector)["values"] == imported["values"]
    broken = ET.parse(embedded_project)
    node = broken.find(".//bitinvader")
    assert node is not None
    node.set("sampleShape", base64.b64encode(b"abc").decode("ascii"))
    malformed = evidence / "graphs-malformed.mmp"
    broken.write(malformed, encoding="utf-8", xml_declaration=True)
    studio.call("graphs-reject-malformed-project", "project.load", {"path": str(malformed)}, ok=False)
    assert inspect("graphs-after-failed-load", selector)["values"] == imported["values"]
    studio.call("graphs-sine-for-effects", "graph.transform", dict(selector, action="sine"))

    effects = {}
    for plugin, keys in (("waveshaper", ["transfer"]), ("dynamicsprocessor", ["transfer"]),
                         ("multitapecho", ["tap-gain", "tap-filter"])):
        prefix = "graphs-" + plugin
        studio.call(prefix + "-add", "effect.add", {"channel": 0, "plugin": plugin})
        native = studio.call(prefix + "-list", "graph.list", {"channel": 0, "effect": 0})["result"]["graphs"]
        assert [item["key"] for item in native] == keys
        edits = {}
        for graph in native:
            target = {"channel": 0, "effect": 0, "graph": graph["key"]}
            values = [graph["min"] + (graph["max"] - graph["min"]) * index / max(1, graph["length"] - 1)
                      for index in range(graph["length"])]
            edits[graph["key"]] = studio.call(prefix + "-set-" + graph["key"], "graph.set", dict(target, values=values))["result"]["graph"]["values"]
        project = evidence / (prefix + ".mmp")
        studio.call(prefix + "-save", "project.save", {"path": str(project)})
        studio.call(prefix + "-load", "project.load", {"path": str(project)})
        for key, values in edits.items():
            assert inspect(prefix + "-restored-" + key, {"channel": 0, "effect": 0, "graph": key})["values"] == values
        if plugin == "waveshaper":
            assert render(prefix + "-audible") > 300
            studio.call(prefix + "-silence", "graph.transform", {"channel": 0, "effect": 0, "graph": "transfer", "action": "clear"})
            assert render(prefix + "-silent") <= 1
        effects[plugin] = {"keys": keys, "project": str(project)}
        studio.call(prefix + "-remove", "effect.clear", {"channel": 0})

    instruments = {}
    for plugin, expected_keys in (("watsyn", ["a1", "a2", "b1", "b2"]), ("freeboy", ["wave"]),
                                  ("vibedstrings", ["string" + str(index) for index in range(1, 10)]),
                                  ("xpressive", ["w1", "w2", "w3"])):
        prefix = "graphs-" + plugin
        studio.call(prefix + "-load", "instrument.load", {"track": 0, "instrument": plugin})
        native = studio.inspect_parameters(prefix + "-inspect", "instrument.inspect", {"track": 0})["result"]["graphs"]
        assert [item["key"] for item in native] == expected_keys
        edited = {}
        for graph in native:
            values = [graph["max"] if index % 2 else graph["min"] for index in range(graph["length"])]
            edited[graph["key"]] = studio.call(prefix + "-set-" + graph["key"], "graph.set",
                                              {"track": 0, "graph": graph["key"], "values": values})["result"]["graph"]["values"]
        project = evidence / (prefix + ".mmp")
        studio.call(prefix + "-save", "project.save", {"path": str(project)})
        studio.call(prefix + "-reload", "project.load", {"path": str(project)})
        for key, values in edited.items():
            assert inspect(prefix + "-restored-" + key, {"track": 0, "graph": key})["values"] == values
        instruments[plugin] = {"keys": expected_keys, "project": str(project)}

    # Native expression capabilities make output depend on W1 without an editor.
    preset = evidence / "graphs-xpressive.xpf"
    for key in ("o1", "o2"):
        studio.call("graphs-xpressive-expression-" + key, "expression.set",
                    {"track": 0, "expression": key, "source": "W1(t*f)"})
    studio.call("graphs-xpressive-wave-expression", "expression.set",
                {"track": 0, "expression": "w1", "source": "sinew(t)"})
    studio.call("graphs-xpressive-native-wave", "graph.transform", {"track": 0, "graph": "w1", "action": "sine"})
    xpressive_peak = render("graphs-xpressive-native-dsp")
    assert xpressive_peak > 300
    studio.call("graphs-xpressive-native-clear", "graph.transform", {"track": 0, "graph": "w1", "action": "clear"})
    assert render("graphs-xpressive-native-silent") <= 1
    studio.call("graphs-xpressive-save-cleared-expression", "preset.save", {"track": 0, "path": str(preset)})
    assert ET.parse(preset).find(".//xpressive").get("W1") == ""
    studio.call("graphs-restore-original", "project.load", {"path": str(original)})
    return {"effects": effects, "instruments": instruments, "bitinvaderPeakPcm": baseline_peak,
            "xpressivePeakPcm": xpressive_peak, "embeddedProject": str(embedded_project),
            "verification": "native graph discovery, DSP/silence, hidden tail, file embedding, malformed-project rollback, undo/redo/replay and eight bundled plugin serializers"}


def verify_native_expressions(binary, evidence, studios):
    session = evidence / "native-expressions-session"
    studio = Studio(binary, session, evidence)
    studios.append(studio)
    studio.call("expressions-new", "project.new", {"empty": True})
    studio.call("expressions-track", "track.add", {"type": "instrument", "name": "Native expressions"})
    studio.call("expressions-instrument", "instrument.load", {"track": 0, "instrument": "xpressive"})
    studio.call("expressions-tempo", "song.set", {"tempo": 120, "masterVolume": 50})
    studio.call("expressions-clip", "clip.add", {"track": 0, "position": 0})
    studio.call("expressions-note", "note.add", {"track": 0, "clip": 0, "position": 0, "length": 144, "key": 57, "volume": 65})

    def inventory(client, identifier):
        return client.call(identifier, "expression.list", {"track": 0})["result"]["expressions"]

    def waveform(client, identifier, key="w1"):
        return client.call(identifier, "graph.inspect", {"track": 0, "graph": key})["result"]["graph"]["values"]

    def render(client, identifier):
        path = evidence / (identifier + ".wav")
        client.call(identifier, "audio.render", {"path": str(path), "sampleRate": 44100, "bitDepth": 16})
        with wave.open(str(path), "rb") as audio:
            pcm = array.array("h", audio.readframes(audio.getnframes()))
            if sys.byteorder != "little":
                pcm.byteswap()
        assert len(pcm) > 32000
        return max(abs(value) for value in pcm)

    initial = inventory(studio, "expressions-discover")
    assert [item["key"] for item in initial] == ["w1", "w2", "w3", "o1", "o2"]
    assert "f" not in initial[0]["symbols"] and "f" in initial[3]["symbols"]
    before_wave = waveform(studio, "expressions-wave-before-validation")
    before_manifest = json.loads((session / "session.json").read_text(encoding="utf-8"))
    for key, source, valid in (("w1", "sinew(t)", True), ("w1", "W2(t)", False),
                               ("o1", "sinew(integrate(f))*(1-rel)", True), ("o1", "sinew(", False),
                               ("o1", "sqrt(-1)", False)):
        response = studio.call("expressions-validate-" + str(len(studio.trace)), "expression.validate",
                               {"track": 0, "expression": key, "source": source, "preview": True})["result"]
        assert response["valid"] is valid
        assert (response["error"] == "") == valid
    random_previews = [studio.call("expressions-random-" + str(index), "expression.validate",
                                   {"track": 0, "expression": "w1", "source": "randv(t*srate)", "preview": True})["result"]["preview"]
                       for index in range(2)]
    assert random_previews[0] == random_previews[1]
    assert waveform(studio, "expressions-wave-after-validation") == before_wave
    assert inventory(studio, "expressions-after-validation") == initial
    after_manifest = json.loads((session / "session.json").read_text(encoding="utf-8"))
    assert all(before_manifest[key] == after_manifest[key] for key in ("history", "cursor", "revision"))

    generated = studio.call("expressions-wave-sine", "expression.set",
                            {"track": 0, "expression": "w1", "source": "sinew(t)", "preview": True})["result"]
    assert generated["valid"] and generated["frames"] == 4096 and generated["nonfiniteSamples"] == 0
    assert abs(generated["preview"][1024] - 1) < 0.00001
    assert waveform(studio, "expressions-generated-wave") == generated["preview"]
    studio.call("expressions-wave-two", "expression.set", {"track": 0, "expression": "w2", "source": "trianglew(t)"})
    replaced = studio.call("expressions-wave-nonfinite", "expression.set",
                           {"track": 0, "expression": "w3", "source": "sqrt(-1)"})["result"]
    assert replaced["valid"] and replaced["nonfiniteSamples"] == 4096
    assert waveform(studio, "expressions-sanitized-wave", "w3") == [0] * 4096
    studio.call("expressions-output-one", "expression.set", {"track": 0, "expression": "o1", "source": "W1(integrate(f))"})
    studio.call("expressions-output-two", "expression.set", {"track": 0, "expression": "o2", "source": ""})
    audible = render(studio, "expressions-audible")
    assert audible > 300
    previous = inventory(studio, "expressions-before-bad-edit")
    for index, source in enumerate(("sinew(", "sqrt(-1)")):
        studio.call("expressions-reject-output-" + str(index), "expression.set",
                    {"track": 0, "expression": "o1", "source": source}, ok=False)
        assert inventory(studio, "expressions-retained-output-" + str(index)) == previous
    silent_args = {"track": 0, "expression": "o1", "source": ""}
    studio.call("expressions-silence", "expression.set", silent_args)
    assert render(studio, "expressions-silent") <= 1
    studio.call("expressions-undo-silence", "session.undo")
    assert inventory(studio, "expressions-undone") == previous
    assert studio.call("expressions-silence", "expression.set", silent_args).get("replayed") is True
    assert inventory(studio, "expressions-replayed") == previous
    studio.call("expressions-redo-silence", "session.redo")
    assert inventory(studio, "expressions-redone")[3]["source"] == ""
    studio.call("expressions-restore-output", "expression.set", {"track": 0, "expression": "o1", "source": "W1(integrate(f))"})
    retained = waveform(studio, "expressions-retain-wave-before")
    studio.call("expressions-release-wave-formula", "expression.set", {"track": 0, "expression": "w1", "source": ""})
    assert waveform(studio, "expressions-retain-wave-after") == retained
    studio.call("expressions-edit-native-graph", "graph.transform", {"track": 0, "graph": "w2", "action": "sine"})
    assert inventory(studio, "expressions-after-graph-edit")[1]["source"] == ""
    expected = inventory(studio, "expressions-final-text")
    waves = {key: waveform(studio, "expressions-final-" + key, key) for key in ("w1", "w2", "w3")}
    project = evidence / "native-expressions.mmp"
    preset = evidence / "native-expressions.xpf"
    studio.call("expressions-project-save", "project.save", {"path": str(project)})
    studio.call("expressions-preset-save", "preset.save", {"track": 0, "path": str(preset)})
    studio.call("expressions-project-load", "project.load", {"path": str(project)})
    studio.call("expressions-preset-load", "preset.load", {"track": 0, "path": str(preset)})
    assert inventory(studio, "expressions-after-native-loads") == expected
    studio.shutdown()
    resumed = Studio(binary, session, evidence)
    studios.append(resumed)
    assert inventory(resumed, "expressions-reopened") == expected
    for key, values in waves.items():
        assert waveform(resumed, "expressions-reopened-" + key, key) == values
    restored_peak = render(resumed, "expressions-reopened-audio")
    assert restored_peak > 300
    resumed.shutdown()
    (evidence / "native-expressions-trace.json").write_text(json.dumps(studio.trace + resumed.trace, indent=2), encoding="utf-8")
    return {"project": str(project), "preset": str(preset), "audiblePeakPcm": audible, "reopenedPeakPcm": restored_peak,
            "verification": "actual Xpressive formulas, nonmutating validation, native generated waves and DSP, refusal, empty semantics, graph ownership, history/replay, presets/projects and session restart"}


def verify_native_midi_configuration(studio, evidence):
    graph_before = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    graph = studio.call("midi-graph-inspect", "midi.backend.inspect", {"direction": "output", "limit": 1})["result"]
    assert graph["scope"] == "client" and graph["deviceReceiptVerified"] is False
    assert graph["supported"] is False and graph["observed"] is False and graph["ports"] == []
    assert graph["reconnectSupported"] is False and graph["reconnectDeferred"] is False
    route_args = {"direction": "output", "port": "missing-fixture:input", "enabled": True}
    refused_graph = studio.call("midi-graph-unsupported", "midi.backend.route", route_args, ok=False)
    assert "does not expose client-wide graph routing" in refused_graph["error"]
    assert studio.call("midi-graph-unsupported", "midi.backend.route", route_args, ok=False).get("replayed") is True
    for enabled in (True, False):
        args = {**route_args, "enabled": enabled}
        request_id = "midi-reconnect-unsupported-" + str(enabled)
        refused_reconnect = studio.call(request_id, "midi.backend.reconnect", args, ok=False)
        assert "does not expose client-wide reconnect preferences" in refused_reconnect["error"]
        assert studio.call(request_id, "midi.backend.reconnect", args, ok=False).get("replayed") is True
    graph_after = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    for key in ("revision", "history", "historyIndex"):
        assert graph_after.get(key) == graph_before.get(key)
    studio.call("midi-fixture-track", "track.add", {"type": "instrument", "name": "MIDI configuration verification"})
    settings = {"track": 1, "readable": True, "writable": True, "inputChannel": 3, "outputChannel": 4,
                "inputController": 74, "outputController": -1, "fixedInputVelocity": 80,
                "fixedOutputVelocity": 70, "fixedOutputNote": 65, "outputProgram": 128, "baseVelocity": 100}
    configured = studio.call("midi-fixture-configure", "midi.configure", settings)["result"]
    assert configured["rawBackend"] is True and configured["connectionVerified"] is False
    assert configured["outputProgram"] == 128 and configured["baseVelocity"] == 100
    assert studio.call("midi-fixture-configure", "midi.configure", settings).get("replayed") is True
    rejected = studio.call("midi-fixture-unknown", "midi.subscribe", {
        "track": 1, "direction": "input", "port": "invented endpoint", "enabled": True}, ok=False)
    assert "raw MIDI backend" in rejected["error"], rejected
    preserved = studio.call("midi-fixture-inspect", "midi.inspect", {"track": 1})["result"]
    assert preserved["inputChannel"] == 3 and preserved["fixedOutputNote"] == 65
    before_reset = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    reset = studio.call("midi-fixture-reset", "midi.input.reset", {"track": 1})
    assert reset["result"]["recoveryChanged"] is False and reset["result"]["inputDelivery"]["recoveryCount"] == "0"
    assert studio.call("midi-fixture-reset", "midi.input.reset", {"track": 1}).get("replayed") is True
    studio.call("midi-fixture-reset-malformed", "midi.input.reset", {"track": 1, "readable": False}, ok=False)
    after_reset = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    assert after_reset["history"] == before_reset["history"] and after_reset["revision"] == before_reset["revision"]
    saved = evidence / "midi-configuration.mmp"
    studio.call("midi-fixture-save", "project.save", {"path": str(saved)})
    ports = ET.parse(saved).getroot().findall(".//midiport")
    assert any(port.get("inputchannel") == "3" and port.get("outputprogram") == "128" and
               port.get("basevelocity") == "100" for port in ports), "Native MIDI models must serialize"

    def volume_selector(identifier):
        parameters = studio.inspect_parameters(identifier, "instrument.inspect", {"track": 1})["result"]["parameters"]
        targets = [item for item in parameters if item["name"].endswith("Volume") and item["modelPath"] and
                   item["modelPath"][-1]["class"].endswith("InstrumentTrack")]
        assert len(targets) == 1, targets
        return {"parameter": targets[0]["id"]}

    selector = volume_selector("midi-cc-discover")
    cc_args = dict(selector, inputController=74, inputChannel=3)
    connected = studio.call("midi-cc-connect", "controller.midi.connect", cc_args)["result"]
    assert connected["ownership"] == "parameter connection" and connected["midi"]["inputController"] == 74
    assert studio.call("midi-cc-connect", "controller.midi.connect", cc_args).get("replayed") is True
    metadata = studio.call("midi-cc-metadata", "parameter.inspect", selector)["result"]["controller"]
    assert metadata["index"] is None and metadata["midiSelector"] == selector
    studio.call("midi-cc-configure", "midi.configure", dict(selector, inputController=71, inputChannel=16))
    studio.call("midi-cc-undo", "session.undo")
    selector = volume_selector("midi-cc-after-undo")
    assert studio.call("midi-cc-prior", "midi.inspect", selector)["result"]["inputController"] == 74
    studio.call("midi-cc-redo", "session.redo")
    selector = volume_selector("midi-cc-after-redo")
    studio.call("midi-cc-reject-output", "midi.configure", dict(selector, inputController=72, writable=True), ok=False)
    selector = volume_selector("midi-cc-after-refusal")
    cc_preserved = studio.call("midi-cc-preserved", "midi.inspect", selector)["result"]
    assert cc_preserved["inputController"] == 71 and cc_preserved["inputChannel"] == 16 and not cc_preserved["writable"]
    before_learn = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    learn_args = dict(selector, timeoutMs=1000)
    learn_refused = studio.call("midi-learn-no-input", "controller.midi.learn", learn_args, ok=False)
    assert "observed active queued MIDI input" in learn_refused["error"], learn_refused
    after_learn = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    assert after_learn["history"] == before_learn["history"] and after_learn["cursor"] == before_learn["cursor"]
    assert after_learn["revision"] == before_learn["revision"] + 1
    assert studio.call("midi-learn-no-input", "controller.midi.learn", learn_args, ok=False).get("replayed") is True
    selector = volume_selector("midi-cc-after-learn-refusal")
    cc_preserved = studio.call("midi-cc-learn-preserved", "midi.inspect", selector)["result"]
    assert cc_preserved["inputController"] == 71 and cc_preserved["inputChannel"] == 16
    cc_saved = evidence / "midi-controller.mmp"
    studio.call("midi-cc-save", "project.save", {"path": str(cc_saved)})
    controllers = ET.parse(cc_saved).getroot().findall(".//Midicontroller")
    assert len(controllers) == 1 and controllers[0].get("inputcontroller") == "71" and controllers[0].get("inputchannel") == "16"
    studio.call("midi-fixture-remove", "track.remove", {"track": 1})
    return {"configured": configured, "preserved": preserved, "inputReset": reset, "controller": cc_preserved,
            "learnRefusal": learn_refused,
            "verification": "native executable track and CC-controller models, serialization, learn refusal, history, healthy reset and replay; no physical connectivity"}


def verify_native_track_move(studio, evidence):
    for suffix, kind, name in (("a", "pattern", "Move Pattern A"), ("plain", "instrument", "Move Interlude"),
                               ("b", "pattern", "Move Pattern B"), ("c", "pattern", "Move Pattern C")):
        studio.call("move-add-" + suffix, "track.add", {"type": kind, "name": name})
    studio.call("move-note-row", "track.add", {"container": "pattern", "type": "instrument", "name": "Move notes"})
    for pattern in range(3):
        studio.call(f"move-note-{pattern}", "note.add", {"container": "pattern", "track": 0, "clip": pattern,
                    "position": 12, "length": 24, "key": 50 + pattern, "volume": 93})

    def observe(identifier):
        state = studio.call(identifier, "project.inspect")["result"]
        patterns = [track for track in state["tracks"] if "patternIndex" in track]
        mapping = {}
        for track in patterns:
            notes = state["patternTracks"][0]["clips"][track["patternIndex"]]["notes"]
            added = [note for note in notes if note["position"] == 12 and note["length"] == 24]
            assert len(added) == 1, notes
            mapping[track["name"]] = added[0]["key"]
        assert mapping == {"Move Pattern A": 50, "Move Pattern B": 51, "Move Pattern C": 52}, mapping
        return {"order": [track["name"] for track in state["tracks"]], "content": mapping,
                "columns": [track["patternIndex"] for track in patterns], "selectedPattern": state["transport"]["pattern"]}

    before = observe("move-before")
    move_args = {"track": 1, "to": 4}
    result = studio.call("move-pattern", "track.edit", move_args)["result"]
    assert {key: result[key] for key in ("from", "track", "changed", "patternIndex")} == {
        "from": 1, "track": 4, "changed": True, "patternIndex": 2}, result
    after = observe("move-after")
    assert after["order"] == ["Native lead", "Move Interlude", "Move Pattern B", "Move Pattern C", "Move Pattern A"]
    assert after["columns"] == [0, 1, 2]
    assert before["selectedPattern"] == 2 and after["selectedPattern"] == 1
    assert studio.call("move-pattern", "track.edit", move_args).get("replayed") is True
    assert observe("move-after-replay") == after
    studio.call("move-undo", "session.undo")
    assert observe("move-after-undo") == before
    studio.call("move-redo", "session.redo")
    assert observe("move-after-redo") == after
    history = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    studio.call("move-invalid", "track.edit", {"track": 1, "to": 99}, ok=False)
    assert observe("move-after-refusal") == after
    rejected = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    assert rejected["history"] == history["history"] and rejected["cursor"] == history["cursor"]
    saved = evidence / "reordered-tracks.mmp"
    studio.call("move-save", "project.save", {"path": str(saved)})
    saved_patterns = [track.get("name") for track in ET.parse(saved).getroot().findall(".//track")
                      if track.get("name", "").startswith("Move Pattern ")]
    assert saved_patterns == ["Move Pattern B", "Move Pattern C", "Move Pattern A"], saved_patterns
    assert ET.parse(saved).getroot().find("head").get("currentpattern") == "1"
    studio.call("move-remove-notes", "track.remove", {"container": "pattern", "track": 0})
    for track in range(4, 0, -1):
        studio.call(f"move-remove-{track}", "track.remove", {"track": track})
    return {"before": before, "after": after,
            "verification": "native process pattern-column ownership, multi-row move, metadata, rejection, history/replay and XML order"}


def verify_native_track_clone(studio, evidence):
    studio.call("clone-curves", "track.add", {"type": "automation", "name": "Clone identity curves"})
    studio.call("clone-source-curve", "clip.add", {"track": 1, "position": 0})

    def volume(identifier, track):
        parameters = studio.inspect_parameters(identifier, "parameter.list")["result"]["parameters"]
        matches = [parameter for parameter in parameters
                   if parameter["location"] == {"container": "song", "track": track}
                   and parameter["name"].endswith("Volume")]
        assert len(matches) == 1, matches
        return matches[0]["id"]

    original_volume = volume("clone-source-volume", 0)
    studio.call("clone-source-connect", "automation.connect", {"track": 1, "clip": 0, "parameter": original_volume})
    studio.call("clone-source-point", "automation.point", {"track": 1, "clip": 0, "position": 0, "value": 81})
    cloned = studio.call("clone-instrument", "track.clone", {"track": 0})["result"]
    assert cloned["track"] == 2
    duplicate_volume = volume("clone-duplicate-volume", 2)
    assert duplicate_volume != original_volume
    studio.call("clone-rename", "track.edit", {"track": 2, "name": "Cloned native lead"})
    studio.call("clone-target-curve", "clip.add", {"track": 1, "position": 384})
    studio.call("clone-target-connect", "automation.connect", {"track": 1, "clip": 1, "parameter": duplicate_volume})
    studio.call("clone-target-point", "automation.point", {"track": 1, "clip": 1, "position": 0, "value": 43})
    state = studio.call("clone-instrument-observe", "project.inspect")["result"]
    assert state["tracks"][0]["clips"] == state["tracks"][2]["clips"]

    studio.call("clone-pattern-track", "track.add", {"type": "pattern", "name": "Clone source pattern"})
    studio.call("clone-pattern-row", "track.add", {"container": "pattern", "type": "instrument", "name": "Clone pattern notes"})
    studio.call("clone-pattern-note", "note.add", {"container": "pattern", "track": 0, "clip": 0, "position": 12, "length": 36, "key": 53})
    pattern_copy = studio.call("clone-pattern", "track.clone", {"track": 3})["result"]
    assert pattern_copy["track"] == 4

    def patterns(identifier, count):
        state = studio.call(identifier, "project.inspect")["result"]
        clips = state["patternTracks"][0]["clips"]
        assert len(clips) == count
        assert state["transport"]["pattern"] == count - 1
        if count == 2:
            assert clips[0]["notes"] == clips[1]["notes"]
            assert state["tracks"][4]["patternIndex"] == 1
        return clips

    patterns("clone-pattern-after", 2)
    studio.call("clone-pattern-undo", "session.undo")
    patterns("clone-pattern-after-undo", 1)
    assert studio.call("clone-pattern", "track.clone", {"track": 3}).get("replayed") is True
    patterns("clone-pattern-after-replay", 1)
    studio.call("clone-pattern-redo", "session.redo")
    patterns("clone-pattern-after-redo", 2)
    saved = evidence / "cloned-tracks.mmp"
    studio.call("clone-save", "project.save", {"path": str(saved)})
    saved_tracks = ET.parse(saved).getroot().find("song/trackcontainer").findall("track")
    source_id = saved_tracks[0].find("instrumenttrack/vol").get("id")
    copy_id = saved_tracks[2].find("instrumenttrack/vol").get("id")
    assert source_id != copy_id, "A copied model must not alias the source's serialized identity"
    saved_curves = saved_tracks[1].findall("automationclip")
    assert saved_curves[0].find("object").get("id") == source_id
    assert saved_curves[1].find("object").get("id") == copy_id
    studio.call("clone-load", "project.load", {"path": str(saved)})
    patterns("clone-pattern-after-load", 2)
    for clip, track in ((0, 0), (1, 2)):
        identity = volume(f"clone-restored-volume-{track}", track)
        curve = studio.call(f"clone-restored-curve-{clip}", "automation.inspect", {"track": 1, "clip": clip})["result"]
        assert len(curve["targets"]) == 1 and curve["targets"][0]["parameter"] == identity
    studio.call("clone-remove-pattern-row", "track.remove", {"container": "pattern", "track": 0})
    for track in range(4, 0, -1):
        studio.call(f"clone-remove-{track}", "track.remove", {"track": track})
    return {"trackCopy": cloned, "patternCopy": pattern_copy,
            "verification": "native instrument and full pattern duplication, distinct persisted automation targets, XML reload, history and cached replay"}


def verify_native_clip_copy(studio, evidence):
    studio.call("copy-destination", "track.add", {"type": "instrument", "name": "Copy destination"})
    before = studio.call("copy-before", "project.inspect")["result"]["tracks"][0]["clips"][0]
    args = {"track": 0, "clip": 0, "targetTrack": 1, "position": 384}
    result = studio.call("copy-native", "clip.clone", args)["result"]
    assert result == {"container": "song", "track": 1, "clip": 0, "position": 384, "replaced": False}, result

    def destination(identifier):
        return studio.call(identifier, "project.inspect")["result"]["tracks"][1]["clips"]

    copied = destination("copy-after")
    assert len(copied) == 1 and copied[0]["notes"] == before["notes"]
    assert copied[0]["length"] == before["length"] and copied[0]["offset"] == before["offset"]
    studio.call("copy-undo", "session.undo")
    assert destination("copy-after-undo") == []
    assert studio.call("copy-native", "clip.clone", args).get("replayed") is True
    assert destination("copy-after-replay") == []
    studio.call("copy-redo", "session.redo")
    assert destination("copy-after-redo") == copied

    studio.call("copy-pattern-track", "track.add", {"type": "pattern", "name": "Copy pattern"})
    studio.call("copy-pattern-row", "track.add", {"container": "pattern", "type": "instrument", "name": "Copied pattern notes"})
    into_pattern = {"track": 0, "clip": 0, "targetContainer": "pattern", "targetTrack": 0, "targetClip": 0, "replace": True}
    replaced = studio.call("copy-into-pattern", "clip.clone", into_pattern)["result"]
    assert replaced == {"container": "pattern", "track": 0, "clip": 0, "position": 0, "replaced": True}, replaced
    pattern = studio.call("copy-pattern-observe", "project.inspect")["result"]["patternTracks"][0]["clips"]
    assert len(pattern) == 1 and pattern[0]["notes"] == before["notes"]
    studio.call("copy-out-of-pattern", "clip.clone", {"container": "pattern", "track": 0, "clip": 0,
                "targetContainer": "song", "targetTrack": 1, "position": 768})
    assert destination("copy-after-pattern-export")[1]["notes"] == before["notes"]
    history = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    studio.call("copy-incompatible", "clip.clone", {"track": 0, "clip": 0, "targetTrack": 2, "position": 0}, ok=False)
    rejected = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    assert rejected["history"] == history["history"] and rejected["cursor"] == history["cursor"]
    studio.call("copy-note-edit", "note.edit", {"track": 1, "clip": 0, "note": 0, "key": 41})
    untouched = studio.call("copy-source-unchanged", "project.inspect")["result"]["tracks"][0]["clips"][0]
    assert untouched == before
    saved = evidence / "copied-clips.mmp"
    studio.call("copy-save", "project.save", {"path": str(saved)})
    saved_track = next(track for track in ET.parse(saved).getroot().findall(".//track") if track.get("name") == "Copy destination")
    assert len(saved_track.findall("midiclip")) == 2
    studio.call("copy-remove-pattern-row", "track.remove", {"container": "pattern", "track": 0})
    studio.call("copy-remove-pattern", "track.remove", {"track": 2})
    studio.call("copy-remove-destination", "track.remove", {"track": 1})
    return {"copy": result, "patternCopy": replaced,
            "verification": "native destination copy, pattern cell replacement, source independence, history/replay, incompatible target refusal and XML"}


def verify_native_recording_refusal(studio):
    declared = studio.call("recording-capability", "studio.capabilities")["result"]["capabilities"]
    recording = next(item for item in declared if item["name"] == "audio.record")
    assert recording["mutatesProject"] is True
    studio.call("recording-target-track", "track.add", {"type": "sample", "name": "Recording verification target"})
    studio.call("recording-target-clip", "clip.add", {"track": 1, "position": 0})
    original = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    args = {"track": 1, "clip": 0, "durationMs": 20, "maximumBytes": 8192, "timeoutMs": 1000}
    refused = studio.call("recording-no-device", "audio.record", args, ok=False)
    assert "capture-capable" in refused["error"], refused
    after = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
    assert after["history"] == original["history"] and after["cursor"] == original["cursor"]
    assert after["revision"] == original["revision"] + 1
    assert studio.call("recording-no-device", "audio.record", args, ok=False).get("replayed") is True
    current = studio.call("recording-after-refusal", "project.inspect")["result"]
    assert current["tracks"][1]["clips"][0]["sample"]["sourceFrames"] == 0
    assert current["transport"]["recording"]["reservedSampleClips"] == 0
    studio.call("recording-remove-fixture", "track.remove", {"track": 1})
    return {"refused": refused, "verification": "native executable dummy-input refusal, rollback and replay; no hardware recording"}


def verify_native_backend_startup(binary, evidence, first, studios):
    declared = first.call("backend-factories", "audio.backend.list")["result"]
    audio = declared["current"]["backend"]["engineReportedName"]
    midi = declared["current"]["midiBackend"]["engineReportedName"]
    assert audio in declared["audioBackends"] and midi in declared["midiBackends"], declared
    environment = os.environ.copy()
    for key in ("DISPLAY", "WAYLAND_DISPLAY", "QT_QPA_PLATFORM"):
        environment.pop(key, None)
    session = evidence / "backend-cli-rejected"
    original_config = (evidence / "config.xml").read_bytes()
    rejected_options = (
        ("--studio-live", "--studio-audio", "not-a-compiled-native-backend"),
        ("--studio-live", "--studio-midi", "not-a-compiled-native-backend"),
        ("--studio-audio", audio),
        ("--studio-midi", midi),
        ("--studio-live", "--studio-live"),
        ("--studio-live", "--studio-audio"),
        ("--studio-live", "--studio-midi"),
        ("--studio-live", "--studio-audio", audio, "--studio-audio", audio),
        ("--studio-live", "--studio-midi", midi, "--studio-midi", midi),
    )
    for options in rejected_options:
        failed = subprocess.run([str(binary), "studio", str(session), "--allowroot", *options],
                                input="", capture_output=True, text=True, encoding="utf-8", env=environment, timeout=20)
        assert failed.returncode != 0, (options, failed.stdout, failed.stderr)
        assert not session.exists(), "Rejected startup must not open a session"
    standalone = subprocess.run([str(binary), "--allowroot", "--studio-live"], input="", capture_output=True,
                                text=True, encoding="utf-8", env=environment, timeout=20)
    assert standalone.returncode != 0
    live = Studio(binary, evidence / "native-live-session", evidence,
                  ("--studio-live", "--studio-audio", audio, "--studio-midi", midi))
    studios.append(live)
    current = live.ready["transport"]
    assert current["backend"]["renderOnly"] is False and current["backend"]["kind"] == "dummy", current
    assert current["backend"]["startFailed"] is False and current["backend"]["running"] is True
    assert current["backend"]["processingFailure"] == "none" and current["backend"]["nativeError"] == 0
    assert current["backend"]["requestedName"] == audio and current["midiBackend"]["requestedName"] == midi
    assert current["midiBackend"]["kind"] == "dummy" and current["midiBackend"]["engineReportedName"] == midi
    chosen = live.call("live-factory-selection", "audio.backend.list")["result"]
    assert chosen["configuredAudioBackend"] == declared["configuredAudioBackend"], chosen
    assert chosen["configuredMidiBackend"] == declared["configuredMidiBackend"], chosen
    revision = live.revision
    graph = live.call("audio-graph-inspect", "audio.backend.inspect", {"direction": "output", "limit": 1})["result"]
    assert graph["scope"] == "master" and graph["supported"] is False and graph["observed"] is False
    assert graph["ports"] == [] and graph["channels"] == [] and graph["deviceReceiptVerified"] is False
    route = {"direction": "output", "channel": 0, "port": "missing-fixture:audio", "enabled": True}
    refused_graph = live.call("audio-graph-unsupported", "audio.backend.route", route, ok=False)
    assert "does not expose native master graph routing" in refused_graph["error"]
    assert live.call("audio-graph-unsupported", "audio.backend.route", route, ok=False).get("replayed") is True
    assert live.revision == revision
    live.call("native-bus-track", "track.add", {"type": "sample", "name": "Native bus capability probe"})
    revision = live.revision
    bus_graph = live.call("native-bus-inspect", "audio.bus.inspect", {"track": 0})["result"]
    assert bus_graph["scope"] == "track" and bus_graph["supported"] is False and bus_graph["observed"] is False
    assert bus_graph["published"] is False and bus_graph["channels"] == [] and bus_graph["deviceReceiptVerified"] is False
    bus_requests = (
        ("native-bus-configure", "audio.bus.configure", {"track": 0, "enabled": True}),
        ("native-bus-route", "audio.bus.route", {"track": 0, "channel": 0, "port": "missing-fixture:audio", "enabled": True}),
    )
    for request_id, operation, arguments in bus_requests:
        failure = live.call(request_id, operation, arguments, ok=False)
        assert "does not expose native track audio" in failure["error"], failure
        replay = live.call(request_id, operation, arguments, ok=False)
        assert replay.pop("replayed") is True and replay == failure
    assert live.revision == revision
    selection = live.call("runtime-backend-selection", "audio.backend.select", {"name": audio})
    assert selection["result"]["backend"]["running"] is True
    assert selection["result"]["backend"]["renderOnly"] is False
    assert selection["result"]["midiBackend"] == current["midiBackend"]
    assert live.revision == revision
    midi_selection = live.call("runtime-midi-selection", "midi.backend.select", {"name": midi})
    assert midi_selection["result"]["committed"] is True
    assert midi_selection["result"]["audioRestored"] is True
    assert midi_selection["result"]["previousBackend"] == midi
    assert midi_selection["result"]["outputAbandonment"]["abandoned"] is False
    assert midi_selection["result"]["outputAbandonment"]["deviceCleanupVerified"] is False
    assert midi_selection["result"]["current"]["midiBackend"]["engineReportedName"] == midi
    assert midi_selection["result"]["current"]["backend"]["running"] is True
    midi_output = midi_selection["result"]["current"]["midiBackend"]["outputBackend"]
    assert midi_output["abandonmentSupported"] is False and midi_output["abandonmentEligible"] is False
    midi_input = midi_selection["result"]["current"]["midiBackend"]["inputBackend"]
    assert midi_input["overrunObservationAvailable"] is False and midi_input["reportedOverrunBytes"] == "0"
    abandon_arguments = {"name": midi, "abandonFailedOutput": True}
    refused_abandonment = live.call("runtime-midi-abandon-refused", "midi.backend.select", abandon_arguments, ok=False)
    assert "terminal backend" in refused_abandonment["error"], refused_abandonment
    assert live.revision == revision
    live.call("live-start", "transport.play")
    time.sleep(0.1)
    clock = live.call("live-native-clock", "transport.inspect")["result"]
    assert clock["playing"] is True and clock["position"] > 0, clock
    refused_switch = live.call("runtime-backend-busy", "audio.backend.select", {"name": audio}, ok=False)
    live.call("runtime-midi-busy", "midi.backend.select", {"name": midi}, ok=False)
    live.call("live-stop", "transport.stop")
    replacement = live.call("runtime-backend-replacement", "audio.backend.select", {"name": audio})
    epoch = replacement["result"]["backend"]["inputErrorEpoch"]
    assert int(epoch) > int(selection["result"]["backend"]["inputErrorEpoch"])
    replay = live.call("runtime-backend-selection", "audio.backend.select", {"name": audio})
    assert replay.pop("replayed") is True and replay == selection
    assert live.call("runtime-backend-current", "transport.inspect")["result"]["backend"]["inputErrorEpoch"] == epoch
    midi_replay = live.call("runtime-midi-selection", "midi.backend.select", {"name": midi})
    assert midi_replay.pop("replayed") is True and midi_replay == midi_selection
    assert live.call("runtime-midi-current", "transport.inspect")["result"]["backend"]["inputErrorEpoch"] == epoch
    assert live.revision == revision
    live.shutdown()
    reopened = Studio(binary, live.session, evidence)
    studios.append(reopened)
    assert reopened.ready["transport"]["backend"]["renderOnly"] is True
    replay = reopened.call("runtime-backend-selection", "audio.backend.select", {"name": audio})
    assert replay.pop("replayed") is True and replay == selection
    assert reopened.call("backend-after-replay", "transport.inspect")["result"]["backend"]["renderOnly"] is True
    midi_replay = reopened.call("runtime-midi-selection", "midi.backend.select", {"name": midi})
    assert midi_replay.pop("replayed") is True and midi_replay == midi_selection
    assert reopened.call("midi-after-replay", "transport.inspect")["result"]["backend"]["renderOnly"] is True
    abandon_replay = reopened.call("runtime-midi-abandon-refused", "midi.backend.select", abandon_arguments, ok=False)
    assert abandon_replay.pop("replayed") is True and abandon_replay == refused_abandonment
    reopened.shutdown()
    # The CLI overrides belong to that process and must not save user configuration.
    def backend_attributes(raw):
        node = ET.fromstring(raw).find("audioengine")
        return {} if node is None else {key: node.get(key) for key in ("audiodev", "mididev") if key in node.attrib}
    assert backend_attributes((evidence / "config.xml").read_bytes()) == backend_attributes(original_config)
    return {"declared": declared, "selected": chosen, "clock": clock,
            "runtimeSelection": selection, "runtimeReplacement": replacement, "refusedBusySelection": refused_switch,
            "refusedMidiOutputAbandonment": refused_abandonment,
            "runtimeReplayCases": 2,
            "rejectedCliCases": len(rejected_options) + 1,
            "verification": "native executable live factory path and runtime selection with explicit dummy devices, busy refusal and receipt replay across process restart; physical hardware unverified"}


def assert_process_sample(sample, pid):
    assert sample["format"] == 1 and sample["scope"] == "lmms-process", sample
    assert sample["process"] == str(pid) and sample["instance"], sample
    for key in ("cpuUserMs", "cpuSystemMs"):
        assert isinstance(sample[key], (int, float)) and math.isfinite(sample[key]) and sample[key] >= 0, sample
    if os.name == "nt" or sys.platform.startswith(("linux", "darwin")):
        for key in ("residentBytes", "peakResidentBytes"):
            assert isinstance(sample[key], (int, float)) and math.isfinite(sample[key]) and sample[key] > 0, sample


def verify_native_resource_budgets(binary, evidence, studios):
    directory = evidence / "resource-budgets"
    studio = Studio(binary, directory, evidence)
    studios.append(studio)
    fixture = ProviderFixture(evidence / "resource-budget-unused-render.wav")
    try:
        sample = studio.call("resources", "studio.resources.inspect")["result"]
        assert_process_sample(sample, studio.process.pid)
        assert sample["residentBytes"] >= 1024 * 1024, "Budget fixture requires at least 1 MiB native resident memory"
        studio.call("baseline", "song.set", {"tempo": 91})
        studio.call("provider", "session.provider.set", {"role": "reasoning", "configuration": fixture.configuration})
        studio.call("chat", "session.chat", {"text": "Revise the project within my resource budget.",
                                             "maximumResidentMiB": 1, "maxOperations": 1})
        paused = studio.wait_job("paused")["job"]
        assert paused["rounds"] == 0 and paused["operations"] == 0, paused
        assert "resident-memory budget" in paused["lastError"] and not fixture.requests, paused
        assert studio.call("preserved", "project.inspect")["result"]["tempo"] == 91
        studio.call("resume", "agent.resume", {"maximumResidentMiB": 0})
        resumed = studio.wait_job("paused")["job"]
        assert resumed["operations"] == 1 and resumed["maximumResidentMiB"] == 0, resumed
        assert "native-operation budget" in resumed["lastError"], resumed
        assert studio.call("changed", "project.inspect")["result"]["tempo"] == 122
        assert not (evidence / "resource-budget-unused-render.wav").exists()
        studio.shutdown()
        recovered = Studio(binary, directory, evidence)
        studios.append(recovered)
        recovered_job = recovered.call("job", "agent.inspect")["result"]["job"]
        assert recovered_job["resourceUsage"] == resumed["resourceUsage"]
        assert recovered_job["maximumResidentMiB"] == 0 and recovered_job["maxOperations"] == 1
        recovered.shutdown()
        return {"initialPause": paused, "resumedPause": resumed,
                "scope": "actual native resident-memory admission and explicit resume; CPU limit scenarios are native unit fixtures"}
    finally:
        fixture.close()
        (evidence / "resource-budget-provider.json").write_text(json.dumps({
            "requestCount": len(fixture.requests), "errors": fixture.errors,
        }, indent=2), encoding="utf-8")
        assert not fixture.errors, fixture.errors


def verify_native_storage(binary, evidence, studios):
    directory = evidence / "storage-retirement"
    studio = Studio(binary, directory, evidence)
    studios.append(studio)
    studio.call("tempo-91", "song.set", {"tempo": 91})
    studio.call("tempo-123", "song.set", {"tempo": 123})
    before = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    redo_path = directory / before["history"][-1]["path"]
    studio.call("undo", "session.undo")
    assert redo_path.is_file()
    studio.call("branch", "song.set", {"tempo": 97})
    assert not redo_path.exists()
    info = studio.call("storage", "session.inspect")["result"]["storage"]
    assert info["lastCleanup"]["removed"] == 1
    large = {"memory": {"preference": "m" * 20000}}
    studio.call("large-receipt", "session.memory.set", large)
    state = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    archive = directory / state["requests"][-1]["archive"]["path"]
    assert archive.is_file()
    assert studio.call("large-receipt", "session.memory.set", large).get("replayed") is True
    for index in range(128):
        studio.call(f"small-{index}", "transport.inspect")
    assert not archive.exists()
    assert studio.call("small-127", "transport.inspect").get("replayed") is True
    studio.shutdown()
    recovered = Studio(binary, directory, evidence)
    studios.append(recovered)
    assert recovered.call("recovered", "project.inspect")["result"]["tempo"] == 97
    recovered.call("undo-new", "session.undo")
    assert recovered.call("undo-new", "session.undo").get("replayed") is True
    assert recovered.call("after-undo", "project.inspect")["result"]["tempo"] == 91
    recovered.call("redo-new", "session.redo")
    assert recovered.call("after-redo", "project.inspect")["result"]["tempo"] == 97
    after = json.loads((directory / "session.json").read_text(encoding="utf-8"))
    assert all((directory / entry["path"]).is_file() for entry in after["history"])
    recovered.shutdown()
    return {"expiredRedoRemoved": True, "expiredReceiptRemoved": True,
            "retainedUndoRedoAfterRestart": True, "nativeTempoAfterRestart": 97}


def verify_native_interrupts(binary, evidence, studios):
    # Windows CTRL_C_EVENT cannot target a child group. CTRL_BREAK_EVENT can;
    # never broadcast a console event to the test runner's own process group.
    signals = [("ctrl-break", signal.CTRL_BREAK_EVENT)] if os.name == "nt" else [
        ("sigint", signal.SIGINT), ("sigterm", signal.SIGTERM),
    ]
    results = []
    for name, number in signals:
        for held in (False, True):
            case = f"interrupt-{name}-{'inference' if held else 'idle'}"
            directory = evidence / case
            studio = Studio(binary, directory, evidence, isolate_interrupts=True)
            studios.append(studio)
            fixture = None
            try:
                studio.call("baseline", "song.set", {"tempo": 91})
                if held:
                    fixture = ProviderFixture(evidence / f"{case}.wav")
                    fixture.hold = True
                    studio.call("provider", "session.provider.set", {
                        "role": "reasoning", "configuration": fixture.configuration,
                    })
                    studio.chat_text("Consider a revision while I decide.")
                    assert fixture.started.wait(timeout=10), "Native inference did not start"
                before = json.loads((directory / "session.json").read_text(encoding="utf-8"))
                studio.trace.append({"signal": name})
                sent = time.monotonic()
                studio.process.send_signal(number)
                deadline = sent + 10
                while True:
                    response = studio.receive(timeout=max(0.01, deadline - time.monotonic()))
                    if response.get("event") == "shutdown":
                        assert response.get("ok") is True, response
                        assert response.get("reason") == "process interrupt", response
                        break
                    assert response.get("event") == "agent" or response.get("ok") is False, response
                    assert time.monotonic() < deadline, "Interrupt did not acknowledge shutdown"
                # Inference must unwind before the held provider is released.
                assert studio.process.wait(timeout=max(0.01, deadline - time.monotonic())) == 0
                elapsed = time.monotonic() - sent
                if fixture is not None:
                    fixture.release.set()
                studio.process.stdin.close()
                studio.reader.join(timeout=5)
                studio.stderr_reader.join(timeout=5)
                assert not studio.reader.is_alive() and not studio.stderr_reader.is_alive()
                while not studio.responses.empty():
                    remaining = studio.responses.get_nowait()
                    assert "protocolError" not in remaining, remaining
                    if "eof" not in remaining:
                        studio.trace.append({"response": remaining})
                assert sum(item.get("response", {}).get("event") == "shutdown" for item in studio.trace) == 1
                after = json.loads((directory / "session.json").read_text(encoding="utf-8"))
                assert after["history"] == before["history"] and after["revision"] == before["revision"]
                if held:
                    assert after["agent"]["job"]["status"] == "paused"
                    assert after["agent"]["job"]["operations"] == 0
                    assert not after["agent"]["job"]["pending"]
                recovered = Studio(binary, directory, evidence)
                studios.append(recovered)
                assert recovered.call("after-interrupt", "project.inspect")["result"]["tempo"] == 91
                if held:
                    assert recovered.call("job-after-interrupt", "agent.inspect")["result"]["job"]["status"] == "paused"
                recovered.shutdown()
                results.append({"signal": name, "duringInference": held, "exitCode": 0,
                                "shutdownSeconds": elapsed, "nativeTempoAfterRestart": 91})
            finally:
                if fixture is not None:
                    fixture.close()
                    (evidence / f"{case}-provider.json").write_text(json.dumps({
                        "requestCount": len(fixture.requests), "errors": fixture.errors,
                    }, indent=2), encoding="utf-8")
                    assert not fixture.errors, fixture.errors
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lmms", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary = args.lmms.resolve()
    if not binary.is_file():
        parser.error("A compiled native LMMS executable is required")
    # Use a fresh child per run so existing evidence and compositions survive.
    evidence = args.output.resolve() / str(time.time_ns())
    evidence.mkdir(parents=True)
    session = evidence / "session"
    studios = []
    fixture = None
    started = time.monotonic()
    try:
        first = Studio(binary, session, evidence)
        studios.append(first)
        backends = verify_native_backend_startup(binary, evidence, first, studios)
        interrupts = verify_native_interrupts(binary, evidence, studios)
        storage = verify_native_storage(binary, evidence, studios)
        resource_budgets = verify_native_resource_budgets(binary, evidence, studios)
        first.process.stdin.write('{"id":"bad-frame","op":"project.insX\bpect","args":{}}\n')
        first.process.stdin.flush()
        assert first.receive().get("ok") is False, "Pipe bytes must not be silently edited like console input"
        first.process.stdin.write("x" * (1024 * 1024 + 1) + "\n")
        first.process.stdin.flush()
        assert first.receive().get("ok") is False, "Oversized frames must be rejected"
        plugins = first.call("plugins", "plugin.list")["result"]["plugins"]
        instruments = [p for p in plugins if p["type"] == 0]
        assert len(instruments) >= 15, "Verification requires the full bundled instrument build"
        assert any(p["name"] == "tripleoscillator" for p in instruments)
        assert any(p["name"] == "audiofileprocessor" for p in instruments)
        assert any(p["name"] == "sf2player" for p in instruments), "Sound-bank verification requires the bundled FluidSynth player"
        assert any(p["name"] == "gigplayer" for p in instruments), "Sound-bank verification requires the bundled libgig player"
        assert any(p["name"] == "patman" for p in instruments), "GUS verification requires the bundled native Patman instrument"
        assert any(p["name"] == "slicert" for p in instruments), "Slicing verification requires the bundled native SlicerT instrument"
        assert any(p["name"] == "opulenz" for p in instruments), "OPL verification requires the bundled native OpulenZ instrument"
        first.call("new", "project.new", {"empty": True})
        first.call("tempo", "song.set", {"tempo": 120, "masterVolume": 80})
        first.call("track", "track.add", {"type": "instrument", "name": "Native lead"})
        duplicate = first.call("track", "track.add", {"type": "instrument", "name": "Native lead"})
        assert duplicate.get("replayed") is True
        first.call("instrument", "instrument.load", {"track": 0, "instrument": "tripleoscillator"})
        # Native instrument loading adopts the plugin display name. Set the
        # fixture name afterwards so later history/order checks test that name.
        first.call("track-name", "track.edit", {"track": 0, "name": "Native lead"})
        first.call("clip", "clip.add", {"track": 0, "position": 0})
        for index, key in enumerate([60, 64, 67, 72]):
            first.call("note-" + str(index), "note.add", {
                "track": 0, "clip": 0, "position": index * 48,
                "length": 48, "key": key, "volume": 65,
            })
        first.call("memory", "session.memory.set", {"memory": {"intent": "Keep this editable after recovery"}})
        state = first.call("inspect", "project.inspect")["result"]
        assert len(state["tracks"]) == 1
        assert len(state["tracks"][0]["clips"][0]["notes"]) == 4
        transport = verify_native_transport(first)
        recording = verify_native_recording_refusal(first)
        midi_configuration = verify_native_midi_configuration(first, evidence)
        track_move = verify_native_track_move(first, evidence)
        track_clone = verify_native_track_clone(first, evidence)
        clip_copy = verify_native_clip_copy(first, evidence)
        first.call("stale", "song.set", {"tempo": 200}, revision=0, ok=False)
        first.call("channel", "mixer.add", {"name": "Lead bus"})
        mixer_solo = verify_native_mixer_solo(first)
        mixer_move = verify_native_mixer_move(first, evidence)
        mixer_lifecycle = verify_native_mixer_removal_and_reset(first, evidence)
        effect_chain = verify_native_effect_chain(first, evidence)
        graphs = verify_native_graphs(first, evidence)
        expressions = verify_native_expressions(binary, evidence, studios)
        project = evidence / "composition.mmp"
        first.call("save", "project.save", {"path": str(project)})
        midi = evidence / "composition.mid"
        first.call("export-midi", "project.exportMidi", {"path": str(midi)})
        midi_bytes = midi.read_bytes()
        assert midi_bytes.startswith(b"MThd") and b"MTrk" in midi_bytes
        first.call("export-midi-fail", "project.exportMidi", {
            "path": str(evidence / "missing-parent" / "composition.mid"),
        }, ok=False)
        first.call("render-fail", "audio.render", {
            "path": str(evidence / "missing-parent" / "composition.wav"),
        }, ok=False)
        survived = first.call("after-render-failure", "project.inspect")["result"]
        assert len(survived["tracks"][0]["clips"][0]["notes"]) == 4
        audio = evidence / "composition.wav"
        render = first.call("render", "audio.render", {"path": str(audio), "bitDepth": 16})["result"]
        analysis = first.call("analyze", "audio.analyze", {"path": str(audio)})["result"]
        assert analysis["perceptualListening"] is False
        assert analysis["kind"] == "technical measurement"
        assert analysis["nonfiniteSamples"] == 0
        assert max(channel["rms"] for channel in analysis["channels"]) > 0.001
        with wave.open(str(audio), "rb") as decoded:
            assert decoded.getnchannels() == 2
            assert decoded.getsampwidth() == 2
            assert decoded.getframerate() == 44100
            assert decoded.getnframes() == analysis["frames"]
            samples = array.array("h", decoded.readframes(decoded.getnframes()))
            if sys.byteorder != "little":
                samples.byteswap()
            for channel in range(2):
                values = samples[channel::2]
                rms = math.sqrt(sum((value / 32768.0) ** 2 for value in values) / len(values))
                assert abs(rms - analysis["channels"][channel]["rms"]) < 0.00001

        fixture = ProviderFixture(evidence / "conversational.wav")
        resources_before = first.call("resources-before-agent", "studio.resources.inspect")["result"]
        assert_process_sample(resources_before, first.process.pid)
        for role in ("reasoning", "audio"):
            first.call("provider-" + role, "session.provider.set", {
                "role": role, "configuration": fixture.configuration,
            })
        first.chat_text("Revise the tempo, render and evaluate the editable piece. Preserve my idea: café, 音楽.")
        completed = first.wait_job("complete")
        assert completed["job"]["operations"] == 5, completed
        resource_usage = completed["job"]["resourceUsage"]
        assert resource_usage["transitions"] == completed["job"]["rounds"] + completed["job"]["operations"]
        assert resource_usage["cpuMeasuredTransitions"] == resource_usage["transitions"]
        assert resource_usage["cpuMs"] > 0 and resource_usage["wallMs"] > 0
        assert_process_sample(resource_usage["lastSample"], first.process.pid)
        observations = [json.loads(request["messages"][-1]["content"].split("\n", 1)[1])
                        for request in fixture.requests if isinstance(request["messages"][-1]["content"], str)]
        assert observations
        for observation in observations:
            assert_process_sample(observation["processResources"], first.process.pid)
        resources_after = first.call("resources-after-agent", "studio.resources.inspect")["result"]
        assert_process_sample(resources_after, first.process.pid)
        assert resources_after["cpuUserMs"] + resources_after["cpuSystemMs"] > resources_before["cpuUserMs"] + resources_before["cpuSystemMs"]
        assert first.call("resources-before-agent", "studio.resources.inspect").get("replayed") is True
        resource_verification_process = {"before": resources_before, "after": resources_after,
                                         "jobUsage": resource_usage, "providerObservations": len(observations),
                                         "scope": "current LMMS process; child plugins/external inference excluded"}
        assert (evidence / "conversational.wav").is_file()
        assert len(fixture.audio_observations) == 1
        assert not fixture.errors, fixture.errors
        fixture.hold = True
        first.chat_text("Consider another revision while I decide what to change.")
        assert fixture.started.wait(timeout=10), "Native inference did not start"
        first.call("cancel-during-inference", "agent.cancel")
        first.wait_job("cancelled")
        fixture.release.set()
        state = first.call("after-cancel", "project.inspect")["result"]
        assert state["tempo"] == 122, "Cancelled response must not execute its late tempo edit"
        first.call("restore-tempo", "song.set", {"tempo": 120})
        conversation = first.call("conversation", "conversation.history")["result"]
        assert "café, 音楽" in conversation["messages"][0]["content"]
        sample_verification = verify_native_samples(first, evidence)
        detuning_verification = verify_native_detuning(first, evidence)
        resource_verification = verify_native_resources(first, evidence)
        tuning_verification = verify_native_tuning(first, evidence)
        soundbank_verification = verify_native_soundbanks(first, evidence)
        patman_verification = verify_native_patman(first, evidence)
        slicer_verification = verify_native_slicer(first, evidence)
        opulenz_verification = verify_native_opulenz(first, evidence)
        first.shutdown()

        resumed = Studio(binary, session, evidence)
        studios.append(resumed)
        state = resumed.call("after-restart", "project.inspect")["result"]
        assert state["tempo"] == 120
        assert state["tracks"][0]["instrument"] == "tripleoscillator"
        assert len(state["tracks"][0]["clips"][0]["notes"]) == 4
        assert len(state["tracks"][1]["clips"]) == 2
        for clip in state["tracks"][1]["clips"]:
            assert_restored_media(resumed, clip["sample"], sample_verification["sourceWindow"])
        assert state["tracks"][2]["muted"]
        assert state["tracks"][2]["clips"][0]["automation"]["points"] == sample_verification["arrangementCurve"]["points"]
        assert state["tracks"][0]["clips"][0]["notes"][0]["detuning"]["points"] == detuning_verification["curve"]["points"]
        assert any(curve["points"] == sample_verification["globalCurve"]["points"] for curve in state["globalAutomations"])
        synth = resumed.inspect_parameters("resources-after-restart", "instrument.inspect", {"track": 0})["result"]
        wave_resource = next(item for item in synth["resources"] if item["key"] == "oscillator1")
        assert_owned_asset(resumed, wave_resource["path"], resource_verification["waveformPath"])
        assert wave_resource["selected"] and wave_resource["selectionValue"] == resource_verification["waveformSelection"]
        assert synth["arpeggioEnabled"]
        arp_type = next(p for p in synth["parameters"] if p["name"].endswith("Arpeggio type"))
        assert arp_type["manualValue"] == resource_verification["arpeggioChord"]["index"]
        assert state["tracks"][3]["instrument"] == "audiofileprocessor" and state["tracks"][3]["muted"]
        sampler = resumed.inspect_parameters("sampler-after-restart", "instrument.inspect", {"track": 3})["result"]
        assert_owned_asset(resumed, sampler["resources"][0]["path"], resource_verification["samplePath"])
        amplify = next(p for p in sampler["parameters"] if p["name"].endswith("Amplify"))
        assert amplify["manualValue"] == resource_verification["samplerAmplify"]
        rack = resumed.call("lfo-after-restart", "controller.list")["result"]["controllers"]
        restored_lfo = rack[resource_verification["controller"]]
        assert_restored_media(resumed, restored_lfo["userWave"], resource_verification["lfoWave"])
        wave_choice = next(p for p in restored_lfo["parameters"] if p["name"].endswith("Oscillator waveform"))
        assert wave_choice["manualValue"] == resource_verification["lfoSelection"]
        restored_scale = resumed.call("tuning-scale-after-restart", "tuning.scale.inspect", {"slot": 4})["result"]
        restored_map = resumed.call("tuning-map-after-restart", "tuning.keymap.inspect", {"slot": 4})["result"]
        assert restored_scale == tuning_verification["scale"] and restored_map == tuning_verification["keymap"]
        assert state["tracks"][4]["muted"] and state["tracks"][4]["clips"][0]["notes"][0]["key"] == 60
        restored_pitch = resumed.call("tuning-pitch-after-restart", "instrument.tuning.inspect", {"track": 4, "key": 60})["result"]["keys"][0]
        assert restored_pitch == tuning_verification["negativeKey"]
        restored_bank = resumed.call("bank-after-restart", "instrument.program.list", {"track": 5})["result"]
        assert restored_bank["programs"] == soundbank_verification["programs"]
        assert restored_bank["selection"] == soundbank_verification["selection"]
        bank_instrument = resumed.inspect_parameters("bank-resource-after-restart", "instrument.inspect", {"track": 5})["result"]
        assert_owned_asset(resumed, bank_instrument["resources"][0]["path"], soundbank_verification["path"])
        assert state["tracks"][5]["muted"] and state["tracks"][6]["muted"]
        # Keep this request within the native bounded replay window as coverage grows.
        restored_patch = resumed.inspect_parameters("patman-after-restart", "instrument.inspect", {"track": 7})["result"]
        assert_owned_asset(resumed, restored_patch["resources"][0]["path"], patman_verification["path"])
        assert state["tracks"][7]["muted"] and state["tracks"][7]["clips"][0]["notes"][0]["key"] == 81
        for name in ("Loop sample", "Tune sample"):
            controls = [item for item in restored_patch["parameters"] if item["name"].endswith(name)]
            assert len(controls) == 1 and controls[0]["manualValue"] == 1
        restored_slices = resumed.call("slicer-after-restart", "instrument.slices.inspect", {"track": 8})["result"]
        assert restored_slices == slicer_verification["layout"] and state["tracks"][8]["muted"]
        restored_opl = resumed.inspect_parameters("opulenz-after-restart", "instrument.inspect", {"track": 9})["result"]
        assert restored_opl["resources"][0]["path"] == opulenz_verification["path"] and restored_opl["programSelection"] == {}
        assert restored_opl["resources"][0]["requiredForRestore"] is False and state["tracks"][9]["muted"]
        assert not Path(opulenz_verification["originalImportPath"]).exists()
        replay = resumed.call("opulenz-final-source", "instrument.resource.set", {
            "track": 9, "resource": "patch", "path": opulenz_verification["path"],
        })
        assert replay.get("replayed") is True
        replayed_opl = resumed.inspect_parameters("opulenz-after-replay", "instrument.inspect", {"track": 9})["result"]
        multiplier = next(item for item in replayed_opl["parameters"] if item["name"].endswith("Op 2 frequency multiplier"))
        assert multiplier["manualValue"] == 3
        restored_opl_gm = resumed.call("opulenz-gm-after-restart", "instrument.program.list", {"track": 10})["result"]
        assert restored_opl_gm["selection"]["available"] and restored_opl_gm["selection"]["program"] == 42 and state["tracks"][10]["muted"]
        resumed.call("opulenz-recovery-isolate", "track.edit", {"track": 0, "muted": True})
        resumed.call("opulenz-recovery-unmute", "track.edit", {"track": 9, "muted": False})
        opl_audio = evidence / "opulenz-recovered.wav"
        resumed.call("opulenz-recovery-render", "audio.render", {"path": str(opl_audio), "sampleRate": 44100, "bitDepth": 16})
        opulenz_verification["restartAudio"] = measure_native_opl_wav(opl_audio)
        assert abs(opulenz_verification["restartAudio"]["frequency"] - 1320) < 5
        resumed.call("opulenz-recovery-mute", "track.edit", {"track": 9, "muted": True})
        resumed.call("opulenz-gm-recovery-unmute", "track.edit", {"track": 10, "muted": False})
        opl_gm_audio = evidence / "opulenz-gm-recovered.wav"
        resumed.call("opulenz-gm-recovery-render", "audio.render", {"path": str(opl_gm_audio), "sampleRate": 44100, "bitDepth": 16})
        opulenz_verification["restartGmAudio"] = measure_native_opl_wav(opl_gm_audio)
        assert abs(opulenz_verification["restartGmAudio"]["rms"] - opulenz_verification["gm"]["rms"]) < opulenz_verification["gm"]["rms"] * 0.05
        resumed.call("opulenz-gm-recovery-mute", "track.edit", {"track": 10, "muted": True})
        resumed.call("opulenz-recovery-restore-lead", "track.edit", {"track": 0, "muted": False})
        assert resumed.call("slicer-after-replay", "instrument.slices.inspect", {"track": 8})["result"]["points"] == [0, 0.25, 0.5, 1]
        resumed.call("slicer-recovery-isolate", "track.edit", {"track": 0, "muted": True})
        resumed.call("slicer-recovery-unmute", "track.edit", {"track": 8, "muted": False})
        slicer_audio = evidence / "slicer-recovered.wav"
        resumed.call("slicer-recovery-render", "audio.render", {"path": str(slicer_audio), "sampleRate": 44100, "bitDepth": 16})
        slicer_verification["restartAudio"] = measure_native_slicer_wav(slicer_audio)
        assert abs(slicer_verification["restartAudio"]["firstPitch"] - 440) < 3
        assert abs(slicer_verification["restartAudio"]["secondPitch"] - 880) < 3
        resumed.call("slicer-recovery-mute", "track.edit", {"track": 8, "muted": True})
        resumed.call("slicer-recovery-restore-lead", "track.edit", {"track": 0, "muted": False})
        resumed.call("patman-recovery-isolate", "track.edit", {"track": 0, "muted": True})
        resumed.call("patman-recovery-unmute", "track.edit", {"track": 7, "muted": False})
        recovered_audio = evidence / "patman-recovered.wav"
        resumed.call("patman-recovery-render", "audio.render", {"path": str(recovered_audio), "sampleRate": 44100, "bitDepth": 16})
        patman_verification["restartAudio"] = measure_native_patch_wav(recovered_audio)
        assert abs(patman_verification["restartAudio"]["frequency"] - 660) < 3
        resumed.call("patman-recovery-mute", "track.edit", {"track": 7, "muted": True})
        resumed.call("patman-recovery-restore-lead", "track.edit", {"track": 0, "muted": False})
        resumed.call("undo", "session.undo")
        resumed.call("redo", "session.redo")
        resumed.call("revise", "song.set", {"tempo": 125})
        resumed.kill()  # Recover the last acknowledged commit after abrupt termination.

        recovered = Studio(binary, session, evidence)
        studios.append(recovered)
        state = recovered.call("after-crash", "project.inspect")["result"]
        assert state["tempo"] == 125
        info = recovered.call("memory-after-crash", "session.inspect")["result"]
        assert info["memory"]["intent"] == "Keep this editable after recovery"
        recovered.call("continue", "note.edit", {"track": 0, "clip": 0, "note": 0, "key": 62})
        recovered.call("reload", "project.load", {"path": str(project)})
        state = recovered.call("loaded-project", "project.inspect")["result"]
        assert state["tempo"] == 120
        assert state["tracks"][0]["clips"][0]["notes"][0]["key"] == 60
        assert state["tuning"]["scales"][4]["degreeCount"] == 0
        assert state["tuning"]["keymaps"][4]["size"] == 0, "Loading the earlier project must replace the tuning library"
        recovered.call("new-for-import", "project.new", {"empty": True})
        recovered.call("import-midi", "project.import", {"path": str(midi)})
        imported = recovered.call("inspect-import", "project.inspect")["result"]
        imported_notes = [
            note for track in imported["tracks"] for clip in track["clips"]
            for note in clip.get("notes", [])
        ]
        assert len(imported_notes) >= 4
        recovered.shutdown()
        (evidence / "verification.json").write_text(json.dumps({
            "status": "passed", "scope": "native process integration; commission remains broader",
            "display": "absent", "render": render, "analysis": analysis, "transport": transport, "backends": backends, "recording": recording,
            "conversation": "deterministic provider fixture; native orchestration only",
            "audioSubmission": fixture.audio_observations, "perceptualQualityVerified": False,
            "samples": sample_verification, "detuning": detuning_verification, "instrumentResources": resource_verification,
            "tuning": tuning_verification,
            "midiConfiguration": midi_configuration,
            "trackMove": track_move,
            "trackClone": track_clone,
            "clipCopy": clip_copy,
            "mixerSolo": mixer_solo,
            "mixerMove": mixer_move,
            "mixerLifecycle": mixer_lifecycle,
            "effectChain": effect_chain,
            "nativeGraphs": graphs,
            "nativeExpressions": expressions,
            "interrupts": interrupts,
            "storageRetirement": storage,
            "processResources": resource_verification_process,
            "resourceBudgets": resource_budgets,
            "soundbanks": soundbank_verification,
            "patman": patman_verification,
            "slicer": slicer_verification,
            "opulenz": opulenz_verification,
            "wallTimeSeconds": time.monotonic() - started,
        }, indent=2), encoding="utf-8")
        print(str(evidence))
    finally:
        if fixture is not None:
            fixture.close()
            (evidence / "provider-fixture.json").write_text(json.dumps({
                "requestCount": len(fixture.requests), "audioSubmission": fixture.audio_observations,
                "errors": fixture.errors, "scope": "protocol fixture; not real musical inference",
            }, indent=2), encoding="utf-8")
        for index, studio in enumerate(studios):
            if studio.process.poll() is None:
                studio.kill()
            (evidence / f"trace-{index}.json").write_text(json.dumps(studio.trace, indent=2), encoding="utf-8")
            (evidence / f"stderr-{index}.log").write_text("\n".join(studio.errors), encoding="utf-8")


if __name__ == "__main__":
    main()
