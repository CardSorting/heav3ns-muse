#!/usr/bin/env python3
"""Risk-based change impact analyzer and test selector for HEAV3NS-MUSE CI/CD.

Evaluates git diffs to determine required CI verification gates according to the
risk tier of changed files, ensuring fast feedback for doc-only changes while
conservatively enforcing complete build and regression suites for runtime code.
"""

import argparse
import fnmatch
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

# Path pattern categorization
DOC_PATTERNS = [
    "doc/**",
    "*.md",
    "*.txt",
    ".tx/**",
    "data/locale/**"
]

HYGIENE_PATTERNS = [
    ".clang-format",
    ".clang-tidy",
    ".yamllint",
    ".editorconfig",
    ".gitmodules",
    ".mailmap",
    "package.json",
    "vcpkg.json"
]

KNOWLEDGE_PATTERNS = [
    "doc/knowledge/**",
    "doc/roadmap/**",
    "doc/evidence/**",
    "doc/adr/**",
    "doc/maintainers/**",
    "doc/playbooks/**",
    "doc/runbooks/**",
    "scripts/docs-kb.py",
    "scripts/roadmap-sync.py",
    "tests/test_docs_kb.py"
]

RUNTIME_PATTERNS = [
    "src/core/**",
    "include/**",
    "src/common/**",
    "src/tracks/**",
    "src/gui/**",
    "src/3rdparty/**",
    "plugins/**"
]

BUILD_PATTERNS = [
    "CMakeLists.txt",
    "cmake/**",
    "buildtools/**",
    "src/CMakeLists.txt",
    "tests/CMakeLists.txt",
    ".github/workflows/**",
    "Brewfile",
    "scripts/**"
]

TEST_PATTERNS = [
    "tests/**"
]

SECURITY_PATTERNS = [
    "scripts/verify*",
    "tests/test_security*",
    ".github/workflows/**"
]


def match_any(path_str, patterns):
    for pat in patterns:
        if fnmatch.fnmatch(path_str, pat):
            return True
        if pat.endswith("/**") and (path_str == pat[:-3] or path_str.startswith(pat[:-2])):
            return True
    return False


def get_changed_files(base_ref=None, head_ref=None):
    if base_ref:
        cmd = ["git", "diff", "--name-only", base_ref]
        if head_ref:
            cmd.append(head_ref)
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True)
        files = [f.strip() for f in proc.stdout.splitlines() if f.strip()]
        return sorted(files)
    # Default to uncommitted or HEAD changes
    proc = subprocess.run(["git", "diff", "--name-only", "HEAD"], cwd=ROOT, capture_output=True, text=True, check=True)
    uncommitted = [f.strip() for f in proc.stdout.splitlines() if f.strip()]
    proc_untracked = subprocess.run(
        ["git", "ls-files", "--others", "--exclude-standard"],
        cwd=ROOT, capture_output=True, text=True, check=True
    )
    untracked = [f.strip() for f in proc_untracked.stdout.splitlines() if f.strip()]
    return sorted(set(uncommitted + untracked))


