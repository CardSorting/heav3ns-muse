"""Verify live acceptance evidence checks using synthetic records, never a live claim."""
import copy
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import wave

sys.path.insert(0, str(Path(__file__).resolve().parent))
import studio_live_acceptance as acceptance


class LiveAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.seed = self.root / "seed.wav"
        self.audio = self.root / "revised.wav"
        self.project = self.root / "revised.mmp"
        self.seed.write_bytes(b"seed fixture")
        self.audio.write_bytes(b"revised fixture")
        self.project.write_bytes(b"project fixture")
        self.seed_digest = acceptance.digest(self.seed)
        self.records = [
            self.record("listen", "audio_listen", {"path": str(self.seed), "sourceSha256": self.seed_digest}, 3,
                        {"audioSubmitted": True, "kind": "provider audio evaluation",
                         "excerpt": {"sourceSha256": self.seed_digest}, "judgment": "Synthetic critique for audit tests"}),
            self.record("edit", "note_edit", {"track": 0, "clip": 0, "note": 0, "key": 62}, 4),
            self.record("save", "project_save", {"path": str(self.project)}, 4),
            self.record("render", "audio_render", {"path": str(self.audio)}, 4, {"sha256": acceptance.digest(self.audio)}),
        ]

    def record(self, identifier, name, args, revision, result=None):
        return {"kind": "tool", "call": {"id": identifier, "name": name, "args": args},
                "response": {"ok": True, "revision": revision, "result": result or {}}}

    def audit(self):
        return acceptance.audit_revision(self.records, self.seed, self.seed_digest, self.audio, self.project)

    def test_ordered_native_evidence_requires_musical_review(self):
        result = self.audit()
        self.assertTrue(result["nativeRevisionObserved"])
        self.assertIn("requires review", result["musicalQuality"])

    def test_dispatch_tool_is_unwrapped_for_native_evidence(self):
        original = self.records[1]["call"]
        self.records[1]["call"] = {"id": original["id"], "name": "studio_execute",
                                    "args": {"op": "note.edit", "args": original["args"]}}
        self.assertTrue(self.audit()["nativeRevisionObserved"])

    def test_missing_or_failed_audio_submission_is_not_listening(self):
        for field, value in (("audioSubmitted", False), ("kind", "technical measurement"), ("judgment", " ")):
            with self.subTest(field=field):
                original = copy.deepcopy(self.records)
                self.records[0]["response"]["result"][field] = value
                with self.assertRaises(ValueError):
                    self.audit()
                self.records = original
        self.records[0]["response"]["ok"] = False
        with self.assertRaises(ValueError):
            self.audit()

    def test_input_and_output_source_digests_must_match(self):
        for source in (self.records[0]["call"]["args"], self.records[0]["response"]["result"]["excerpt"]):
            source["sourceSha256"] = "0" * 64
            with self.assertRaises(ValueError):
                self.audit()
            source["sourceSha256"] = self.seed_digest

    def test_a_revision_before_listening_does_not_satisfy_the_loop(self):
        self.records[0], self.records[1] = self.records[1], self.records[0]
        with self.assertRaises(ValueError):
            self.audit()

    def test_observation_after_project_replacement_is_not_an_edit(self):
        self.records[1]["call"]["name"] = "project_load"
        self.records.insert(2, self.record("inspect", "project_inspect", {}, 4))
        with self.assertRaises(ValueError):
            self.audit()

    def test_failed_edit_then_observation_is_not_a_successful_edit(self):
        self.records[1]["response"]["ok"] = False
        self.records.insert(2, self.record("inspect", "project_inspect", {}, 4))
        with self.assertRaises(ValueError):
            self.audit()

    def test_outputs_must_follow_final_edit(self):
        self.records.append(self.record("later-edit", "note_edit", {"key": 65}, 5))
        with self.assertRaises(ValueError):
            self.audit()

    def test_modified_audio_is_rejected(self):
        self.audio.write_bytes(b"tampered")
        with self.assertRaises(ValueError):
            self.audit()

    def test_multiple_revisions_can_replace_prior_render(self):
        self.records[3]["response"]["result"]["sha256"] = "old rendered bytes"
        self.records.extend([
            self.record("later-edit", "note_edit", {"key": 65}, 5),
            self.record("later-save", "project_save", {"path": str(self.project)}, 5),
            self.record("later-render", "audio_render", {"path": str(self.audio)}, 5, {"sha256": acceptance.digest(self.audio)})])
        self.assertTrue(self.audit()["renderAfterRevision"])

    def test_provider_prerequisites_do_not_disclose_credentials(self):
        config = {"protocol": "chat", "endpoint": "https://example.invalid/chat", "model": "configured-model", "keyEnv": "FIXTURE_SECRET"}
        with patch.dict(os.environ, {"FIXTURE_CONFIG": json.dumps(config)}, clear=True):
            with self.assertRaisesRegex(ValueError, "credential environment variable is unavailable"):
                acceptance.provider_configuration("FIXTURE_CONFIG", audio=True)
        with patch.dict(os.environ, {"FIXTURE_CONFIG": json.dumps(config), "FIXTURE_SECRET": "fixture-only"}, clear=True):
            self.assertEqual(acceptance.provider_configuration("FIXTURE_CONFIG", audio=True), config)

    def test_audio_configuration_refuses_responses_protocol(self):
        with patch.dict(os.environ, {"FIXTURE_CONFIG": '{"protocol":"responses"}'}, clear=True):
            with self.assertRaisesRegex(ValueError, "requires protocol chat"):
                acceptance.provider_configuration("FIXTURE_CONFIG", audio=True)

    def test_chunked_tool_record_is_verified_as_utf8_bytes(self):
        payload = json.dumps(self.records[0], ensure_ascii=False).encode()
        fingerprint = hashlib.sha256(payload).hexdigest()

        class History:
            def call(_, identifier, operation, args):
                start = args["offset"]
                end = min(start + 32, len(payload))
                return {"result": {"offset": start, "nextOffset": end, "complete": end == len(payload),
                                   "totalBytes": len(payload), "sha256": fingerprint,
                                   "text": payload[start:end].decode()}}
        self.assertEqual(acceptance.read_record(History(), "job", "listen"), self.records[0])

    def test_pcm_measurement_rejects_silence_and_detects_changed_samples(self):
        def write_wav(samples):
            with wave.open(str(self.audio), "wb") as output:
                output.setnchannels(2)
                output.setsampwidth(2)
                output.setframerate(44100)
                output.writeframes(samples)
        write_wav(b"\0" * 400)
        with self.assertRaisesRegex(ValueError, "nonsilent"):
            acceptance.measure(self.audio)
        write_wav(b"\x00\x10\x00\x10" * 100)
        first = acceptance.measure(self.audio)
        write_wav(b"\x00\x20\x00\x20" * 100)
        second = acceptance.measure(self.audio)
        self.assertNotEqual(first["pcmSha256"], second["pcmSha256"])


if __name__ == "__main__":
    unittest.main()
