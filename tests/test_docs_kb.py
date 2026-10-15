#!/usr/bin/env python3
"""Comprehensive unit test suite for HEAV3NS-MUSE knowledgebase guards and search."""

import copy
from datetime import date
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

# Import docs-kb module dynamically from scripts/docs-kb.py
SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts/docs-kb.py"
spec = importlib.util.spec_from_file_location("docs_kb", SCRIPT_PATH)
kb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kb)


class KnowledgeGuardTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()

        self.guide = "doc/how-to/studio-guide.md"
        self.adr = "doc/adr/0001-test-decision.md"
        self.runbook = "doc/runbooks/test-recovery.md"
        self.evidence = "doc/evidence/test-runs/receipt.json"

        self.write(self.guide, "# Studio Guide\n\n<!-- docs-scope: current-guide -->\n\nGuide content.\n")
        self.write(self.adr, "# Test Decision\n\n<!-- docs-scope: decision-record -->\n\nDecision content.\n")
        self.write("src/core/test_source.cpp", "// Original C++ source\n")
        self.write("package.json", '{"name":"test-repo","scripts":{"test:studio":"fixture"}}')
        self.write(self.evidence, '{"gate":"GATE-001","status":"verified"}')

        # Canonical runbook with all 6 required sections
        runbook_content = (
            "# Runbook: Test Recovery\n\n<!-- docs-scope: current-guide -->\n\n"
            "## Purpose and trigger\nTrigger.\n\n"
            "## Authority and prerequisites\nPrereqs.\n\n"
            "## Procedure\nSteps.\n\n"
            "## Stop conditions\nStop.\n\n"
            "## Recovery and rollback\nRollback.\n\n"
            "## Verification and evidence\nVerification.\n"
        )
        self.write(self.runbook, runbook_content)

        self.domain = {
            "title": "Studio Core",
            "owner": "Core Maintainers",
            "sources": ["src/core/**/*.cpp"],
            "guides": [self.guide],
            "checks": ["python3 tests/studio_process_test.py"]
        }
        self.record = {
            "status": "accepted",
            "delivery": "verified",
            "domain": "studio-core",
            "owner": "Core Maintainers",
            "date": "2026-10-12",
            "evidence": [self.evidence],
            "supersedes": []
        }
        self.config = {
            "version": 1,
            "require_domain_reviews": True,
            "require_review_evidence": True,
            "managed_adr_from": 1,
            "domains": {"studio-core": self.domain},
            "documents": {},
            "runbooks": [self.runbook],
            "decisions": {self.adr: self.record}
        }

    def write(self, name, content):
        p = self.root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content, encoding="utf-8")

    def review(self):
        return {
            "reviewer": "Test Reviewer",
            "note": "Fixture review attestation",
            "reviewed_at": "2026-10-12",
            "review_by": "2026-11-12",
            "files": kb.fingerprints(self.root, self.domain),
            "evidence": {self.evidence: "fc69a589cfca63ff"}
        }

    def test_safe_file_prevents_directory_traversal(self):
        for unsafe in ["../outside.txt", "/tmp/forbidden", "doc/../../escape"]:
            with self.subTest(path=unsafe), self.assertRaises(ValueError):
                kb.safe_file(self.root, unsafe)

    def test_missing_runbook_section_fails_validation(self):
        # Remove a required section from runbook
        bad_runbook = (
            "# Runbook: Broken\n\n<!-- docs-scope: current-guide -->\n\n"
            "## Purpose and trigger\nTrigger.\n\n"
            "## Procedure\nMissing sections.\n"
        )
        self.write(self.runbook, bad_runbook)
        errors = kb.validate(self.root, self.config, {})
        self.assertTrue(any("missing runbook section" in e for e in errors))

    def test_unregistered_runbook_fails_validation(self):
        self.write("doc/runbooks/unregistered.md", "# Unregistered\n\n<!-- docs-scope: current-guide -->\n")
        errors = kb.validate(self.root, self.config, {})
        self.assertTrue(any("new runbook requires registration" in e for e in errors))

    def test_adr_supersession_requires_reciprocity(self):
        adr2 = "doc/adr/0002-successor.md"
        self.write(adr2, "# Successor Decision\n\n<!-- docs-scope: decision-record -->\n")
        self.config["decisions"][adr2] = copy.deepcopy(self.record)

        # Mark adr 1 superseded by adr 2, but adr 2 does not reciprocate
        self.record.update(status="superseded", superseded_by=adr2)
        errors = kb.validate(self.root, self.config, {})
        self.assertTrue(any("reciprocate supersession" in e for e in errors))

    def test_source_modification_invalidates_review(self):
        rev = self.review()
        today = date(2026, 10, 15)
        # Initially fresh
        rev["evidence"][self.evidence] = kb.hashlib.sha256((self.root / self.evidence).read_bytes()).hexdigest()
        self.assertEqual(kb.review_state(self.root, self.domain, rev, today=today), "reviewed")

        # Mutate source file
        self.write("src/core/test_source.cpp", "// Mutated source code\n")
        self.assertEqual(kb.review_state(self.root, self.domain, rev, today=today), "stale")

    def test_search_engine_ranking(self):
        self.write("doc/how-to/synth.md", "# Synthesizer Envelope Control\n\n<!-- docs-scope: current-guide -->\n\nDetails on ADSR envelope shaping.\n")
        self.write("doc/how-to/mixer.md", "# Mixer Channels and Volume\n\n<!-- docs-scope: current-guide -->\n\nDetails on mixer gain and routing.\n")

        results = kb.search(self.root, self.config, "synthesizer envelope")
        self.assertGreater(len(results), 0)
        self.assertEqual(results[0]["path"], "doc/how-to/synth.md")
        self.assertIn("envelope", results[0]["title"].lower())

    def test_unmapped_core_studio_source_fails_validation(self):
        # Restrict sources to specific file so StudioUnmappedExample is unmapped
        self.domain["sources"] = ["src/core/test_source.cpp"]
        unmapped_file = "src/core/StudioUnmappedExample.cpp"
        self.write(unmapped_file, "// Unmapped studio source\n")
        errors = kb.validate(self.root, self.config, {})
        self.assertTrue(any("Unmapped core studio sources detected" in e for e in errors))

    def test_agent_anchor_invariants_compiled(self):
        gen = kb.generated(self.root, self.config)
        self.assertIn("doc/knowledge/agent-anchor.json", gen)
        anchor = json.loads(gen["doc/knowledge/agent-anchor.json"])
        self.assertIn("invariants", anchor)
        self.assertIn("model_authority", anchor["invariants"])
        self.assertIn("capture_safety", anchor["invariants"])
        self.assertIn("capability_namespaces", anchor)
        self.assertIn("recording", anchor["capability_namespaces"])


if __name__ == "__main__":
    unittest.main()

