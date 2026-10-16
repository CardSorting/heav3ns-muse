"""Retain evidence for a real native audio-critique/revision run.

Provider configuration stays in private environment variables and session files.
No mock provider is supplied. Structural checks do not certify musical quality.
"""
from __future__ import annotations

import argparse
import array
import hashlib
import json
import math
import os
from pathlib import Path
import sys
import time
import wave

from studio_process_test import Studio


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def measure(path):
    """Decode actual PCM independently of the studio's own audio.analyze."""
    with wave.open(str(path), "rb") as audio:
        require(audio.getsampwidth() == 2 and audio.getnchannels() == 2,
                "Acceptance audio must be stereo PCM16 WAV")
        frames, rate = audio.getnframes(), audio.getframerate()
        require(0 < frames <= rate * 120, "Acceptance audio must contain at most 120 seconds")
        pcm = audio.readframes(frames)
        require(len(pcm) == frames * 4, "PCM decode is incomplete")
    values = array.array("h", pcm)
    if sys.byteorder != "little":
        values.byteswap()
    rms = math.sqrt(sum((value / 32768.0) ** 2 for value in values) / len(values))
    require(math.isfinite(rms) and rms > 0.00001, "Acceptance audio must be finite and nonsilent")
    return {"sha256": digest(path), "pcmSha256": hashlib.sha256(pcm).hexdigest(),
            "frames": frames, "sampleRate": rate, "rms": rms}


def provider_configuration(name, *, audio=False):
    require(bool(name) and bool(os.environ.get(name)), "A provider configuration environment variable is required")
    try:
        config = json.loads(os.environ[name])
    except (ValueError, TypeError):
        raise ValueError("Provider configuration must contain JSON") from None
    require(isinstance(config, dict), "Provider configuration must be an object")
    require(not audio or config.get("protocol") == "chat", "Native audio input currently requires protocol chat")
    key_name = config.get("keyEnv")
    require(not key_name or (isinstance(key_name, str) and bool(os.environ.get(key_name))),
            "The configured credential environment variable is unavailable")
    # The native operation owns configuration validation and protocol semantics.
    return config


def native_call(record):
    call = record["call"]
    if call["name"] == "studio_execute":
        return call["args"]["op"], call["args"]["args"]
    return call["name"].replace("_", "."), call["args"]


def read_record(studio, job, call_id):
    offset, fingerprint, chunks = 0, None, []
    while True:
        arguments = {"job": job, "call": call_id, "offset": offset, "limit": 65536}
        if fingerprint:
            arguments["sha256"] = fingerprint
        request_tag = hashlib.sha256(call_id.encode()).hexdigest()[:16]
        result = studio.call(f"acceptance-record-{request_tag}-{offset}", "agent.history.read", arguments)["result"]
        require(result["offset"] == offset, "Native history returned a different offset")
        require(fingerprint is None or fingerprint == result["sha256"], "History changed during read")
        fingerprint = result["sha256"]
        chunks.append(result["text"].encode("utf-8"))
        if result["complete"]:
            payload = b"".join(chunks)
            require(len(payload) == result["totalBytes"] and hashlib.sha256(payload).hexdigest() == fingerprint,
                    "Native tool record fingerprint does not match")
            record = json.loads(payload)
            require(record["call"]["id"] == call_id, "Native history returned a different tool call")
            return record
        require(offset < result["nextOffset"] <= 16 * 1024 * 1024, "History did not make bounded progress")
        offset = result["nextOffset"]