def analyze_impact(files):
    if not files:
        # No changes detected, default to clean pass
        return {
            "categories": [],
            "files_count": 0,
            "hygiene_required": True,
            "knowledge_required": True,
            "security_required": True,
            "build_required": False,
            "regression_required": False,
            "rationale": "No changed files detected. Running baseline hygiene and knowledge checks."
        }

    categories = set()
    unclassified = []

    for f in files:
        matched = False
        if match_any(f, RUNTIME_PATTERNS):
            categories.add("runtime")
            matched = True
        if match_any(f, BUILD_PATTERNS):
            categories.add("build")
            matched = True
        if match_any(f, TEST_PATTERNS):
            categories.add("tests")
            matched = True
        if match_any(f, KNOWLEDGE_PATTERNS):
            categories.add("knowledge")
            matched = True
        if match_any(f, SECURITY_PATTERNS):
            categories.add("security")
            matched = True
        if match_any(f, HYGIENE_PATTERNS):
            categories.add("hygiene")
            matched = True
        if match_any(f, DOC_PATTERNS):
            categories.add("docs")
            matched = True

        if not matched:
            categories.add("unclassified")
            unclassified.append(f)

    # Core rule: Hygiene, Knowledge integrity, and Security baseline run on all evaluated changes
    hygiene_req = True
    knowledge_req = True
    security_req = True

    # Heavy build & regression: Required if runtime, build system, plugins, or unclassified files changed
    build_req = False
    regression_req = False
    reasons = []

    if "runtime" in categories:
        build_req = True
        regression_req = True
        reasons.append("Native studio runtime or C++ DSP models modified")
    if "build" in categories:
        build_req = True
        regression_req = True
        reasons.append("CMake configuration or CI build infrastructure modified")
    if "tests" in categories:
        build_req = True
        regression_req = True
        reasons.append("Test suites or regression harnesses modified")
    if "unclassified" in categories:
        # Conservative fail-safe: unknown changes require full build & test
        build_req = True
        regression_req = True
        reasons.append(f"Unclassified files detected ({', '.join(unclassified[:3])}); defaulting to full verification")

    if not build_req:
        reasons.append("Documentation and metadata changes only; native compile skipped per risk-tier policy")

    return {
        "categories": sorted(categories),
        "files_count": len(files),
        "changed_files": files[:15],
        "hygiene_required": hygiene_req,
        "knowledge_required": knowledge_req,
        "security_required": security_req,
        "build_required": build_req,
        "regression_required": regression_req,
        "rationale": "; ".join(reasons)
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", help="Git base ref or commit (e.g., origin/main)")
    parser.add_argument("--head", help="Git head ref (optional)")
    parser.add_argument("--format", choices=["json", "github", "text"], default="text", help="Output format")
    args = parser.parse_args()

    files = get_changed_files(base_ref=args.base, head_ref=args.head)
    impact = analyze_impact(files)

    if args.format == "json":
        print(json.dumps(impact, indent=2))
        return 0

    if args.format == "github":
        # Emit GitHub Actions step outputs
        print(f"hygiene_required={'true' if impact['hygiene_required'] else 'false'}")
        print(f"knowledge_required={'true' if impact['knowledge_required'] else 'false'}")
        print(f"security_required={'true' if impact['security_required'] else 'false'}")
        print(f"build_required={'true' if impact['build_required'] else 'false'}")
        print(f"regression_required={'true' if impact['regression_required'] else 'false'}")
        print(f"rationale={impact['rationale']}")
        return 0

    # Human-readable text
    print("========================================================================")
    print("                 HEAV3NS-MUSE CHANGE IMPACT ANALYZER                    ")
    print("========================================================================")
    print(f"  Changed Files:         {impact['files_count']}")
    print(f"  Detected Categories:   {', '.join(impact['categories']) or 'None'}")
    print("------------------------------------------------------------------------")
    print(f"  Gate 0: ci/hygiene:              {'REQUIRED' if impact['hygiene_required'] else 'SKIPPED'}")
    print(f"  Gate 1: ci/knowledge-integrity:  {'REQUIRED' if impact['knowledge_required'] else 'SKIPPED'}")
    print(f"  Gate 2: ci/linux-build:          {'REQUIRED' if impact['build_required'] else 'SKIPPED (Doc/Meta only)'}")
    print(f"  Gate 3: ci/regression:           {'REQUIRED' if impact['regression_required'] else 'SKIPPED (Doc/Meta only)'}")
    print(f"  Gate 4: ci/security-baseline:    {'REQUIRED' if impact['security_required'] else 'SKIPPED'}")
    print("------------------------------------------------------------------------")
    print(f"  Selection Rationale:   {impact['rationale']}")
    print("========================================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
