# Evidence Catalog & Execution Artifact Registry

<!-- docs-scope: operational-record -->

> **Scope**: Operational record. Preserves dated empirical verification receipts, physical test runs, benchmarking logs, and gate completion artifacts. Historical measurements are permanent; they are never overwritten or deleted when milestones advance.

---

## 1. Artifact Retention & Invariant Contract

The evidence pipeline provides immutable proof of completed milestones, ensuring high-velocity codebase changes remain anchored to verified reality:

1. **Permanence of Historical Evidence**: When a roadmap gate passes, its output log, execution receipt, and SHA-256 fingerprint are permanently retained under `doc/evidence/test-runs/`. Newer test runs append new receipts rather than overwriting historical artifacts.
2. **Cryptographic Binding**: Receipts are recorded in `doc/evidence/registry.json` and hashed using SHA-256. Any modification to a recorded test output triggers a validation failure in `npm run docs:check`.
3. **Traceability from Horizon to Command**: Every closed gate in `ROADMAP.md` links to a specific entry in `doc/roadmap/register.json` and its backing evidence receipt.

---

## 2. Retained Verification Artifacts

| Date | Gate ID | Target Platform | Verifying Command | Receipt Artifact | Status |
| :--- | :---: | :--- | :--- | :--- | :---: |
| **2026-10-12** | `GATE-001` | macOS / Linux | `python3 tests/studio_process_test.py` | [test-runs/20261012-studio-core.json](test-runs/20261012-studio-core.json) | `verified` |
| **2026-10-12** | `GATE-002` | Headless Host | `python3 tests/studio_process_test.py` | [test-runs/20261012-stream-isolation.json](test-runs/20261012-stream-isolation.json) | `verified` |
| **2026-10-13** | `GATE-003` | File Engine | `python3 tests/studio_process_test.py` | [test-runs/20261013-single-writer-lock.json](test-runs/20261013-single-writer-lock.json) | `verified` |
| **2026-10-13** | `GATE-007` | C++ Mutators | `python3 tests/studio_process_test.py` | [test-runs/20261013-capabilities-matrix.json](test-runs/20261013-capabilities-matrix.json) | `verified` |
| **2026-10-14** | `GATE-013` | Multimodal Audio | `python3 tests/studio_process_test.py` | [test-runs/20261014-full-song-render.json](test-runs/20261014-full-song-render.json) | `verified` |

---

## 3. Recording New Evidence

Contracted agents and maintainers must record physical evidence upon completing any roadmap gate:

```sh
# Record test execution artifact and update registry
python3 scripts/docs-kb.py record-evidence \
  --gate GATE-014 \
  --command "python3 tests/studio_process_test.py" \
  --artifact doc/evidence/test-runs/20261015-gate014-verification.json \
  --note "Verified live multimodal audio perception pipeline"
```
