# CI/CD Integrity Gates & Required Status Checks

<!-- docs-scope: current-guide -->

Authoritative specification for the continuous integration, continuous delivery, and repository governance infrastructure of HEAV3NS-MUSE pursuant to **HEAV3NS Engineering Commission 011**.

---

## 1. Architectural Philosophy: Independent Verification

In HEAV3NS-MUSE, **the CI system is an independent verifier of the agent's work—not another agent-generated declaration of success**.

- A test that never ran is not a pass.
- A document that was never checked is not authoritative.
- A successful build is not proof of correct behavior.
- A green checkmark without retained evidence is not engineering assurance.

```mermaid
flowchart TD
    PR[Pull Request / Git Push] --> Detect[Risk-Based Change Detection]
    Detect --> G0[ci / hygiene]
    Detect --> G1[ci / knowledge-integrity]
    Detect --> G4[ci / security-baseline]
    Detect --> G2[ci / linux-build]
    Detect --> G3[ci / regression]
    G0 & G1 & G4 & G2 & G3 --> Summary[ci / summary (Authoritative Gate)]
    Summary -->|All Passed| Merge[Eligible for Merge / Release]
    Summary -->|Any Failed| Block[Merge Blocked + Actionable Local Repro]
```

---

## 2. The Five Verification Gates

The repository enforces five layered gates executed across GitHub Actions (`.github/workflows/ci.yml`) and reproduced locally via `./scripts/verify.sh`:

| Check Name | Gate ID | Scope & Invariant | Execution Rule | Local Repro Command |
| :--- | :---: | :--- | :--- | :--- |
| **`ci / hygiene`** | Gate 0 | Syntax, submodules, format, licensing, manifests | Mandatory on all PRs | `./scripts/verify.sh hygiene` |
| **`ci / knowledge-integrity`** | Gate 1 | 7 sealed domains, scopes, links, zero-stale, agent anchor | Mandatory on all PRs | `./scripts/verify.sh knowledge` |
| **`ci / linux-build`** | Gate 2 | Native headless C++ reference build (`-DUSE_WERROR=ON`) | Change-aware (skipped for docs-only) | `cmake -B build && cmake --build build` |
| **`ci / regression`** | Gate 3 | CTest suites, headless studio integration, persistence recovery | Change-aware (skipped for docs-only) | `./scripts/verify.sh regression` |
| **`ci / security-baseline`** | Gate 4 | Secret scanning, least-privilege tokens, boundary fuzzing | Mandatory on all PRs | `./scripts/verify.sh security` |
| **`ci / summary`** | Gate ★ | Authoritative summary evaluator & PR status gate | Mandatory on all PRs | `./scripts/verify.sh full` |

---

## 3. Change-Aware Risk-Based Test Selection

To optimize feedback velocity without sacrificing correctness, `scripts/change-impact.py` evaluates git diffs against the pull request base:

- **Documentation & Metadata Changes (`doc/**`, `*.md`, `LICENSE.txt`)**:
  - Triggers: `ci / hygiene`, `ci / knowledge-integrity`, `ci / security-baseline`.
  - Skips: `ci / linux-build`, `ci / regression` (reporting explicit non-execution rationale).
- **Native Studio Runtime (`src/core/**`, `include/**`, `plugins/**`)**:
  - Triggers all five gates: full compilation, CTest unit tests, and headless integration.
- **Build Infrastructure (`CMakeLists.txt`, `cmake/**`, `.github/**`)**:
  - Triggers full multi-gate verification.
- **Fail-Safe Invariant**:
  - If any modified file cannot be confidently mapped to a known safe category, the system conservatively falls back to full verification.

---

## 4. Recommended GitHub Repository Ruleset Configuration

To enforce these gates on GitHub without allowing skips to bypass checks, repository administrators must configure a branch ruleset for `main`:

```json
{
  "name": "HEAV3NS-MUSE Protection Ruleset",
  "target": "branch",
  "enforcement": "active",
  "conditions": {
    "ref_name": {
      "include": ["refs/heads/main", "refs/heads/master"],
      "exclude": []
    }
  },
  "rules": [
    {
      "type": "deletion"
    },
    {
      "type": "non_fast_forward"
    },
    {
      "type": "pull_request",
      "parameters": {
        "required_approving_review_count": 1,
        "dismiss_stale_reviews_on_push": true,
        "require_code_owner_review": false,
        "require_last_push_approval": true
      }
    },
    {
      "type": "required_status_checks",
      "parameters": {
        "strict_required_status_checks_policy": true,
        "required_status_checks": [
          {"context": "ci / summary"},
          {"context": "ci / hygiene"},
          {"context": "ci / knowledge-integrity"},
          {"context": "ci / security-baseline"}
        ]
      }
    }
  ]
}
```

> [!IMPORTANT]
> Because `ci / summary` depends on all upstream jobs and runs unconditionally (`if: always()`), requiring `ci / summary` guarantees that no skipped, failed, or missing check can silently allow an unverified merge.

---

## 5. Local Command Parity

Maintainers and contracted agents must verify changes locally before pushing:

```sh
# Fast pre-commit check (Hygiene + Knowledge integrity)
./scripts/verify.sh quick
npm run verify:quick

# Full pre-push check (All gates)
./scripts/verify.sh full
npm run verify:full

# Inspect risk-based change impact
python3 scripts/change-impact.py --base origin/main
npm run ci:impact
```
