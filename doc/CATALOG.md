# Complete Documentation Catalog

<!-- docs-scope: current-guide -->

Complete index of all authoritative documentation, architecture records, guides, playbooks, runbooks, and specifications across HEAV3NS-MUSE. Every document has an assigned scope marker and authority domain.

---

## 1. Core Architecture & Protocol

| Title | Path | Scope | Domain | Kind |
| :--- | :--- | :---: | :--- | :---: |
| [Documentation Authority & Policy](DOCUMENTATION_POLICY.md) | `doc/DOCUMENTATION_POLICY.md` | `current-guide` | `studio-runtime` | `reference` |
| [Native Studio Development Interface](native-studio-protocol.md) | `doc/native-studio-protocol.md` | `current-guide` | `capabilities` | `reference` |
| [HEAV3NS LMMS Engineering Record](agent-native-architecture.md) | `doc/agent-native-architecture.md` | `operational-record` | `agent-coordinator` | `evidence` |
| [How-to: Change with Documentation](how-to/change-with-documentation.md) | `doc/how-to/change-with-documentation.md` | `current-guide` | `studio-runtime` | `how-to` |

---

## 2. Architectural Decision Records (ADR)

| Title | Path | Scope | Domain | Kind |
| :--- | :--- | :---: | :--- | :---: |
| [ADR Catalog Index](adr/README.md) | `doc/adr/README.md` | `current-guide` | `capabilities` | `navigation` |
| [ADR 0001: Native LMMS C++ Models as Authoritative State](adr/0001-native-dsp-model-authority.md) | `doc/adr/0001-native-dsp-model-authority.md` | `decision-record` | `capabilities` | `decision` |
| [ADR 0002: In-Process QCoreApplication for Headless Studio Execution](adr/0002-in-process-qcoreapplication-headless-studio.md) | `doc/adr/0002-in-process-qcoreapplication-headless-studio.md` | `decision-record` | `studio-runtime` | `decision` |
| [ADR 0003: Atomic Manifests, File Locks, and Request Idempotency](adr/0003-atomic-manifest-and-single-writer-persistence.md) | `doc/adr/0003-atomic-manifest-and-single-writer-persistence.md` | `decision-record` | `session-durability` | `decision` |
| [ADR 0004: Two-Stage Audio Understanding](adr/0004-two-stage-audio-understanding.md) | `doc/adr/0004-two-stage-audio-understanding.md` | `decision-record` | `audio-dsp` | `decision` |

---

## 3. Maintainer Engineering & Governance

| Title | Path | Scope | Domain | Kind |
| :--- | :--- | :---: | :--- | :---: |
| [Maintainer Engineering Catalog](maintainers/README.md) | `doc/maintainers/README.md` | `current-guide` | `studio-runtime` | `navigation` |
| [AI Agent Pair Programming & Governance](maintainers/AI_AGENT_PAIR_PROGRAMMING_AND_GOVERNANCE.md) | `doc/maintainers/AI_AGENT_PAIR_PROGRAMMING_AND_GOVERNANCE.md` | `current-guide` | `agent-coordinator` | `reference` |
| [Maintainer Onboarding Roadmap (Day 1 to 30)](maintainers/DAY_1_TO_30_ONBOARDING_ROADMAP.md) | `doc/maintainers/DAY_1_TO_30_ONBOARDING_ROADMAP.md` | `current-guide` | `studio-runtime` | `how-to` |
| [Maintainer CLI Cheatsheet](maintainers/MAINTAINER_CLI_CHEATSHEET.md) | `doc/maintainers/MAINTAINER_CLI_CHEATSHEET.md` | `current-guide` | `studio-runtime` | `reference` |
| [Maintainer Glossary & Lexicon](maintainers/MAINTAINER_GLOSSARY_AND_LEXICON.md) | `doc/maintainers/MAINTAINER_GLOSSARY_AND_LEXICON.md` | `domain-reference` | `capabilities` | `reference` |
| [Subsystem Internals & Dependency DAG](maintainers/SUBSYSTEM_INTERNALS_AND_DEPENDENCY_GRAPH.md) | `doc/maintainers/SUBSYSTEM_INTERNALS_AND_DEPENDENCY_GRAPH.md` | `domain-reference` | `studio-runtime` | `reference` |
| [Release Engineering & Versioning](maintainers/RELEASE_ENGINEERING_AND_VERSIONING.md) | `doc/maintainers/RELEASE_ENGINEERING_AND_VERSIONING.md` | `current-guide` | `studio-runtime` | `reference` |
| [Debugging & Diagnostic Runbooks](maintainers/DEBUGGING_AND_DIAGNOSTIC_RUNBOOKS.md) | `doc/maintainers/DEBUGGING_AND_DIAGNOSTIC_RUNBOOKS.md` | `current-guide` | `studio-runtime` | `runbook` |
| [Performance Benchmarking & SLOs](maintainers/PERFORMANCE_BENCHMARKING_AND_SLOS.md) | `doc/maintainers/PERFORMANCE_BENCHMARKING_AND_SLOS.md` | `domain-reference` | `audio-dsp` | `reference` |
| [Git Workflows, Hooks & PR Attestation](maintainers/GIT_WORKFLOWS_AND_HOOKS.md) | `doc/maintainers/GIT_WORKFLOWS_AND_HOOKS.md` | `current-guide` | `studio-runtime` | `reference` |

