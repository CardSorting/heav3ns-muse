#!/usr/bin/env python3
"""Install local git pre-push and pre-commit governance hooks for HEAV3NS-MUSE."""

import os
from pathlib import Path
import stat
import sys

ROOT = Path(__file__).resolve().parents[1]
HOOKS_DIR = ROOT / ".git/hooks"

PRE_PUSH_HOOK = """#!/bin/sh
set -e

echo "🔒 [HEAV3NS-MUSE Gate] Verifying documentation, knowledgebase, and roadmap before push..."
npm run docs:check

echo "✅ [HEAV3NS-MUSE Gate] All documentation and cryptographic reviews verified."
"""

PRE_COMMIT_HOOK = """#!/bin/sh
set -e

echo "🔍 [HEAV3NS-MUSE Gate] Running pre-commit documentation checks..."
python3 tests/test_docs_kb.py
python3 scripts/verify-docs.py

echo "✅ [HEAV3NS-MUSE Gate] Pre-commit checks passed."
"""


def main():
    if not HOOKS_DIR.is_dir():
        print(f"Error: .git/hooks directory not found at {HOOKS_DIR}", file=sys.stderr)
        return 1

    pre_push = HOOKS_DIR / "pre-push"
    pre_push.write_text(PRE_PUSH_HOOK, encoding="utf-8")
    pre_push.chmod(pre_push.stat().st_mode | stat.S_IEXEC)

    pre_commit = HOOKS_DIR / "pre-commit"
    pre_commit.write_text(PRE_COMMIT_HOOK, encoding="utf-8")
    pre_commit.chmod(pre_commit.stat().st_mode | stat.S_IEXEC)

    print(f"Installed git hooks in {HOOKS_DIR}:")
    print(f"  - {pre_push.name} (runs npm run docs:check before push)")
    print(f"  - {pre_commit.name} (runs tests and link verification before commit)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
