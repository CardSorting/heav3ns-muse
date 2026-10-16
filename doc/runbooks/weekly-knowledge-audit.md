# Runbook: Weekly Knowledge Audit

<!-- docs-scope: current-guide -->

Scheduled weekly maintenance procedure for maintainers and autonomous monitoring agents to audit repository health, documentation integrity, and roadmap evidence.

---

## Purpose and trigger

- Trigger: Every Monday at 09:00 UTC or upon sprint milestone rollover.
- Purpose: Prevent stale knowledge from accumulating, reconcile rapid C++ commits with active documentation, and audit roadmap gates against physical evidence.
- Primary Owner: Lead Engineering Maintainer or Designated Review Agent.

## Authority and prerequisites

- Clean git checkout of `main` branch.
- Verified local toolchain (`cmake`, `clang++`/`g++`, `python3`, `node`).
- Access to run headless studio verification test suite.

## Procedure

1. **Audit Documentation Corpus**: Run `npm run docs:check` to verify that all 33+ guides have valid scopes and intact internal links.
2. **Audit Review Freshness**: Run `npm run docs:status -- --fail-on-due --fail-on-unreviewed` to confirm zero stale or due domain reviews.
3. **Execute Scorecard Audit**: Run `npm run docs:audit` to generate the complete governance scorecard.
4. **Audit Roadmap Gates**: Run `npm run roadmap:check` and cross-reference closed gates against physical artifacts in `doc/evidence/`.
5. **Verify Preflight Health**: Run `npm run maintainer:check` to verify toolchain readiness.

## Stop conditions

- Halt if any roadmap gate is marked "completed" without an associated test command and passing exit code.
- Halt if any C++ source in `src/core/` has drifted without a documented review.

## Recovery and rollback

- If drift is detected, record follow-up in `doc/roadmap/register.json`, regenerate open actions with `npm run roadmap:sync`, and execute [knowledge-maintenance.md](knowledge-maintenance.md). `OPEN_ITEMS.md` is generated.
- If a gate lacks physical verification evidence, reopen it via `npm run roadmap:roll -- --gate GATE-014 --status in-progress --note "Describe the missing evidence"`, substituting the affected gate ID. Prior completion events remain historical records.

## Verification and evidence

- `npm run docs:audit` outputs all checks `[PASS] Clean`.
- Record the weekly audit summary in [doc/roadmap/ROLLING_LOG.md](../roadmap/ROLLING_LOG.md).
