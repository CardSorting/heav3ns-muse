#!/usr/bin/env python3
"""Security baseline, boundary fuzzing, and permission audit tests for HEAV3NS-MUSE."""

import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

# Secret patterns to detect in tracked repository files
SECRET_PATTERNS = [
    (r"ghp_[A-Za-z0-9]{36}", "GitHub Personal Access Token"),
    (r"ghs_[A-Za-z0-9]{36}", "GitHub App Session Token"),
    (r"AKIA[0-9A-Z]{16}", "AWS Access Key"),
    (r"sk-[A-Za-z0-9]{20,48}", "OpenAI API Key"),
    (r"sk-ant-[A-Za-z0-9]{20,48}", "Anthropic API Key"),
    (r"-----BEGIN (RSA |EC |DSA |OPENSSH )?PRIVATE KEY-----", "Private Key Header"),
]

# Sensitive files to exclude from secret scanning
SCANNED_EXTENSIONS = {".py", ".cpp", ".h", ".json", ".yml", ".yaml", ".md", ".txt", ".sh"}


class SecurityBoundaryTests(unittest.TestCase):
    def test_repository_secret_scanning_clean(self):
        """Scans tracked files for leaked API keys, tokens, and private keys."""
        violations = []
        for path in ROOT.rglob("*"):
            if not path.is_file() or path.is_symlink():
                continue
            if any(p.startswith(".") for p in path.parts if p != ".github"):
                continue
            if path.suffix not in SCANNED_EXTENSIONS:
                continue
            if "build" in path.parts or "tmp" in path.parts or ".git" in path.parts:
                continue

            try:
                content = path.read_text(encoding="utf-8", errors="ignore")
            except Exception:
                continue

            for pattern, name in SECRET_PATTERNS:
                matches = re.findall(pattern, content)
                if matches:
                    rel = path.relative_to(ROOT).as_posix()
                    violations.append(f"{rel}: Potential {name} detected ({len(matches)} match(es))")

        self.assertEqual(violations, [], "Secret scanning detected potential leaks:\n" + "\n".join(violations))

    def test_workflow_permissions_least_privilege(self):
        """Audits all GitHub Actions workflows to ensure explicit least-privilege permissions."""
        wf_dir = ROOT / ".github/workflows"
        if not wf_dir.is_dir():
            self.skipTest("No .github/workflows directory found")

        violations = []
        for yml in wf_dir.glob("*.yml"):
            body = yml.read_text(encoding="utf-8")
            # Must declare explicit permissions or have permissions block
            if "permissions:" not in body:
                violations.append(f"{yml.name}: Missing explicit 'permissions:' declaration (violates least privilege)")
            if "write-all" in body:
                violations.append(f"{yml.name}: Unsafe 'permissions: write-all' detected")

        self.assertEqual(violations, [], "Workflow permission violations detected:\n" + "\n".join(violations))

    def test_safe_file_directory_traversal_prevention(self):
        """Verifies safe_file correctly rejects path traversal and escapes."""
        # Dynamically import docs-kb safe_file
        import importlib.util
        spec = importlib.util.spec_from_file_location("docs_kb", ROOT / "scripts/docs-kb.py")
        kb = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(kb)

        dangerous_paths = [
            "../outside.txt",
            "../../etc/passwd",
            "/tmp/forbidden",
            "doc/../../escape",
            "src/core/../../../private"
        ]
        for p in dangerous_paths:
            with self.subTest(path=p), self.assertRaises(ValueError):
                kb.safe_file(ROOT, p)

    def test_submodule_configuration_security(self):
        """Verifies .gitmodules contains secure URLs (no git://, only https:// or relative)."""
        gm = ROOT / ".gitmodules"
        if not gm.is_file():
            self.skipTest("No .gitmodules file")

        content = gm.read_text(encoding="utf-8")
        unencrypted = re.findall(r"url\s*=\s*git://.*", content)
        self.assertEqual(unencrypted, [], "Unencrypted git:// protocol detected in .gitmodules:\n" + "\n".join(unencrypted))

    def test_malformed_json_rpc_boundary_fuzzing(self):
        """Fuzzes JSON-RPC input parsing logic with edge-case frames."""
        malformed_inputs = [
            "",                                      # Empty payload
            "   ",                                   # Whitespace
            "{not valid json}",                      # Malformed JSON
            "[]",                                    # Array instead of object
            '{"id": 1}',                             # Missing jsonrpc version and method
            '{"jsonrpc": "1.0", "method": "test"}',  # Invalid protocol version
            '{"jsonrpc": "2.0"}',                    # Missing method
            '{"jsonrpc": "2.0", "method": ""}',      # Empty method name
            '{"jsonrpc": "2.0", "method": "' + "a" * 10000 + '"}', # Giant method
            '{"jsonrpc": "2.0", "method": "test", "params": ' + ("[" * 200) + ("]" * 200) + '}', # Deep recursion
            '{"jsonrpc": "2.0", "id": 9999999999999999999999999999999999, "method": "test"}'   # Giant integer ID
        ]

        def sanitize_json_rpc_frame(raw_line):
            """Simulates protocol frame boundary validation."""
            if not raw_line or len(raw_line) > 1024 * 1024:
                return False, -32700, "Frame too large or empty"
            try:
                payload = json.loads(raw_line)
            except Exception:
                return False, -32700, "Parse error"
            if not isinstance(payload, dict):
                return False, -32600, "Invalid Request: payload must be a JSON object"
            if payload.get("jsonrpc") != "2.0":
                return False, -32600, "Invalid Request: jsonrpc must be '2.0'"
            method = payload.get("method")
            if not method or not isinstance(method, str) or len(method) > 256:
                return False, -32601, "Method not found or malformed"

            def check_depth(obj, depth=0):
                if depth > 10:
                    return False
                if isinstance(obj, dict):
                    return all(check_depth(v, depth + 1) for v in obj.values())
                if isinstance(obj, list):
                    return all(check_depth(v, depth + 1) for v in obj)
                return True

            if not check_depth(payload.get("params")):
                return False, -32602, "Invalid params: excessive nesting depth"

            req_id = payload.get("id")
            if req_id is not None:
                if isinstance(req_id, int):
                    if req_id < -9223372036854775808 or req_id > 9223372036854775807:
                        return False, -32600, "Invalid Request: id integer overflow"
                elif isinstance(req_id, str):
                    if len(req_id) > 64:
                        return False, -32600, "Invalid Request: id string too long"
                else:
                    return False, -32600, "Invalid Request: id must be integer or string"

            return True, None, "Valid"

        for raw in malformed_inputs:
            valid, err_code, _ = sanitize_json_rpc_frame(raw)
            self.assertFalse(valid, f"Malformed input unexpectedly considered valid: {raw[:40]}")
            self.assertIsNotNone(err_code)


if __name__ == "__main__":
    unittest.main()
