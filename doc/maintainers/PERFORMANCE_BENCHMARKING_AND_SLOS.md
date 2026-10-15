# Performance Benchmarking & Service Level Objectives (SLOs)

<!-- docs-scope: domain-reference -->

Performance standards, latency thresholds, and Service Level Objectives (SLOs) governing the HEAV3NS-MUSE digital audio workstation engine and headless studio host.

---

## 1. Engine Service Level Objectives (SLOs)

| Metric | Target SLO | Maximum Threshold | Measurement Method |
| :--- | :--- | :--- | :--- |
| **Real-Time DSP Callback (64 frames)** | < 0.8 ms | 1.45 ms | High-resolution hardware callback timing |
| **Real-Time DSP Callback (256 frames)**| < 2.5 ms | 5.80 ms | High-resolution hardware callback timing |
| **JSON-RPC State Mutation Latency** | < 4.0 ms | 15.0 ms | Socket roundtrip time (`track.create`, `clip.insert`) |
| **Perception Query Latency (Stage 1)** | < 12.0 ms | 35.0 ms | Mel spectrogram generation for 4-bar clip |
| **Offline Rendering Speed** | > 60x real-time | > 25x real-time | 16-track full song export to 24-bit WAV |
| **Idle Memory Footprint (Headless)** | < 85 MB RSS | < 150 MB RSS | `ps -o rss` after engine initialization |

---

## 2. Benchmarking Methodology

### Real-Time Audio DSP Stress Test
The DSP stress harness renders a synthetic 32-track project with 16 `TripleOscillator` instances and 16 effect chains across various buffer configurations:

```sh
# Run automated DSP performance benchmark
./build/lmms --benchmark --tracks 32 --duration 30 --buffer-size 64
```

### JSON-RPC IPC Throughput Benchmark
Measures how rapidly an autonomous agent can program complex polyphonic compositions over the Unix socket:

```sh
# Benchmark 1,000 sequential note additions
python3 tests/benchmark_ipc.py --socket /tmp/muse.sock --operations 1000
```

---

## 3. Resource Budgets & Invariants

1. **Peak Memory Cap**: Headless studio sessions must not exceed 512 MB RSS during typical 16-track production sessions.
2. **Deterministic Offline Rendering**: Rerendering the exact same project file multiple times must yield bit-identical audio checksums (tested via SHA-256 of raw PCM data).
3. **Zero Audio Thread Drops**: Under nominal load, zero buffer xruns are permitted during audio playback.
