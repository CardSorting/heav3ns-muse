# Performance Benchmarking & Service Level Objectives (SLOs)

<!-- docs-scope: domain-reference -->

This reference describes how to collect native performance evidence. It does
not establish certified latency, memory, throughput or real-time service levels.
Use the [engineering record](../agent-native-architecture.md) for dated results,
platforms and remaining failures.

## 1. Engine Service Level Objectives (SLOs)

No universal callback-latency, render-speed, request-latency or resident-memory
SLO is certified by the current native test results. Report the actual build,
backend, sample rate, period size, project, plugins and measurement method with
each result. A finite render or passing worker test is not proof of physical
output, zero xruns or hard real-time behavior.

The studio uses JSON lines over stdin/stdout. Socket round-trip measurements
and unimplemented benchmark command-line flags do not describe this interface.

## 2. Benchmarking Methodology

Use existing native fixtures to establish correctness before measuring speed:

```sh
ctest --test-dir build/native --output-on-failure -R '^(AudioEngineWorkerTest|MixerProcessingTest|StudioRuntimeTest)$'
python3 tests/studio_process_test.py --lmms build/native/lmms --output build/studio-evidence
```

Record elapsed time and repetitions alongside their assertions. Worker tests
exercise dispatch/scan retirement and ownership; process tests exercise native
protocol and audio workflows. Their wall times include test setup and filesystem
work, so they are not hardware callback latency measurements.

For a responsive studio, `studio.resources.inspect` observes process resources,
`audio.processing.inspect` observes DSP failure/retry and worker state, and
`transport.inspect` includes backend health. Follow the
[protocol reference](../native-studio-protocol.md) for response contracts.
Separate those observations from any external callback-timing or memory
instrumentation, and retain measurement failures rather than inventing values.

## 3. Resource Budgets & Invariants

Configured job budgets and measured usage are separate from process-wide limits.
A worker completion barrier preserves queue lifetime; it does not bound a
processor that never returns. Repeated render equality depends on the project,
plugin state, random sources and backend configuration. Use exact PCM hashes
only for inputs whose determinism was established; otherwise retain appropriate
finite-sample, level, pitch, channel and recovery checks.

Document allocation, CPU, RSS, latency and hardware coverage only when observed
for the stated configuration. Cross-platform, sanitizer and broader hardware
acceptance remain separate from a successful local Debug run.
