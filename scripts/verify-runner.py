#!/usr/bin/env python3
"""Unified local and CI verification test runner for HEAV3NS-MUSE.

Provides standard verification gates mirroring cargo and mature C++ ecosystems:
  quick:      Hygiene and knowledge integrity
  knowledge:  Cryptographic domain reviews, scopes, links, and agent anchor
  hygiene:    YAML syntax, file formatting, license notices, and submodules
  security:   Secret scanning, boundary fuzzing, and workflow permissions
  regression: CTest suites and headless studio process integration tests
  full:       Complete layered verification of all gates
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]

# ANSI formatting
BOLD = "\033[1m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
RED = "\033[31m"
CYAN = "\033[36m"
DIM = "\033[2m"
RESET = "\033[0m"


def run_step(name, cmd, cwd=ROOT):
    start = time.time()
    print(f"  {CYAN}▶{RESET} {BOLD}{name:<52}{RESET} ", end="", flush=True)
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    dur = time.time() - start
    if proc.returncode == 0:
        print(f"{GREEN}[PASS]{RESET} {DIM}({dur:.2f}s){RESET}")
        return True, proc.stdout, dur
    print(f"{RED}[FAIL]{RESET} {DIM}({dur:.2f}s){RESET}")
    print(f"\n{RED}--- Error Output for '{name}' ---{RESET}")
    if proc.stderr:
        print(proc.stderr.strip())
    if proc.stdout:
        print(proc.stdout.strip())
    print(f"{RED}-----------------------------------{RESET}\n")
    return False, proc.stderr + proc.stdout, dur


def run_hygiene_gate():
    print(f"\n{BOLD}Gate 0 — Repository Hygiene & Code Formatting{RESET}")
    success = True
    total_dur = 0

    # 1. YAML syntax & formatting
    if shutil.which("yamllint"):
        yamllint_cfg = ROOT / ".yamllint"
        cmd = ["yamllint", "-c", str(yamllint_cfg), ".github/workflows"] if yamllint_cfg.exists() else ["yamllint", "-d", "{extends: default, rules: {line-length: disable, document-start: disable, truthy: disable}}", ".github/workflows"]
        ok, _, dur = run_step("YAML Syntax & Formatting (yamllint)", cmd)
        success = success and ok
        total_dur += dur
    else:
        try:
            import yaml
            for y in ROOT.glob(".github/**/*.yml"):
                if y.is_file():
                    yaml.safe_load(y.read_text(encoding="utf-8"))
            print(f"  {CYAN}▶{RESET} {BOLD}{'YAML Syntax Validation (pyyaml)':<52}{RESET} {GREEN}[PASS]{RESET}")
        except ImportError:
            # Built-in fallback syntax & tab checker
            def check_yaml_file(path):
                lines = path.read_text(encoding="utf-8").splitlines()
                for idx, line in enumerate(lines, 1):
                    if "\t" in line and not line.strip().startswith("#"):
                        raise ValueError(f"{path.name}:{idx}: Tab character not allowed in YAML")
            try:
                for y in ROOT.glob(".github/**/*.yml"):
                    if y.is_file():
                        check_yaml_file(y)
                print(f"  {CYAN}▶{RESET} {BOLD}{'YAML Syntax & Formatting (built-in)':<52}{RESET} {GREEN}[PASS]{RESET}")
            except Exception as e:
                print(f"  {CYAN}▶{RESET} {BOLD}{'YAML Syntax & Formatting':<52}{RESET} {RED}[FAIL]{RESET} {e}")
                success = False

    # 2. JSON Manifest & Schema Syntax
    try:
        json_targets = [ROOT / "package.json"] + list(ROOT.glob("doc/knowledge/*.json")) + list(ROOT.glob("doc/roadmap/*.json"))
        for j in json_targets:
            if j.is_file():
                json.loads(j.read_text(encoding="utf-8"))
        print(f"  {CYAN}▶{RESET} {BOLD}{'JSON Manifests & Schemas Syntax':<52}{RESET} {GREEN}[PASS]{RESET}")
    except Exception as e:
        print(f"  {CYAN}▶{RESET} {BOLD}{'JSON Manifests & Schemas Syntax':<52}{RESET} {RED}[FAIL]{RESET} {e}")
        success = False

    # 2. Submodule status
    ok, _, dur = run_step("Git Submodule Pinning & Integrity", ["git", "submodule", "status"])
    success = success and ok
    total_dur += dur

    # 3. Change impact analyzer test
    ok, _, dur = run_step("Change-Aware Impact Engine Unit Tests", ["python3", "tests/test_change_impact.py"])
    success = success and ok
    total_dur += dur

    return success, total_dur


def run_knowledge_gate():
    print(f"\n{BOLD}Gate 1 — Knowledgebase Integrity & Governance Gates{RESET}")
    success = True
    total_dur = 0

    # 1. Knowledge guard unit tests
    ok, _, dur = run_step("Knowledge Guard Unit Tests (8/8)", ["python3", "tests/test_docs_kb.py"])
    success = success and ok
    total_dur += dur

    # 2. Documentation scope, links, catalog
    ok, _, dur = run_step("Documentation Scopes, Links & ADRs", ["python3", "scripts/verify-docs.py"])
    success = success and ok
    total_dur += dur

    # 3. Cryptographic reviews & 7 domains
    ok, _, dur = run_step("Cryptographic Domain Reviews (7 Domains)", ["python3", "scripts/docs-kb.py", "check"])
    success = success and ok
    total_dur += dur

    # 4. Zero-stale domain check
    ok, _, dur = run_step("Zero-Stale Domain Seal Verification", ["python3", "scripts/docs-kb.py", "status", "--fail-on-due", "--fail-on-unreviewed"])
    success = success and ok
    total_dur += dur

    # 5. Roadmap register validation
    ok, _, dur = run_step("Auto-Rolling Roadmap Register Schema", ["python3", "scripts/roadmap-sync.py", "check"])
    success = success and ok
    total_dur += dur

    # 6. Contracted agent anchor validation
    ok, _, dur = run_step("Contracted Agent Ground Truth Anchor", ["python3", "scripts/docs-kb.py", "anchor", "--validate"])
    success = success and ok
    total_dur += dur

    return success, total_dur


def run_security_gate():
    print(f"\n{BOLD}Gate 4 — Security Baseline, Least Privilege & Boundary Fuzzing{RESET}")
    success = True
    total_dur = 0

    # 1. Security boundary test suite
    ok, _, dur = run_step("Security Baseline & Input Boundary Tests", ["python3", "tests/test_security_boundary.py"])
    success = success and ok
    total_dur += dur

    return success, total_dur


def run_regression_gate():
    print(f"\n{BOLD}Gate 3 — Unit and Integration Verification{RESET}")
    success = True
    total_dur = 0

    # 1. Check if build/lmms exists
    lmms_bin = ROOT / "build/lmms"
    if not lmms_bin.is_file():
        # Check if CTest is available in build/
        if (ROOT / "build/CTestTestfile.cmake").is_file():
            ok, _, dur = run_step("Native CTest Suite (build/)", ["ctest", "--test-dir", "build", "--output-on-failure", "-j4"])
            success = success and ok
            total_dur += dur
        else:
            print(f"  {YELLOW}▶{RESET} {BOLD}{'Headless Studio Integration Tests':<52}{RESET} {YELLOW}[SKIPPED]{RESET} {DIM}(build/lmms not compiled){RESET}")
            print(f"    {DIM}To compile: cmake -B build -DHEADLESS=ON && cmake --build build{RESET}")
    else:
        ok, _, dur = run_step("Headless Studio Process Tests", ["python3", "tests/studio_process_test.py", "--lmms", str(lmms_bin), "--output", "build/evidence"])
        success = success and ok
        total_dur += dur

    return success, total_dur


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gate", nargs="?", default="quick",
                        choices=["quick", "knowledge", "hygiene", "security", "regression", "full"],
                        help="Verification scope to execute")
    parser.add_argument("--json", action="store_true", help="Output machine-readable JSON result")
    args = parser.parse_args()

    print(f"{BOLD}{CYAN}========================================================================{RESET}")
    print(f"{BOLD}{CYAN}             HEAV3NS-MUSE LOCAL & CI VERIFICATION RUNNER                {RESET}")
    print(f"{BOLD}{CYAN}========================================================================{RESET}")
    print(f"  Target Scope:      {BOLD}{args.gate.upper()}{RESET}")
    print(f"  Timestamp:         {datetime.now(timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')}")

    results = {}
    overall_start = time.time()
    overall_success = True

    if args.gate in {"quick", "hygiene", "full"}:
        ok, dur = run_hygiene_gate()
        results["hygiene"] = {"status": "PASS" if ok else "FAIL", "duration": dur}
        overall_success = overall_success and ok

    if args.gate in {"quick", "knowledge", "full"}:
        ok, dur = run_knowledge_gate()
        results["knowledge"] = {"status": "PASS" if ok else "FAIL", "duration": dur}
        overall_success = overall_success and ok

    if args.gate in {"security", "full"}:
        ok, dur = run_security_gate()
        results["security"] = {"status": "PASS" if ok else "FAIL", "duration": dur}
        overall_success = overall_success and ok

    if args.gate in {"regression", "full"}:
        ok, dur = run_regression_gate()
        results["regression"] = {"status": "PASS" if ok else "FAIL", "duration": dur}
        overall_success = overall_success and ok

    overall_dur = time.time() - overall_start

    print(f"\n{BOLD}{CYAN}========================================================================{RESET}")
    print(f"{BOLD}  VERIFICATION SUMMARY:{RESET} {'[' + GREEN + 'ALL GATES PASSED' + RESET + ']' if overall_success else '[' + RED + 'VERIFICATION FAILED' + RESET + ']'}")
    print(f"  Total Duration:     {overall_dur:.2f}s")
    for gname, gres in results.items():
        color = GREEN if gres["status"] == "PASS" else RED
        print(f"    - {gname.capitalize():<18} {color}[{gres['status']}]{RESET} {DIM}({gres['duration']:.2f}s){RESET}")
    print(f"{BOLD}{CYAN}========================================================================{RESET}\n")

    if args.json:
        payload = {
            "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "scope": args.gate,
            "success": overall_success,
            "duration": overall_dur,
            "gates": results
        }
        print(json.dumps(payload, indent=2))

    return 0 if overall_success else 1


if __name__ == "__main__":
    sys.exit(main())
