# Evidence Catalog & Execution Artifact Registry

<!-- docs-scope: operational-record -->

> **Scope**: Operational record. Preserves dated empirical verification receipts, physical test runs, benchmarking logs, and gate completion artifacts. Historical measurements are permanent; they are never overwritten or deleted when milestones advance.

---

## 1. Artifact Retention & Invariant Contract

The evidence pipeline retains observations for review. A hash establishes artifact
integrity; a maintainer must assess whether an observation covers a milestone:

1. **Permanence of Historical Evidence**: When a roadmap gate passes, its output log, execution receipt, and SHA-256 fingerprint are permanently retained under `doc/evidence/test-runs/`. Newer test runs append new receipts rather than overwriting historical artifacts.
2. **Cryptographic Binding**: New completion receipts are recorded in `doc/evidence/registry.json` and hashed using SHA-256. `roadmap-sync.py check` verifies referenced completion receipts and their separate output hashes. Earlier registry entries without hashes are historical claims, not cryptographically validated output.
3. **Traceability from Horizon to Command**: New closures in `ROADMAP.md` link to their completion receipt. Pre-2026-10-16 closures retain their original source links and dated events without retroactively claiming receipt validation.

---

## 2. Historical Verification Artifacts

The following table preserves the original observations and gate labels. Its
older labels and aggregate claims do not override the current gate criteria in
[the register](../roadmap/register.json) or establish coverage of those criteria.

| Date | Gate ID | Target Platform | Verifying Command | Receipt Artifact | Status |
| :--- | :---: | :--- | :--- | :--- | :---: |
| **2026-10-12** | `GATE-001` | macOS / Linux | `python3 tests/studio_process_test.py` | [test-runs/20261012-studio-core.json](test-runs/20261012-studio-core.json) | `verified` |
| **2026-10-12** | `GATE-002` | Headless Host | `python3 tests/studio_process_test.py` | [test-runs/20261012-stream-isolation.json](test-runs/20261012-stream-isolation.json) | `verified` |
| **2026-10-13** | `GATE-003` | File Engine | `python3 tests/studio_process_test.py` | [test-runs/20261013-single-writer-lock.json](test-runs/20261013-single-writer-lock.json) | `verified` |
| **2026-10-13** | `GATE-007` | C++ Mutators | `python3 tests/studio_process_test.py` | [test-runs/20261013-capabilities-matrix.json](test-runs/20261013-capabilities-matrix.json) | `verified` |
| **2026-10-14** | `GATE-013` | Multimodal Audio | `python3 tests/studio_process_test.py` | [test-runs/20261014-full-song-render.json](test-runs/20261014-full-song-render.json) | `verified` |

---

## 3. Recording New Evidence

Follow the [gate completion procedure](../how-to/change-with-documentation.md#close-a-roadmap-gate)
to execute verification, review its scope, and create a dated JSON receipt with
a hash-bound output artifact. Registering an artifact records its hash and does
not execute the command or prove that its contents cover the gate criterion.
Replace the example date and note with the actual observation:

```sh
python3 scripts/docs-kb.py record-evidence \
  --gate GATE-014 \
  --command "python3 tests/studio_process_test.py" \
  --artifact doc/evidence/test-runs/YYYYMMDD-gate014.json \
  --platform "Actual observed platform" \
  --note "Describe observed coverage and limitations"
```

Then use `roadmap-sync.py roll --status completed --receipt` with that registered
receipt and a criterion review note. A successful register update cannot replace
the real-provider audio, revised project, hardware, or reference-audio evidence
required by the relevant gate.
