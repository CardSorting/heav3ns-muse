# Maintainer Documentation & Engineering Catalog

<!-- docs-scope: current-guide -->

Private maintainer operational guide for the HEAV3NS-MUSE agent-native digital audio workstation. Start with root [MAINTAINERS.md](../../MAINTAINERS.md), [HANDOVER.md](../../HANDOVER.md), [Product Scope & Architecture](../../README.md), and [Auto-Rolling Roadmap](../../ROADMAP.md). Public communication adheres to [AGENTS.md](../../AGENTS.md).

---

## 1. Governance & Collaboration

| Document | Purpose | Scope |
| :--- | :--- | :--- |
| [AI Agent Governance](AI_AGENT_PAIR_PROGRAMMING_AND_GOVERNANCE.md) | Parent/subagent contracts, task partitioning, bounded handoff packets, and anti-hallucination rules | `current-guide` |
| [Maintainer Onboarding Roadmap](DAY_1_TO_30_ONBOARDING_ROADMAP.md) | Six-stage progressive learning path from Orient to Maintain with evidence gates | `current-guide` |
| [Maintainer CLI Cheatsheet](MAINTAINER_CLI_CHEATSHEET.md) | Command lookup for CMake, CTest, Python SDK, headless studio, KB sync, and roadmap rolls | `current-guide` |
| [Maintainer Glossary](MAINTAINER_GLOSSARY_AND_LEXICON.md) | Definitive technical lexicon for DSP, headless host, perception pipeline, and state leases | `domain-reference` |
| [Git Workflows & Hooks](GIT_WORKFLOWS_AND_HOOKS.md) | Commit conventions, branch lifecycle, pre-commit verification, and PR attestations | `current-guide` |
| [CI/CD Rules & Required Checks](CI_CD_RULES_AND_REQUIRED_CHECKS.md) | Tiered status checks, change-aware impact matrices, GITHUB_TOKEN hardening, and branch rulesets | `current-guide` |
| [Release Provenance & Archival](RELEASE_PROVENANCE_AND_ARCHIVAL.md) | SLSA v1 in-toto provenance statements, SHA-256 digests, multi-platform assets, and custodial approval | `current-guide` |
| [Contracted Agent Anchor Protocol](CONTRACTED_AGENT_ANCHOR_PROTOCOL.md) | Immutable ground truth anchor for autonomous agents, high-velocity data pipelines, and evidence permanence | `current-guide` |
| [Live Recording & Capture Pipeline](LIVE_RECORDING_AND_CAPTURE_PIPELINE.md) | Bounded MIDI note recording, automation parameter curves, lockless FIFO rings, and take rollback | `current-guide` |

---

## 2. Architecture & Subsystems

| Document | Purpose | Scope |
| :--- | :--- | :--- |
| [Subsystem Internals & Dependency DAG](SUBSYSTEM_INTERNALS_AND_DEPENDENCY_GRAPH.md) | Mermaid architectural topologies, audio vs main thread invariants, and memory limits | `domain-reference` |
| [Release Engineering & Versioning](RELEASE_ENGINEERING_AND_VERSIONING.md) | Multi-platform packaging, ABI compatibility, binary distribution, and deterministic rollbacks | `current-guide` |
| [Performance Benchmarking & SLOs](PERFORMANCE_BENCHMARKING_AND_SLOS.md) | Real-time audio deadlines (64-sample buffer), IPC latency, and offline rendering SLOs | `domain-reference` |
| [Debugging & Diagnostic Runbooks](DEBUGGING_AND_DIAGNOSTIC_RUNBOOKS.md) | LLDB/GDB headless core dump inspection, ASan memory corruption triage, and xrun tracing | `current-guide` |

---

## 3. Playbooks & Runbooks

Operational runbooks provide deterministic step-by-step procedures for live environments and incident remediation:

- **Playbooks** (`doc/playbooks/`):
  - [01: Provisioning & Preflight](../playbooks/01-provisioning-and-preflight.md) - Setting up macOS, Linux, and headless CI build environments.
  - [02: Zero-Hesitation Developer Ergonomics](../playbooks/02-zero-hesitation-developer-ergonomics.md) - Design standards for Python SDK, CLI, and JSON-RPC APIs.
  - [03: Incident Response & Audio Triage](../playbooks/03-incident-response-and-audio-triage.md) - Rapid response to DSP panics, audio clipping, and hung headless processes.
- **Runbooks** (`doc/runbooks/`):
  - [Knowledge Maintenance](../runbooks/knowledge-maintenance.md) - Reconciling stale documentation, source drift, and cryptographic domain reviews.
  - [Weekly Knowledge Audit](../runbooks/weekly-knowledge-audit.md) - Scheduled maintenance checklist for roadmap synchronization.
  - [DSP Crash Recovery](../runbooks/dsp-crash-recovery.md) - Isolating instrument plugin crashes and restoring headless stability.
  - [CI/CD Failure & Recovery](../runbooks/ci-failure-and-recovery.md) - Reproducing and diagnosing CI gate failures locally across all 5 verification tiers.

---

## 4. Verification & Attestation

Maintainers must verify code and documentation changes using the offline toolchain before pushing:

```sh
# Verify full documentation corpus, link validity, and ADR contiguousness
npm run docs:check

# Synchronize generated markdown views and knowledge catalogs
npm run docs:sync

# Inspect domain freshness and source drift
npm run docs:status

# Advance roadmap gates backed by physical verification
npm run roadmap:roll -- --gate GATE-014 --status completed --note "Audio perception verification passing"
```
