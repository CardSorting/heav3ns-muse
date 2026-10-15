#!/usr/bin/env python3
"""Unit tests for risk-based change impact analyzer and test selector."""

import importlib.util
from pathlib import Path
import unittest

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "scripts/change-impact.py"
spec = importlib.util.spec_from_file_location("change_impact", SCRIPT_PATH)
ci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci)


class ChangeImpactTests(unittest.TestCase):
    def test_doc_only_changes_skip_heavy_build(self):
        files = [
            "doc/native-studio-protocol.md",
            "doc/roadmap/OPEN_ITEMS.md",
            "README.md"
        ]
        impact = ci.analyze_impact(files)
        self.assertTrue(impact["hygiene_required"])
        self.assertTrue(impact["knowledge_required"])
        self.assertTrue(impact["security_required"])
        self.assertFalse(impact["build_required"])
        self.assertFalse(impact["regression_required"])
        self.assertIn("Documentation and metadata changes only", impact["rationale"])

    def test_runtime_cpp_changes_require_build_and_regression(self):
        files = [
            "src/core/StudioCapabilities.cpp",
            "include/StudioCapabilities.h"
        ]
        impact = ci.analyze_impact(files)
        self.assertTrue(impact["build_required"])
        self.assertTrue(impact["regression_required"])
        self.assertIn("Native studio runtime", impact["rationale"])

    def test_build_system_changes_require_full_verification(self):
        files = [
            "CMakeLists.txt",
            ".github/workflows/ci.yml"
        ]
        impact = ci.analyze_impact(files)
        self.assertTrue(impact["build_required"])
        self.assertTrue(impact["regression_required"])

    def test_unclassified_files_trigger_conservative_failsafe(self):
        files = [
            "unexpected/unknown_file.xyz"
        ]
        impact = ci.analyze_impact(files)
        self.assertTrue(impact["build_required"])
        self.assertTrue(impact["regression_required"])
        self.assertIn("Unclassified files detected", impact["rationale"])

    def test_empty_diff_defaults_to_clean_baseline(self):
        impact = ci.analyze_impact([])
        self.assertFalse(impact["build_required"])
        self.assertFalse(impact["regression_required"])


if __name__ == "__main__":
    unittest.main()
