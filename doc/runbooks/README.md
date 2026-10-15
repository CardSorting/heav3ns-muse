# Maintainer Runbooks

<!-- docs-scope: current-guide -->

Operational runbooks for recurring maintenance tasks, documentation auditing, and incident remediation in HEAV3NS-MUSE.

---

## 1. Runbook Index

| Runbook | Frequency / Trigger | Primary Owner | Scope |
| :--- | :--- | :--- | :--- |
| [Knowledge Maintenance](knowledge-maintenance.md) | Source drift detected or review due date reached | Documentation Maintainer | `current-guide` |
| [Weekly Knowledge Audit](weekly-knowledge-audit.md) | Weekly recurring maintenance sprint | Lead Engineering Maintainer | `current-guide` |
| [DSP Crash Recovery](dsp-crash-recovery.md) | Plugin crash or headless segfault | Core Audio Engine Team | `current-guide` |

---

## 2. Execution Principles

Maintainers must execute runbooks with disposable test fixtures and non-destructive inspection. Verification commands must pass cleanly before marking runbook tasks complete.
