# Current Project Handover

Operational handover entry point for incoming maintainers and autonomous AI agents. The authoritative single source of truth for current roadmap milestones is [ROADMAP.md](ROADMAP.md) backed by [doc/roadmap/register.json](doc/roadmap/register.json) and [doc/roadmap/ROLLING_LOG.md](doc/roadmap/ROLLING_LOG.md).

---

## 1. Executive Status & Active Horizon

The project is currently executing **Horizon 2 (H2: Production Hardening, Multimodal Audio & Hardware)**:

| Horizon | Description | Gate Range | Completion | Active Status |
| :--- | :--- | :---: | :---: | :--- |
| **H0** | Foundation & In-Process Headless Core | `GATE-001` - `GATE-006` | **100%** | Fully sealed and verified |
| **H1** | Native Studio Protocol & Full Song Model | `GATE-007` - `GATE-013` | **88%** | Protocol engine complete; final test gate passed |
| **H2** | Hardening, Multimodal Audio & Hardware | `GATE-014` - `GATE-018` | **30%** | **ACTIVE IN-FLIGHT HORIZON** |
| **H3** | Autonomous Studio Director & Fleet | `GATE-019` - `GATE-021` | **0%** | Deferred strategic backlog |

---

## 2. In-Flight Work & Next Safe Actions

The active priority queue for the current sprint:

1. **`GATE-014` (Audio Perception Pipeline)**:
   - Target: Live verification of two-stage audio critique loop (`audio.listen`) extracting Mel spectrograms and passing structured acoustic tokens.
   - Verification command: `python3 tests/audio_perception_test.py --socket /tmp/muse.sock`
2. **`GATE-015` (Audio/MIDI Hardware Hot-Swap)**:
   - Target: Stress test dynamic backend switching (`audio.backend.select`, `midi.backend.select`) between CoreAudio, ALSA, PulseAudio, and Jack without session restart.
3. **`GATE-016` (Stem Rendering with Automated LUFS & True-Peak)**:
   - Target: Multi-stem offline export computing integrated loudness (-14 LUFS) and true-peak limiting (-1.0 dBFS).

---

## 3. Verified Architectural Invariants

Incoming maintainers must strictly uphold the following invariants:

1. **Native C++ Model Authority**: `Engine::getSong()`, `Mixer`, and `PatternStore` hold the canonical DAW state. No external shadow ASTs or cached project hierarchies are permitted.
2. **Headless Determinism**: The DAW runs under `QCoreApplication` without requiring any graphical display server (no X11, Wayland, or Cocoa windows).
3. **Offline Governance Validation**: Run `npm run docs:check` or `./scripts/verify.sh quick` before every commit. Zero broken links, 100% scope markers, and valid cryptographic domain reviews are mandatory.
4. **CI/CD Independent Verification**: The CI system acts as an independent verifier of agent changes across Gates 0 through 5. PRs must pass required checks without silent skips; unexecuted tests must never be reported as passed.
5. **Auto-Rolling Progress**: All roadmap gate advancements must be executed via `npm run roadmap:roll -- --gate <ID> --status completed --note "<EVIDENCE>"`.
6. **Custodial Release Authority**: No binary or tag publication without explicit authorization from Founding Custodian EGG and verifiable SLSA v1 in-toto provenance statements.

---

## 4. Key References & Documentation

- [Maintainer Catalog](doc/maintainers/README.md) - Complete inventory of maintainer runbooks and playbooks.
- [CI/CD Rules & Required Checks](doc/maintainers/CI_CD_RULES_AND_REQUIRED_CHECKS.md) - Status checks, change-aware gates, and branch ruleset configuration.
- [CI/CD Failure & Recovery Runbook](doc/runbooks/ci-failure-and-recovery.md) - Diagnostic triage across build, security, and knowledge gates.
- [Release Provenance & Archival](doc/maintainers/RELEASE_PROVENANCE_AND_ARCHIVAL.md) - SLSA in-toto provenance generation and custodial handoff.
- [AI Agent Governance](doc/maintainers/AI_AGENT_PAIR_PROGRAMMING_AND_GOVERNANCE.md) - Parent/subagent protocol and handoff schemas.
- [Active Open Items](doc/roadmap/OPEN_ITEMS.md) - Open blocker register.
- [Rolling Progress Log](doc/roadmap/ROLLING_LOG.md) - Chronological audit log of completed gates and physical evidence.
