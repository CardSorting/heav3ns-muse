# Runbook: CI/CD Failure Triage & Recovery

<!-- docs-scope: current-guide -->

Standard operational procedure for diagnosing, reproducing, and recovering from automated verification gate failures in HEAV3NS-MUSE continuous integration pipelines.

---

## Purpose and trigger

- **Trigger**: Any failure reported on GitHub Actions for `ci / hygiene`, `ci / knowledge-integrity`, `ci / linux-build`, `ci / regression`, `ci / security-baseline`, or `ci / summary`.
- **Purpose**: Rapidly identify root cause, reproduce the defect locally using deterministic commands, apply corrective actions, and restore clean green status without bypassing verification gates.

---

## Authority and prerequisites

- **Authority**: Studio Runtime Maintainers, Capabilities Maintainers, or assigned autonomous agent.
- **Prerequisites**:
  - Local repository checkout with clean git status.
  - Python 3.11+, Node.js 20+, and CMake 3.25+.
  - Access to GitHub Actions job execution logs and step summaries.

---

## Procedure

### 1. Identify the Failing Gate
Inspect the PR check summary or run local verification to isolate the failure:
```sh
./scripts/verify.sh full
```

### 2. Triage by Gate ID

#### If `ci / hygiene` Fails:
1. Check YAML syntax:
   ```sh
   yamllint -c .yamllint .github/
   ```
2. Verify submodules are committed and pinned:
   ```sh
   git submodule status
   git submodule update --init --recursive
   ```
3. Run change-impact unit tests:
   ```sh
   python3 tests/test_change_impact.py
   ```

#### If `ci / knowledge-integrity` Fails:
1. Check for stale cryptographic domain reviews or unmapped core sources:
   ```sh
   npm run docs:status
   npm run docs:check
   ```
2. If source files in `src/core/` or `include/` were modified without an attestation, record a domain review:
   ```sh
   npm run docs:review -- --domain <domain-id> --reviewer "<Name>" --note "<Context>"
   ```
3. Reconcile documentation catalogs and compiled agent anchor:
   ```sh
   npm run docs:sync
   ```

#### If `ci / linux-build` Fails:
1. Review compiler error logs in the `ci / linux-build` step.
2. Reproduce locally:
   ```sh
   cmake -B build -DHEADLESS=ON
   cmake --build build --target lmms
   ```
3. Common causes: missing header includes in `include/`, `-DUSE_WERROR=ON` compiler warnings, or C++20 standard incompatibilities.

#### If `ci / regression` Fails:
1. Run local CTest suite:
   ```sh
   ctest --test-dir build --output-on-failure
   ```
2. Run headless studio process integration test:
   ```sh
   python3 tests/studio_process_test.py --lmms build/lmms --output build/evidence
   ```
3. Check for state leaks, unclosed file locks (`.lock`), or unexpected JSON-RPC output formatting.

#### If `ci / security-baseline` Fails:
1. Run local security boundary test suite:
   ```sh
   python3 tests/test_security_boundary.py
   ```
2. Check for accidental secret commits, unencrypted git URLs in `.gitmodules`, or missing `permissions: contents: read` declarations in `.github/workflows/*.yml`.

---

## Stop conditions

Halt investigation and escalate immediately if:
- A secret, private key, or credential was pushed to any public branch (revoke the secret immediately; do not simply make a new commit).
- A corruption of git history or submodule commit references is detected.
- An unauthorized release tag or workflow mutation is observed.

---

## Recovery and rollback

1. Apply minimal targeted fix to resolve the specific failure.
2. Validate locally with `./scripts/verify.sh <gate-name>`.
3. If code changes altered governed sources, re-run domain review:
   ```sh
   npm run docs:review -- --domain <id> --reviewer "<Name>" --note "Remediation of CI failure"
   npm run docs:sync
   ```
4. If a regression cannot be immediately resolved, revert the offending commit:
   ```sh
   git revert <commit-sha>
   npm run docs:sync
   git push origin <branch>
   ```

---

## Verification and evidence

1. Execute full local verification pass:
   ```sh
   ./scripts/verify.sh full
   ```
2. Verify all status checks exit with code 0.
3. Record a CI evidence receipt in the physical registry:
   ```sh
   python3 scripts/record-ci-evidence.py \
     --gate CI-RECOVERY \
     --status PASS \
     --component studio-runtime \
     --test-suite "./scripts/verify.sh full" \
     --note "Remediated CI failure and verified clean recovery"
   ```
4. Confirm `npm run docs:check` and `npm run docs:audit` report 100% clean status.
