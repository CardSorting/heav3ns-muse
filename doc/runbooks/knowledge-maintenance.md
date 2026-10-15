# Runbook: Knowledgebase Maintenance & Reconciliation

<!-- docs-scope: current-guide -->

Operational procedure for reconciling stale documentation, resolving C++ source drift, updating cryptographic SHA-256 fingerprints, and ensuring knowledgebase freshness.

---

## 1. Purpose & Triggers

Execute this runbook when:
- `npm run docs:status` flags any domain as `stale`, `due`, or `unreviewed`.
- A C++ pull request modifies files in `src/core/`, `plugins/`, or `include/`.
- CI automated checks fail on documentation scope or link integrity.

---

## 2. Step-by-Step Procedure

### Step 1: Diagnose Drift
Run the governance audit commands:
```sh
npm run docs:status
npm run docs:impact
npm run docs:check
```

### Step 2: Reconcile Document Content
1. If C++ capabilities changed, update capability matrices in `README.md` and `doc/native-studio-protocol.md`.
2. Ensure every file under `doc/` contains a valid `<!-- docs-scope: ... -->` marker.
3. If an architectural decision changed, author a successor ADR in `doc/adr/` referencing the superseded decision.

### Step 3: Record Cryptographic Attestation
For each affected domain, record an explicit review attestation:
```sh
npm run docs:review -- \
  --domain <domain-key> \
  --reviewer "Maintainer Name" \
  --note "Reconciled with latest C++ engine updates"
```

### Step 4: Synchronize & Verify
Regenerate catalogs and re-run all validation checks:
```sh
npm run docs:sync
npm run docs:check
```
Both commands must exit with code 0.
