#!/usr/bin/env python3
"""Maintainer preflight and onboarding readiness validator for HEAV3NS-MUSE."""

import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def check_tool(binary, min_version=None):
    path = shutil.which(binary)
    if not path:
        return False, "Not found"
    try:
        proc = subprocess.run([binary, "--version"], capture_output=True, text=True, timeout=5)
        first_line = proc.stdout.splitlines()[0] if proc.stdout else "Available"
        return True, first_line.strip()
    except Exception as e:
        return True, f"Found at {path}"


def main():
    print("========================================================================")
    print("       HEAV3NS-MUSE MAINTAINER ONBOARDING & PREFLIGHT DIAGNOSTIC        ")
    print("========================================================================")
    print()

    # 1. Toolchain Verification
    print("  1. Compiler & Build Toolchain:")
    tools = [
        ("cmake", "CMake build system"),
        ("clang++", "C++ compiler (Clang)"),
        ("g++", "C++ compiler (GCC)"),
        ("ninja", "Ninja build accelerator"),
        ("python3", "Python runtime & test runner"),
        ("node", "Node.js documentation engine"),
        ("npm", "Node package manager"),
    ]
    tool_status = {}
    for cmd, desc in tools:
        found, info = check_tool(cmd)
        tool_status[cmd] = found
        marker = "[OK]" if found else "[MISSING]"
        if cmd == "g++" and tool_status.get("clang++"):
            # Secondary compiler
            continue
        print(f"    {marker:<10} {desc:<30} {info[:35]}")

    # 2. Repository & Governance Status
    print()
    print("  2. Repository Governance & Documentation Health:")
    docs_proc = subprocess.run(["python3", "scripts/verify-docs.py"], cwd=ROOT, capture_output=True, text=True)
    kb_proc = subprocess.run(["python3", "scripts/docs-kb.py", "check"], cwd=ROOT, capture_output=True, text=True)
    roadmap_proc = subprocess.run(["python3", "scripts/roadmap-sync.py", "check"], cwd=ROOT, capture_output=True, text=True)

    print(f"    {'[OK]' if docs_proc.returncode == 0 else '[FAIL]':<10} Documentation Corpus & Link Integrity")
    print(f"    {'[OK]' if kb_proc.returncode == 0 else '[FAIL]':<10} Cryptographic Knowledgebase Reviews")
    print(f"    {'[OK]' if roadmap_proc.returncode == 0 else '[FAIL]':<10} Auto-Rolling Roadmap Register")

    # 3. Maintainer Learning Path Progress
    print()
    print("  3. Maintainer Onboarding Stages (doc/maintainers/DAY_1_TO_30_ONBOARDING_ROADMAP.md):")
    stages = [
        ("Stage 1: Orient", tool_status.get("cmake") and tool_status.get("python3") and docs_proc.returncode == 0),
        ("Stage 2: Trace", (ROOT / "build/lmms").is_file()),
        ("Stage 3: Change", (ROOT / "tests/studio_process_test.py").is_file()),
        ("Stage 4: Recover", (ROOT / "doc/runbooks/dsp-crash-recovery.md").is_file()),
        ("Stage 5: Decide", (ROOT / "doc/adr/0001-native-dsp-model-authority.md").is_file()),
        ("Stage 6: Maintain", kb_proc.returncode == 0 and roadmap_proc.returncode == 0),
    ]

    for name, ready in stages:
        marker = "[READY]" if ready else "[PENDING]"
        print(f"    {marker:<10} {name}")

    print()
    print("========================================================================")
    print("  Next Recommended Action:")
    if not (ROOT / "build/lmms").is_file():
        print("    Run: cmake -B build -DHEADLESS=ON && cmake --build build")
    else:
        print("    Run: npm run test:studio  # Execute in-process headless studio tests")
    print("========================================================================")

    all_ready = (docs_proc.returncode == 0 and kb_proc.returncode == 0 and roadmap_proc.returncode == 0)
    return 0 if all_ready else 1


if __name__ == "__main__":
    sys.exit(main())
