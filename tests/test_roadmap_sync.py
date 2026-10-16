#!/usr/bin/env python3
"""Exercise roadmap transitions, retained evidence and generated-view drift."""

import contextlib
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "roadmap_sync", Path(__file__).resolve().parents[1] / "scripts/roadmap-sync.py")
roadmap = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(roadmap)


class RoadmapTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.register = {
            "version": 1, "last_updated": "2026-10-15T00:00:00Z", "active_horizon": "H0",
            "horizons": {f"H{i}": {"title": f"Horizon {i}", "description": "Scope",
                                  "status": "open", "completion_percentage": 0} for i in range(3)},
            "gates": [{"id": f"GATE-00{i + 1}", "horizon": f"H{i}", "title": "Gate",
                       "status": "open", "owner": "Maintainer", "criterion": "Native output",
                       "verification": "python3 tests/fixture.py", "evidence": "src/fixture.cpp"}
                      for i in range(3)]}
        self.write("src/fixture.cpp", "// implementation\n")
        self.write("doc/roadmap/events.jsonl", "")
        self.save(self.register)

    def write(self, name, content):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")

    def save(self, register):
        self.write("doc/roadmap/register.json", roadmap.dump_json(register))

    def load(self):
        return roadmap.read_json(self.root / "doc/roadmap/register.json")

    def events(self):
        return [json.loads(line) for line in (self.root / "doc/roadmap/events.jsonl").read_text().splitlines()]

    def run_cli(self, *args):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return roadmap.main(list(args), root=self.root)

    def receipt(self, gate="GATE-001"):
        self.write("doc/evidence/test-runs/output.txt", "passed fixture on local target\n")
        name = "doc/evidence/test-runs/run.json"
        self.record = {"gate": gate, "command": "python3 tests/fixture.py", "exit_code": 0,
                       "criterion": "Native output",
                       "executed_at": "2026-10-15T00:00:00Z", "platform": "Fixture platform",
                       "revision": "fixture-revision", "scope": "Native fixture", "limitations": "No hardware",
                       "verified_invariants": ["Native output"],
                       "output": {"path": "doc/evidence/test-runs/output.txt",
                                  "sha256": hashlib.sha256((self.root / "doc/evidence/test-runs/output.txt").read_bytes()).hexdigest()}}
        self.write(name, roadmap.dump_json(self.record))
        self.register_receipt(name)
        return name

    def register_receipt(self, name):
        self.write("doc/evidence/registry.json", roadmap.dump_json({"receipts": {name: {
            "gate": self.record["gate"], "command": self.record["command"], "status": "verified",
            "sha256": hashlib.sha256((self.root / name).read_bytes()).hexdigest()}}}))

    def test_sync_is_idempotent_and_check_is_read_only(self):
        self.assertEqual(self.run_cli("sync"), 0)
        before = {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in self.root.rglob("*") if p.is_file()}
        self.assertEqual(self.run_cli("check"), 0)
        self.assertEqual(self.run_cli("sync"), 0)
        self.assertEqual(before, {p: (p.read_bytes(), p.stat().st_mtime_ns) for p in before})

    def test_status_tracks_work_at_zero_percent_and_reopening(self):
        register = copy.deepcopy(self.register)
        register["gates"][0]["status"] = "in-progress"
        self.assertEqual(roadmap.normalize(register)["horizons"]["H0"]["status"], "in-progress")
        register["horizons"]["H0"].update(status="completed", completion_percentage=100)
        register["gates"][0]["status"] = "open"
        self.assertEqual(roadmap.normalize(register)["horizons"]["H0"]["status"], "open")

    def test_empty_horizon_never_auto_completes(self):
        self.register["gates"] = []
        self.assertEqual(roadmap.normalize(self.register)["active_horizon"], "H0")

    def test_invalid_completion_has_no_side_effects(self):
        for args in [("--gate", "GATE-001", "--status", "completed"),
                     ("--gate", "missing", "--status", "open")]:
            before = self.load()
            self.assertEqual(self.run_cli("roll", *args, "--note", "Review"), 1)
            self.assertEqual(before, self.load())
            self.assertEqual(self.events(), [])
            self.assertFalse((self.root / "ROADMAP.md").exists())

    def test_completion_roll_and_reopen_append_horizon_events(self):
        receipt = self.receipt()
        self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "completed",
                                      "--receipt", receipt, "--note", "Reviewed criterion and scope"), 0)
        self.assertEqual(self.load()["active_horizon"], "H1")
        self.assertEqual([e.get("type") for e in self.events()], [None, "horizon-changed"])
        self.assertEqual(self.run_cli("check"), 0)
        self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "open", "--note", "Regression"), 0)
        gate = self.load()["gates"][0]
        self.assertIsNone(gate["completed_at"])
        self.assertNotIn("completion_receipt", gate)
        self.assertEqual(self.load()["active_horizon"], "H0")
        self.assertEqual(len(self.events()), 4)
        self.assertEqual(self.events()[0]["completion_receipt"], receipt)
        self.assertEqual(self.run_cli("check"), 0)

    def test_unchanged_roll_does_not_append_duplicate_events(self):
        self.assertEqual(self.run_cli("sync"), 0)
        self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "open", "--note", "Unchanged"), 0)
        self.assertEqual(self.events(), [])

    def test_every_horizon_complete_has_no_active_horizon(self):
        for g in self.register["gates"]:
            g["status"] = "completed"
        self.assertIsNone(roadmap.normalize(self.register)["active_horizon"])

    def test_legacy_completion_requires_matching_historical_event(self):
        gate = self.register["gates"][0]
        gate.update(status="completed", completed_at="2026-10-12")
        self.save(self.register)
        self.assertEqual(self.run_cli("sync"), 1)
        event = {"event_id": "old-event", "timestamp": "2026-10-12T12:00:00Z",
                 "gate_id": gate["id"], "status": "completed"}
        self.write("doc/roadmap/events.jsonl", json.dumps(event) + "\n")
        self.assertEqual(self.run_cli("sync"), 0)
        self.assertEqual(self.run_cli("check"), 0)
        gate["completed_at"] = "2026-10-13"
        self.save(self.register)
        self.assertEqual(self.run_cli("sync"), 1)

    def test_direct_new_completion_without_receipt_fails(self):
        self.register["gates"][0].update(status="completed", completed_at="2026-10-16")
        self.save(self.register)
        self.assertEqual(self.run_cli("check"), 1)

    def test_reopening_prevents_reusing_an_old_completion_date(self):
        gate = self.register["gates"][0]
        gate.update(status="completed", completed_at="2026-10-12")
        events = [{"event_id": "old", "timestamp": "2026-10-12T00:00:00Z",
                   "gate_id": gate["id"], "status": "completed"},
                  {"event_id": "reopened", "timestamp": "2026-10-16T00:00:00Z",
                   "gate_id": gate["id"], "status": "open"}]
        self.assertFalse(roadmap.legacy_closure(gate, events))

    def test_receipt_invariants_are_required_even_when_aggregate_tests_pass(self):
        name = self.receipt()
        self.record.pop("verified_invariants")
        self.record["metrics"] = {"passed": 193, "failed": 0}
        self.write(name, roadmap.dump_json(self.record))
        self.register_receipt(name)
        self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "completed",
                                      "--receipt", name, "--note", "Aggregate pass"), 1)

    def test_successful_horizon_roll_is_not_duplicated_by_sync(self):
        name = self.receipt()
        self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "completed",
                                      "--receipt", name, "--note", "Reviewed"), 0)
        events = self.events()
        self.assertEqual(self.run_cli("sync"), 0)
        self.assertEqual(self.events(), events)

    def test_receipt_rejects_failure_mismatch_and_missing_observation_fields(self):
        for field, value in [("exit_code", 1), ("exit_code", False), ("gate", "GATE-002"),
                             ("criterion", "Different criterion"),
                             ("command", "true"), ("platform", ""), ("revision", ""),
                             ("scope", ""), ("limitations", ""), ("verified_invariants", []),
                             ("executed_at", "2099-01-01T00:00:00Z"),
                             ("executed_at", "2026-10-15T00:00:00")]:
            with self.subTest(field=field, value=value):
                name = self.receipt()
                self.record[field] = value
                self.write(name, roadmap.dump_json(self.record))
                self.register_receipt(name)
                self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "completed",
                                              "--receipt", name, "--note", "Review"), 1)
                self.assertEqual(self.events(), [])

    def test_receipt_and_execution_output_tampering_fail_check(self):
        name = self.receipt()
        self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "completed",
                                      "--receipt", name, "--note", "Reviewed"), 0)
        self.write(name, roadmap.dump_json({**self.record, "scope": "Altered"}))
        self.assertEqual(self.run_cli("check"), 1)
        self.write(name, roadmap.dump_json(self.record))
        self.write("doc/evidence/test-runs/output.txt", "changed\n")
        self.assertEqual(self.run_cli("check"), 1)

    def test_invalid_completion_can_be_reopened_without_erasing_history(self):
        name = self.receipt()
        self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "completed",
                                      "--receipt", name, "--note", "Reviewed"), 0)
        original_event = self.events()[0]
        self.write("doc/evidence/test-runs/output.txt", "changed\n")
        self.assertEqual(self.run_cli("check"), 1)
        self.assertEqual(self.run_cli("roll", "--gate", "GATE-001", "--status", "open",
                                      "--note", "Evidence no longer validates"), 0)
        self.assertEqual(self.events()[0], original_event)
        self.assertEqual(self.run_cli("check"), 0)

    def test_safe_paths_reject_traversal_absolute_and_symlink_escape(self):
        with tempfile.TemporaryDirectory() as external:
            outside = Path(external) / "outside.json"
            outside.write_text("{}")
            (self.root / "escape.json").symlink_to(outside)
            for name in ("../outside.json", str(outside), "escape.json"):
                with self.subTest(name=name), self.assertRaises(ValueError):
                    roadmap.safe_file(self.root, name)

    def test_drift_in_each_generated_view_and_derived_state_fails(self):
        for name in ("ROADMAP.md", "doc/roadmap/OPEN_ITEMS.md"):
            self.assertEqual(self.run_cli("sync"), 0)
            self.write(name, "stale\n")
            self.assertEqual(self.run_cli("check"), 1)
        self.assertEqual(self.run_cli("sync"), 0)
        register = self.load()
        register["active_horizon"] = "H2"
        self.save(register)
        self.assertEqual(self.run_cli("check"), 1)

    def test_register_rejects_duplicate_gate_and_event_ids(self):
        self.register["gates"].append(copy.deepcopy(self.register["gates"][0]))
        self.save(self.register)
        self.assertEqual(self.run_cli("sync"), 1)
        self.register["gates"].pop()
        self.save(self.register)
        event = {"event_id": "duplicate", "timestamp": "2026-10-15T00:00:00Z"}
        self.write("doc/roadmap/events.jsonl", (json.dumps(event) + "\n") * 2)
        self.assertEqual(self.run_cli("sync"), 1)

    def test_horizons_are_ordered_numerically(self):
        self.register["horizons"]["H10"] = self.register["horizons"].pop("H0")
        self.register["gates"][0]["horizon"] = "H10"
        self.assertEqual(roadmap.normalize(self.register)["active_horizon"], "H1")


if __name__ == "__main__":
    unittest.main()