def audit_revision(records, seed_audio, seed_digest, revised_audio, revised_project):
    """Check observed native tool ordering; never judge aesthetic merit."""
    listened, mutations, renders, saves = [], [], [], []
    previous_revision = None
    for index, record in enumerate(records):
        response = record["response"]
        revision = response.get("revision")
        changed = previous_revision is not None and revision is not None and revision > previous_revision
        if revision is not None:
            previous_revision = revision
        if response.get("ok") is not True:
            continue
        operation, args = native_call(record)
        result = response.get("result", {})
        if operation == "audio.listen" and Path(args.get("path", "")).resolve() == seed_audio.resolve():
            require(args.get("sourceSha256") == seed_digest, "Listening request must bind the seed render digest")
            require(result.get("audioSubmitted") is True and result.get("kind") == "provider audio evaluation",
                    "No accepted provider audio submission was retained")
            require(result.get("excerpt", {}).get("sourceSha256") == seed_digest,
                    "Listening result refers to different source audio")
            require(isinstance(result.get("judgment"), str) and result["judgment"].strip(),
                    "Provider critique is empty")
            listened.append((index, response["revision"], record["call"]["id"]))
        if listened and index > listened[0][0] and changed:
            if operation not in {"audio.render", "project.save", "session.undo", "session.redo",
                                 "project.new", "project.load", "project.import"}:
                mutations.append(index)
        if operation == "audio.render" and Path(args.get("path", "")).resolve() == revised_audio.resolve():
            renders.append((index, result.get("sha256")))
        if operation == "project.save" and Path(args.get("path", "")).resolve() == revised_project.resolve():
            saves.append(index)
    require(listened, "Agent did not submit seed audio to the configured audio provider")
    require(mutations, "No successful native revision was observed after critique")
    require(renders and renders[-1][0] > mutations[-1], "Agent did not render after its final revision")
    require(renders[-1][1] == digest(revised_audio), "Revised render receipt does not match output")
    require(saves and saves[-1] > mutations[-1], "Agent did not save after its final revision")
    return {"listenCall": listened[0][2], "nativeRevisionObserved": True,
            "renderAfterRevision": True, "projectSaveAfterRevision": True,
            "musicalQuality": "requires review of the retained critique and audio"}


