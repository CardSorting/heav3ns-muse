# Knowledge domains

<!-- docs-scope: current-guide -->

Generated from [registry.json](registry.json). Run `python3 scripts/docs-kb.py status` for current review state.

## Studio Runtime & Process Lifecycle

Owner role: **Studio Runtime Maintainers**. Domain: `studio-runtime`.

- [Documentation authority and maintenance](../../doc/DOCUMENTATION_POLICY.md)
- [Native studio development interface](../../doc/native-studio-protocol.md)
- [ADR 0002: In-Process QCoreApplication for Headless Studio Execution](../../doc/adr/0002-in-process-qcoreapplication-headless-studio.md)
- [Maintainer Onboarding Roadmap](../../doc/maintainers/DAY_1_TO_30_ONBOARDING_ROADMAP.md)
- [Maintainer CLI Cheatsheet](../../doc/maintainers/MAINTAINER_CLI_CHEATSHEET.md)
- [Subsystem Internals & Dependency DAG](../../doc/maintainers/SUBSYSTEM_INTERNALS_AND_DEPENDENCY_GRAPH.md)
- [Release Engineering & Versioning](../../doc/maintainers/RELEASE_ENGINEERING_AND_VERSIONING.md)
- [Debugging & Diagnostic Runbooks](../../doc/maintainers/DEBUGGING_AND_DIAGNOSTIC_RUNBOOKS.md)
- [Git Workflows, Hooks & PR Attestation](../../doc/maintainers/GIT_WORKFLOWS_AND_HOOKS.md)
- [Playbook 01: Provisioning & Preflight](../../doc/playbooks/01-provisioning-and-preflight.md)
- [Runbook: Knowledgebase Maintenance & Reconciliation](../../doc/runbooks/knowledge-maintenance.md)
- [Runbook: Weekly Knowledge Audit](../../doc/runbooks/weekly-knowledge-audit.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Native DAW Capabilities & Operation Dispatch

Owner role: **Capabilities Maintainers**. Domain: `capabilities`.

- [Native studio development interface](../../doc/native-studio-protocol.md)
- [ADR 0001: Native LMMS C++ Models as Authoritative State](../../doc/adr/0001-native-dsp-model-authority.md)
- [Maintainer Glossary & Lexicon](../../doc/maintainers/MAINTAINER_GLOSSARY_AND_LEXICON.md)
- [Playbook 02: Zero-Hesitation Developer Ergonomics](../../doc/playbooks/02-zero-hesitation-developer-ergonomics.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Agent Execution & Job Lifecycle

Owner role: **Agent Systems Maintainers**. Domain: `agent-coordinator`.

- [HEAV3NS LMMS engineering record](../../doc/agent-native-architecture.md)
- [AI Agent Pair Programming & Governance](../../doc/maintainers/AI_AGENT_PAIR_PROGRAMMING_AND_GOVERNANCE.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Conversational & Multimodal Inference Adapters

Owner role: **AI Inference Maintainers**. Domain: `inference-adapters`.

- [HEAV3NS LMMS engineering record](../../doc/agent-native-architecture.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Audio Synthesis, Rendering & Technical Analysis

Owner role: **Audio DSP Maintainers**. Domain: `audio-dsp`.

- [ADR 0004: Two-Stage Audio Understanding (DSP Analysis + Perceptual Critique)](../../doc/adr/0004-two-stage-audio-understanding.md)
- [Performance Benchmarking & Service Level Objectives (SLOs)](../../doc/maintainers/PERFORMANCE_BENCHMARKING_AND_SLOS.md)
- [Playbook 03: Incident Response & Audio Triage](../../doc/playbooks/03-incident-response-and-audio-triage.md)
- [Runbook: DSP Crash Recovery & Plugin Isolation](../../doc/runbooks/dsp-crash-recovery.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Session Durability, Manifests & Storage

Owner role: **Session Storage Maintainers**. Domain: `session-durability`.

- [ADR 0003: Atomic Manifests, File Locks, and Request Idempotency](../../doc/adr/0003-atomic-manifest-and-single-writer-persistence.md)

Relevant checks: `python3 tests/studio_process_test.py`.