---

## 4. Operational Playbooks

| Title | Path | Scope | Domain | Kind |
| :--- | :--- | :---: | :--- | :---: |
| [Operational Playbooks Index](playbooks/README.md) | `doc/playbooks/README.md` | `current-guide` | `studio-runtime` | `navigation` |
| [Playbook 01: Provisioning & Preflight](playbooks/01-provisioning-and-preflight.md) | `doc/playbooks/01-provisioning-and-preflight.md` | `current-guide` | `studio-runtime` | `how-to` |
| [Playbook 02: Zero-Hesitation Developer Ergonomics](playbooks/02-zero-hesitation-developer-ergonomics.md) | `doc/playbooks/02-zero-hesitation-developer-ergonomics.md` | `current-guide` | `capabilities` | `reference` |
| [Playbook 03: Incident Response & Audio Triage](playbooks/03-incident-response-and-audio-triage.md) | `doc/playbooks/03-incident-response-and-audio-triage.md` | `current-guide` | `audio-dsp` | `runbook` |

---

## 5. Maintenance Runbooks

| Title | Path | Scope | Domain | Kind |
| :--- | :--- | :---: | :--- | :---: |
| [Maintainer Runbooks Index](runbooks/README.md) | `doc/runbooks/README.md` | `current-guide` | `studio-runtime` | `navigation` |
| [Knowledgebase Maintenance & Reconciliation](runbooks/knowledge-maintenance.md) | `doc/runbooks/knowledge-maintenance.md` | `current-guide` | `studio-runtime` | `runbook` |
| [Weekly Knowledge Audit](runbooks/weekly-knowledge-audit.md) | `doc/runbooks/weekly-knowledge-audit.md` | `current-guide` | `studio-runtime` | `runbook` |
| [DSP Crash Recovery & Plugin Isolation](runbooks/dsp-crash-recovery.md) | `doc/runbooks/dsp-crash-recovery.md` | `current-guide` | `audio-dsp` | `runbook` |

---

## 6. Knowledgebase & Roadmap Navigation

| Title | Path | Scope | Domain | Kind |
| :--- | :--- | :---: | :--- | :---: |
| [Knowledgebase Governance & Operations](knowledge/README.md) | `doc/knowledge/README.md` | `current-guide` | `studio-runtime` | `reference` |
| [Knowledge Domains Directory](knowledge/DOMAINS.md) | `doc/knowledge/DOMAINS.md` | `current-guide` | `studio-runtime` | `navigation` |
| [Decision Lifecycle Register](knowledge/DECISIONS.md) | `doc/knowledge/DECISIONS.md` | `current-guide` | `capabilities` | `navigation` |
| [Active Roadmap Actions & Open Gates](roadmap/OPEN_ITEMS.md) | `doc/roadmap/OPEN_ITEMS.md` | `current-guide` | `studio-runtime` | `reference` |
| [Rolling Roadmap Progress Log](roadmap/ROLLING_LOG.md) | `doc/roadmap/ROLLING_LOG.md` | `current-guide` | `studio-runtime` | `evidence` |
| [HEAV3NS-MUSE Auto-Rolling Roadmap](../ROADMAP.md) | `ROADMAP.md` | `current-guide` | `studio-runtime` | `reference` |
| [Public Information Boundary & Authority](../AGENTS.md) | `AGENTS.md` | `current-guide` | `studio-runtime` | `reference` |
| [Maintainer Operational Guide](../MAINTAINERS.md) | `MAINTAINERS.md` | `current-guide` | `studio-runtime` | `reference` |
| [Current Project Handover](../HANDOVER.md) | `HANDOVER.md` | `current-guide` | `studio-runtime` | `reference` |
| [The Agent-Native DAW Guide](../README.md) | `README.md` | `current-guide` | `capabilities` | `reference` |
