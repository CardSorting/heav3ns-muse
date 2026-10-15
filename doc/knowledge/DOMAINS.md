# Knowledge domains

<!-- docs-scope: current-guide -->

Generated from [registry.json](registry.json). Run `python3 scripts/docs-kb.py status` for current review state.

## Studio Runtime & Process Lifecycle

Owner role: **Studio Runtime Maintainers**. Domain: `studio-runtime`.

- [Documentation authority and maintenance](../../doc/DOCUMENTATION_POLICY.md)
- [Native studio development interface](../../doc/native-studio-protocol.md)
- [ADR 0002: In-Process QCoreApplication for Headless Studio Execution](../../doc/adr/0002-in-process-qcoreapplication-headless-studio.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Native DAW Capabilities & Operation Dispatch

Owner role: **Capabilities Maintainers**. Domain: `capabilities`.

- [Native studio development interface](../../doc/native-studio-protocol.md)
- [ADR 0001: Native LMMS C++ Models as Authoritative State](../../doc/adr/0001-native-dsp-model-authority.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Agent Execution & Job Lifecycle

Owner role: **Agent Systems Maintainers**. Domain: `agent-coordinator`.

- [HEAV3NS LMMS engineering record](../../doc/agent-native-architecture.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Conversational & Multimodal Inference Adapters

Owner role: **AI Inference Maintainers**. Domain: `inference-adapters`.

- [HEAV3NS LMMS engineering record](../../doc/agent-native-architecture.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Audio Synthesis, Rendering & Technical Analysis

Owner role: **Audio DSP Maintainers**. Domain: `audio-dsp`.

- [ADR 0004: Two-Stage Audio Understanding (DSP Analysis + Perceptual Critique)](../../doc/adr/0004-two-stage-audio-understanding.md)

Relevant checks: `python3 tests/studio_process_test.py`.

## Session Durability, Manifests & Storage

Owner role: **Session Storage Maintainers**. Domain: `session-durability`.

- [ADR 0003: Atomic Manifests, File Locks, and Request Idempotency](../../doc/adr/0003-atomic-manifest-and-single-writer-persistence.md)

Relevant checks: `python3 tests/studio_process_test.py`.