def seed(studio, root):
    studio.call("seed-new", "project.new", {"empty": True})
    studio.call("seed-tempo", "song.set", {"tempo": 110, "masterVolume": 55})
    for track, name in enumerate(("Lead", "Chord bed")):
        studio.call(f"seed-track-{track}", "track.add", {"type": "instrument", "name": name})
        studio.call(f"seed-instrument-{track}", "instrument.load", {"track": track, "instrument": "tripleoscillator"})
        studio.call(f"seed-clip-{track}", "clip.add", {"track": track, "position": 0})
    for index, key in enumerate((60, 64, 67, 72, 71, 67, 64, 60)):
        studio.call(f"seed-lead-{index}", "note.add", {"track": 0, "clip": 0, "key": key,
                    "position": index * 48, "length": 44, "volume": 75})
    for index, key in enumerate((48, 55, 60)):
        studio.call(f"seed-chord-{index}", "note.add", {"track": 1, "clip": 0, "key": key,
                    "position": 0, "length": 384, "volume": 50})
    studio.call("seed-save", "project.save", {"path": str(root / "seed.mmp")})
    result = studio.call("seed-render", "audio.render", {"path": str(root / "seed.wav"), "bitDepth": 16})["result"]
    measured = measure(root / "seed.wav")
    require(result["sha256"] == measured["sha256"], "Seed render receipt does not match decoded audio")
    return measured


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lmms", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Private local artifact directory")
    parser.add_argument("--preflight", action="store_true", help="Native seed/save/render/reload only; never contact providers")
    parser.add_argument("--reasoning-config-env", help="Environment variable holding native reasoning-provider JSON")
    parser.add_argument("--audio-config-env", help="Environment variable holding native audio-provider JSON")
    args = parser.parse_args(argv)
    if not __debug__:
        parser.error("Assertions must be enabled for the shared process fixture")
    require(args.lmms.is_file(), "A compiled native executable is required")
    require(not args.output.resolve().is_relative_to(Path(__file__).resolve().parents[1] / "doc"),
            "Live session artifacts must remain outside public documentation")
    require(not args.preflight or not (args.reasoning_config_env or args.audio_config_env),
            "Preflight does not accept provider configuration")
    configs = {} if args.preflight else {
        "reasoning": provider_configuration(args.reasoning_config_env),
        "audio": provider_configuration(args.audio_config_env, audio=True)}
    root = args.output.resolve() / str(time.time_ns())
    root.mkdir(parents=True, mode=0o700)
    studios = []
    try:
        studio = Studio(args.lmms.resolve(), root / "session", root)
        studios.append(studio)
        original = seed(studio, root)
        if args.preflight:
            studio.shutdown()
            restored = Studio(args.lmms.resolve(), root / "reload-session", root)
            studios.append(restored)
            restored.call("preflight-load", "project.load", {"path": str(root / "seed.mmp")})
            project = restored.call("preflight-inspect", "project.inspect")["result"]
            require(project["tempo"] == 110 and len(project["tracks"]) == 2, "Seed project did not reload")
            restored.call("preflight-render", "audio.render", {"path": str(root / "reloaded.wav"), "bitDepth": 16})
            reloaded = measure(root / "reloaded.wav")
            require(reloaded["pcmSha256"] == original["pcmSha256"], "Seed audio changed on native project reload")
            restored.shutdown()
            report = {"status": "preflight-passed-provider-not-run", "seed": original,
                      "reloaded": reloaded, "providerCalls": 0, "gateCompleted": False}
        else:
            for role, config in configs.items():
                studio.call(f"configure-{role}", "session.provider.set", {"role": role, "configuration": config})
            revised_audio, revised_project = root / "revised.wav", root / "revised.mmp"
            intent = (
                f"Audition the existing two-track seed using audio_listen on {root / 'seed.wav'} with "
                f"sourceSha256 {original['sha256']}. Obtain qualitative musical, timbral or mix critique before editing. "
                "Use that returned critique to choose and execute a targeted audible native revision. "
                "Do not merely rename tracks or change metadata. Preserve an editable musical project. "
                f"After editing, save {revised_project} and render {revised_audio} as stereo PCM16 WAV. "
                "Use studio_capabilities and studio_execute for any operation without a direct tool declaration. "
                "Finish with agent_finish and cite successful listening, editing, saving and rendering call IDs. "
                "Explain why the musical edit addresses the critique and disclose subjective uncertainty. "
                "Do not claim musical acceptance if listening or native execution failed.")
            studio.call("live-critique-task", "session.chat", {"text": intent, "maxRounds": 24,
                        "maxOperations": 96, "maximumWallTimeMs": 600000})
            completed = studio.wait_job("complete")
            state = json.loads((studio.session / "session.json").read_text(encoding="utf-8"))
            job = state["agent"]["job"]
            records = []
            for entry in job["records"]:
                if entry["kind"] in {"tool", "archivedTool"}:
                    call_id = entry["call"]["id"] if entry["kind"] == "tool" else entry["id"]
                    records.append(read_record(studio, job["id"], call_id))
            (root / "native-tool-records.json").write_text(json.dumps(records, indent=2), encoding="utf-8")
            require(digest(root / "seed.wav") == original["sha256"], "Submitted seed audio was overwritten")
            audit = audit_revision(records, root / "seed.wav", original["sha256"], revised_audio, revised_project)
            revised = measure(revised_audio)
            require(revised["pcmSha256"] != original["pcmSha256"], "Revised audio is identical to the seed")
            studio.shutdown()
            restored = Studio(args.lmms.resolve(), root / "reload-session", root)
            studios.append(restored)
            restored.call("verify-revised-load", "project.load", {"path": str(revised_project)})
            restored.call("verify-revised-render", "audio.render", {"path": str(root / "reloaded.wav"), "bitDepth": 16})
            reloaded = measure(root / "reloaded.wav")
            restored.shutdown()
            report = {"status": "structural-checks-passed-musical-review-required", "gateCompleted": False,
                      "seed": original, "revised": revised, "reloaded": reloaded, "audit": audit,
                      "projectSha256": digest(revised_project), "job": completed["job"]}
        (root / "verification.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps({key: value for key, value in report.items() if key != "job"}, indent=2))
        return 0
    except Exception as error:
        (root / "failure.json").write_text(json.dumps({"status": "failed", "error": repr(error)}), encoding="utf-8")
        print("Acceptance did not complete. Details remain in the private output directory.", file=sys.stderr)
        return 1
    finally:
        for index, instance in enumerate(studios):
            if instance.process.poll() is None:
                instance.kill()
            (root / f"trace-{index}.json").write_text(json.dumps(instance.trace, indent=2), encoding="utf-8")
            (root / f"stderr-{index}.log").write_text("\n".join(instance.errors), encoding="utf-8")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError):
        print("Live acceptance prerequisites are unavailable; check private configuration and native build.", file=sys.stderr)
        raise SystemExit(2)
