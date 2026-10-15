# HEAV3NS-MUSE Maintainer Guide

Private operational and architectural entry point for maintainers of the HEAV3NS-MUSE agent-native digital audio workstation. Public communication and documentation boundaries follow [AGENTS.md](AGENTS.md). For current project status and session transition, consult [HANDOVER.md](HANDOVER.md) and the [Auto-Rolling Roadmap](ROADMAP.md).

---

## 1. Operational Navigation & Authority Boundaries

The C++ audio engine (`AudioEngine`, `Mixer`, `Song`, `TrackContainer`), the headless studio host (`studio_process`), and the external client SDK / autonomous agents operate across strictly defined architectural boundaries:

- **Model Authority**: In-process C++ LMMS models are the sole authoritative representation of musical projects. External clients, subagents, and Python SDK tools interact strictly via the synchronous Native Studio Protocol over Unix domain sockets or loopback TCP.
- **Headless Determinism**: The studio server runs under `QCoreApplication` without display server dependencies (no X11, Wayland, or Cocoa windows). Headless rendering must produce bit-identical audio outputs.
- **Maintainer Catalog**: Consult the comprehensive [Maintainer Documentation Catalog](doc/maintainers/README.md) for detailed guidelines on:
  - [AI Agent Governance](doc/maintainers/AI_AGENT_PAIR_PROGRAMMING_AND_GOVERNANCE.md)
  - [Onboarding Roadmap](doc/maintainers/DAY_1_TO_30_ONBOARDING_ROADMAP.md)
  - [Maintainer CLI Cheatsheet](doc/maintainers/MAINTAINER_CLI_CHEATSHEET.md)
  - [Subsystem Internals & Dependency DAG](doc/maintainers/SUBSYSTEM_INTERNALS_AND_DEPENDENCY_GRAPH.md)
  - [Performance Benchmarking & SLOs](doc/maintainers/PERFORMANCE_BENCHMARKING_AND_SLOS.md)
  - [Release Engineering](doc/maintainers/RELEASE_ENGINEERING_AND_VERSIONING.md)

---

## 2. Verification & Release Protocol

Always inspect scripts and run validation harnesses prior to committing or tagging releases:

1. **Unified Local Verification Interface** (Recommended):
   ```sh
   ./scripts/verify.sh quick       # Gate 0 (Hygiene) + Gate 1 (Knowledge) + Gate 4 (Security)
   ./scripts/verify.sh knowledge   # Gate 1 Knowledge Integrity & catalog consistency
   ./scripts/verify.sh security    # Gate 4 Security baseline, tokens, traversal & JSON-RPC fuzzing
   ./scripts/verify.sh full        # Full multi-gate verification suite (Gates 0, 1, 3, 4)
   ```
2. **Local Documentation & Governance Validation**:
   ```sh
   npm run docs:check
   ```
   Validates documentation scopes (`<!-- docs-scope: ... -->`), local relative links, ADR numbering, and knowledgebase reviews.
3. **Headless Studio Regression Suite**:
   ```sh
   python3 tests/studio_process_test.py --lmms build/lmms
   ```
4. **CI/CD Governance & Change Impact**:
   - Consult [CI/CD Rules & Required Checks](doc/maintainers/CI_CD_RULES_AND_REQUIRED_CHECKS.md) for required status checks (`ci / hygiene`, `ci / knowledge-integrity`, `ci / security-baseline`, `ci / linux-build`, `ci / regression`, `ci / summary`).
   - Run `npm run ci:impact` to compute exact gate execution requirements for any branch or diff.
   - Refer to [CI/CD Failure & Recovery Runbook](doc/runbooks/ci-failure-and-recovery.md) when diagnosing remote CI check failures.
5. **Roadmap Integrity**:
   ```sh
   npm run roadmap:check
   ```
6. **Release Procedures & SLSA Provenance**:
   - Refer to [Release Engineering & Versioning](doc/maintainers/RELEASE_ENGINEERING_AND_VERSIONING.md) for multi-platform packaging (macOS DMG, Linux AppImage/Tarball, Windows Portable ZIP).
   - Refer to [Release Provenance & Archival](doc/maintainers/RELEASE_PROVENANCE_AND_ARCHIVAL.md) for generating SLSA v1 in-toto provenance statements and managing custodial release authority.

---

## 3. Documentation Ownership & Cryptographic Reviews

All documentation follows the [Documentation Policy](doc/DOCUMENTATION_POLICY.md):
- Keep active guides reconciled with source code in whole documents.
- When C++ models or protocol behaviors change, analyze impact with `npm run docs:impact` and update corresponding guides.
- Record maintainer reviews using:
  ```sh
  npm run docs:review -- --domain <domain-id> --reviewer "<name>" --note "<context>"
  ```
- The [Complete Docs Catalog](doc/CATALOG.md) classifies the entire documentation corpus.
- The [Active Open Items](doc/roadmap/OPEN_ITEMS.md) and [Rolling Progress Log](doc/roadmap/ROLLING_LOG.md) track unresolved tasks and verified milestones.
