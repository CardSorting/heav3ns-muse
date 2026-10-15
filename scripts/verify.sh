#!/usr/bin/env bash
# HEAV3NS-MUSE Unified Verification Command Interface
# Usage: ./scripts/verify.sh [quick|knowledge|hygiene|security|regression|full]

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ $# -eq 0 ]; then
    exec python3 "${ROOT}/scripts/verify-runner.py" quick
else
    exec python3 "${ROOT}/scripts/verify-runner.py" "$@"
fi
