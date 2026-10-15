# Architectural Decision Records (ADR) Catalog

<!-- docs-scope: current-guide -->

This directory records architectural decisions for the HEAV3NS-MUSE agent-native digital audio workstation runtime. Decisions follow an immutable sequence where newer decisions supersede older ones rather than editing historical records.

---

## Active & Superseded Decisions

| ID | Title | Status | Date | Owning Domain |
| :---: | :--- | :---: | :---: | :--- |
| [0001](0001-native-dsp-model-authority.md) | Native LMMS C++ Models as Authoritative State | `accepted` | 2026-10-12 | `domain-capabilities` |
| [0002](0002-in-process-qcoreapplication-headless-studio.md) | In-Process QCoreApplication for Headless Studio Execution | `accepted` | 2026-10-12 | `domain-studio-runtime` |
| [0003](0003-atomic-manifest-and-single-writer-persistence.md) | Atomic Manifests, File Locks, and Request Idempotency | `accepted` | 2026-10-13 | `domain-session-durability` |
| [0004](0004-two-stage-audio-understanding.md) | Two-Stage Audio Understanding (DSP Analysis + Perceptual Critique) | `accepted` | 2026-10-14 | `domain-audio-dsp` |

---

## Decision Lifecycle Rules

1. **Immutability**: Once an ADR is accepted and committed, its core rationale and context cannot be rewritten.
2. **Supersession**: If an architectural choice changes, create a new numbered ADR that links `supersedes` to the prior record, and update the prior record's metadata to `status: superseded` pointing to the successor.
3. **Evidence**: Delivery states (`local`, `verified`) must cite concrete source files or passing test runs.
