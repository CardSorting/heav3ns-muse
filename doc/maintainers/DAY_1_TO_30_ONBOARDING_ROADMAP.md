# Maintainer Onboarding Roadmap

<!-- docs-scope: current-guide -->

Six-stage learning path for new engineers and autonomous agents onboarding as maintainers of HEAV3NS-MUSE. Progression is measured solely by demonstrated physical outcomes and verified test runs.

---

## 1. Onboarding Progression Matrix

| Stage | Duration | Core Exercise | Evidence of Readiness |
| :--- | :--- | :--- | :--- |
| **1. Orient** | Day 1–3 | Read [Product Scope](../../README.md), [Headless Architecture](../agent-native-architecture.md), and [ADRs](../adr/README.md). Build the headless binary. | `cmake -B build -DHEADLESS=ON && cmake --build build` compiles with zero warnings. |
| **2. Trace** | Day 4–7 | Trace a JSON-RPC studio command (`track.create`) from wire socket through `studio_process.cpp` into `Song::addTrack()`. | Submit an annotated trace diagram showing thread handoffs and lock acquisitions. |
| **3. Change** | Day 8–14 | Implement a bounded studio capability (e.g., pan curve or automation point edit) with Python SDK test coverage. | `python3 tests/studio_process_test.py` passes with new test case included. |
| **4. Recover** | Day 15–21 | Walk an incident runbook: simulate a synth plugin crash, capture an LLDB core dump headless, and apply isolation. | Reproduce crash and verify headless host remains alive via fallback isolation. |
| **5. Decide** | Day 22–26 | Author an Architectural Decision Record (ADR) under `doc/adr/` following the established numbering and scope rules. | `npm run docs:check` validates ADR contiguousness and index synchronization. |
| **6. Maintain**| Day 27–30 | Execute a full documentation audit, source impact analysis, cryptographic review, and roadmap gate transition. | Submit PR with passing `npm run docs:check` and signed cryptographic review. |

---

## 2. Environment Verification (Preflight)

Before attempting stage exercises, verify your local environment:

```sh
# 1. Check compiler, CMake, and Qt5/Qt6 toolchain
cmake --version
clang++ --version || g++ --version
python3 --version

# 2. Check documentation and knowledgebase tooling
npm run docs:check
npm run docs:status

# 3. Check roadmap integrity
npm run roadmap:check
```

---

## 3. Detailed Stage Guide

### Stage 1: Orient
- Study the single source of truth rules in [AGENTS.md](../../AGENTS.md).
- Understand why LMMS C++ models are authoritative: no shadow state in Python or JSON.
- Verify headless execution: HEAV3NS-MUSE runs under `QCoreApplication` without requiring an X11, Wayland, or Quartz display server.

### Stage 2: Trace
- Examine `src/core/studio_process.cpp`: understand the event loop, socket polling, and how commands interact with `Engine::getSong()`.
- Trace the lifecycle of an audio block through `Mixer::renderNextBuffer()` and `AudioEngine`.

### Stage 3: Change
- Add a new unit test in `tests/studio_process_test.py` validating a specific parameter mutation.
- Use `npm run ci:impact` or `npm run docs:impact` to check which CI gates and documentation domains are impacted by your change.
- Run fast local verification: `./scripts/verify.sh quick` or `npm run verify:quick`.

### Stage 4: Recover
- Read [DSP Crash Recovery](../runbooks/dsp-crash-recovery.md) and [Debugging Runbooks](DEBUGGING_AND_DIAGNOSTIC_RUNBOOKS.md).
- Read [CI/CD Failure & Recovery Runbook](../runbooks/ci-failure-and-recovery.md) to diagnose broken builds, security baseline violations, and stale knowledge.
- Practice capturing backtraces with `lldb` or `gdb` on headless binaries.

### Stage 5: Decide
- Study ADRs `0001` through `0004` under `doc/adr/`.
- Learn when an ADR is required: architectural boundary changes, threading model adjustments, or protocol schema modifications.

### Stage 6: Maintain
- Review [CI/CD Rules & Required Checks](CI_CD_RULES_AND_REQUIRED_CHECKS.md) to understand required status checks and change-aware gating.
- Run `./scripts/verify.sh full` or `npm run verify:full` before submitting any PR.
- Run `npm run docs:review -- --domain <id> --reviewer "Maintainer Name" --note "Explanation"` to refresh SHA-256 fingerprints after code changes.
- Advance roadmap gates using `npm run roadmap:roll`.
