#!/usr/bin/env python3
"""Knowledge Gate <-> CI Machine-Readable Evidence Contract Recorder.

Records structured, cryptographically hashed verification receipts tying CI/CD test
executions and build outcomes directly to knowledgebase governance domains and
roadmap gates according to Commission 011 standards.
"""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = "doc" if (ROOT / "doc").is_dir() else "docs"
EVIDENCE_REGISTRY = f"{DOCS_DIR}/evidence/registry.json"
CI_EVIDENCE_DIR = ROOT / f"{DOCS_DIR}/evidence/ci-runs"

VALID_STATUSES = {
    "PASS", "FAIL", "INCOMPLETE", "STALE", "NOT_APPLICABLE", "AWAITING_AUTHORITY"
}


def get_git_info():
    try:
        commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        branch = subprocess.run(["git", "branch", "--show-current"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        return commit or "unknown", branch or "main"
    except Exception:
        return "unknown", "main"


def record_ci_receipt(gate, status, component, test_suite, artifact=None,
                      failure_class="none", authority="automated", note=""):
    if status not in VALID_STATUSES:
        raise ValueError(f"Invalid status: {status}. Must be one of {sorted(VALID_STATUSES)}")

    commit, branch = get_git_info()
    iso_now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    CI_EVIDENCE_DIR.mkdir(parents=True, exist_ok=True)

    receipt_filename = f"{datetime.now(timezone.utc).strftime('%Y%m%d_%H%M%S')}-{gate.lower()}.json"
    receipt_path = CI_EVIDENCE_DIR / receipt_filename
    rel_receipt_path = receipt_path.relative_to(ROOT).as_posix()

    payload = {
        "version": 1,
        "contract": "HEAV3NS-MUSE-CI-011",
        "gate_id": gate,
        "source_commit": commit,
        "branch": branch,
        "component": component,
        "verification_type": "continuous_integration",
        "test_suite": test_suite,
        "execution_environment": {
            "os": platform.system(),
            "os_release": platform.release(),
            "arch": platform.machine(),
            "python_version": platform.python_version()
        },
        "timestamps": {
            "recorded_at": iso_now
        },
        "result_status": status,
        "failure_classification": failure_class,
        "authorization_boundary": authority,
        "note": note,
        "linked_artifact": artifact or rel_receipt_path
    }

    # Write receipt
    receipt_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    sha = hashlib.sha256(receipt_path.read_bytes()).hexdigest()

    # Update registry.json
    ev_reg_path = ROOT / EVIDENCE_REGISTRY
    ev_data = json.loads(ev_reg_path.read_text(encoding="utf-8")) if ev_reg_path.is_file() else {"version": 1, "receipts": {}}

    ev_data.setdefault("receipts", {})[rel_receipt_path] = {
        "gate": gate,
        "recorded_at": iso_now,
        "command": test_suite,
        "platform": f"{platform.system()} {platform.machine()}",
        "sha256": sha,
        "status": status.lower(),
        "note": note
    }
    ev_reg_path.write_text(json.dumps(ev_data, indent=2) + "\n", encoding="utf-8")

    print(f"Recorded CI evidence contract receipt for {gate}:")
    print(f"  Artifact:  {rel_receipt_path}")
    print(f"  SHA-256:   {sha[:16]}...")
    print(f"  Status:    {status} (authority: {authority})")
    return rel_receipt_path, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gate", required=True, help="Gate identifier (e.g. CI-HYGIENE, GATE-014)")
    parser.add_argument("--status", required=True, choices=list(VALID_STATUSES), help="Execution result status")
    parser.add_argument("--component", required=True, help="Governed component or domain identifier")
    parser.add_argument("--test-suite", required=True, help="Test suite command or identifier")
    parser.add_argument("--artifact", help="Optional path to associated test output/log artifact")
    parser.add_argument("--failure-class", default="none", help="Failure classification if non-zero")
    parser.add_argument("--authority", default="automated", help="Authorization boundary (automated, custodial, review)")
    parser.add_argument("--note", default="", help="Contextual evaluation notes")
    args = parser.parse_args()

    record_ci_receipt(
        gate=args.gate,
        status=args.status,
        component=args.component,
        test_suite=args.test_suite,
        artifact=args.artifact,
        failure_class=args.failure_class,
        authority=args.authority,
        note=args.note
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
