# Runbook: Knowledgebase Maintenance & Reconciliation

<!-- docs-scope: current-guide -->

Operational procedure for reconciling stale documentation, resolving C++ source drift, updating cryptographic SHA-256 fingerprints, and ensuring knowledgebase freshness.

---

## Purpose and trigger

Execute this runbook when:
- `npm run docs:status` flags any domain as `stale`, `due`, or `unreviewed`.
- A C++ pull request modifies files in `src/core/`, `plugins/`, or `include/`.
- CI automated checks fail on documentation scope or link integrity.
- Owner: Documentation Maintainer and affected Subsystem Maintainer.

## Authority and prerequisites

- Local development environment with Python 3.9+, Node.js 18+, and git.
- Read [documentation policy](../DOCUMENTATION_POLICY.md) and preserve clean working trees.
- Documentation authority permits guide and registry updates; it does not authorize bypassing physical test evidence.

## Procedure

1. **Diagnose Drift**: Run `npm run docs:status`, `npm run docs:impact`, and `npm run docs:check` to separate generated drift from source drift.
2. **Reconcile Content**: Update corresponding capability tables in `README.md` and `doc/native-studio-protocol.md`. For architectural changes, author a successor ADR.
3. **Remove Obsolete Recipes**: Purge deprecated recipes from active guides; archive historical measurements under dated evidence records.
4. **Record Cryptographic Attestation**: For each affected domain, run `npm run docs:review -- --domain <id> --reviewer "<name>" --note "<explanation>"`.
5. **Synchronize & Regenerate**: Run `npm run docs:sync` to rebuild `catalog.json`, `DOMAINS.md`, and `DECISIONS.md`.

## Stop conditions

- Stop and escalate if C++ code, ADR rationale, and observable audio behavior disagree.
- Never blindly run `docs:review` to refresh hashes without inspecting source diffs.
- Never delete historical evidence artifacts to force a passing check.

## Recovery and rollback

- If an incorrect guide edit is committed, revert the specific commit using `git revert <sha>`.
- If a review attestation was erroneously recorded, re-run `docs:review` with corrected notes and valid evidence hashes.

## Verification and evidence

- `npm run docs:check` must pass with 0 errors.
- `npm run docs:status -- --fail-on-due --fail-on-unreviewed` must report all domains as `reviewed`.
- Retain inspection logs in the pull request description.
