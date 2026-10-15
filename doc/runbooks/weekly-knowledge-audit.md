# Runbook: Weekly Knowledge Audit

<!-- docs-scope: current-guide -->

Scheduled weekly maintenance procedure for maintainers and autonomous monitoring agents to audit repository health, documentation integrity, and roadmap evidence.

---

## 1. Audit Schedule & Responsibilities

- **Cadence**: Every Monday at 09:00 UTC.
- **Primary Role**: Lead Engineering Maintainer or Designated Review Agent.
- **Success Criteria**: Zero stale reviews, zero unmapped source files, 100% roadmap gate verification integrity.

---

## 2. Weekly Checklist

### 1. Source Drift & Review Freshness
```sh
# Verify that all 6 knowledge domains are currently reviewed
npm run docs:status -- --fail-on-due --fail-on-unreviewed
```
If any domain is due or stale, execute [knowledge-maintenance.md](knowledge-maintenance.md).

### 2. Comprehensive Documentation Verification
```sh
npm run docs:check
```
Ensures:
- Exactly 1 valid `docs-scope` marker per document.
- Zero broken internal markdown links or invalid anchors.
- Strict contiguous numbering of ADRs.
- 100% inclusion in `doc/CATALOG.md`.

### 3. Roadmap Evidentiary Audit
Inspect [doc/roadmap/register.json](../roadmap/register.json) against physical test results:
```sh
npm run roadmap:check
npm run test:studio
```
Verify that every completed gate in Horizon 0, 1, and 2 has a physical timestamp, verifying command, and valid test receipt.
