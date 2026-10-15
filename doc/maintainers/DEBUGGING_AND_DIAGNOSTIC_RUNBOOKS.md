# Debugging & Diagnostic Runbooks

<!-- docs-scope: current-guide -->

Operational diagnostic workflows for investigating crashes, memory corruption, audio buffer dropouts (xruns), and protocol desynchronization in HEAV3NS-MUSE.

---

## 1. Headless Crash Backtrace Capture (LLDB / GDB)

When a headless studio process crashes under automated testing or agent execution:

### macOS (LLDB)
```sh
# Run headless studio inside LLDB
lldb -- ./build/lmms --studio --socket /tmp/muse-debug.sock

# Inside LLDB prompt:
(lldb) run
# ... trigger crash ...
(lldb) thread backtrace all
(lldb) frame variable
```

### Linux (GDB)
```sh
# Enable core dumps
ulimit -c unlimited

# Run with GDB batch mode
gdb -batch -ex "run" -ex "bt full" -ex "quit" --args ./build/lmms --studio --socket /tmp/muse-debug.sock
```

---

## 2. AddressSanitizer (ASan) & ThreadSanitizer (TSan)

Detecting use-after-free, memory buffer overflows, and thread race conditions in DSP plugins:

```sh
# Configure with ASan and UndefinedBehaviorSanitizer
cmake -B build-asan \
  -DCMAKE_BUILD_TYPE=Debug \
  -DHEADLESS=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"

cmake --build build-asan -j$(nproc || sysctl -n hw.ncpu)

# Run integration suite under ASan
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 python3 tests/studio_process_test.py --lmms build-asan/lmms
```

---

## 3. Audio Buffer Underrun (Xrun) Diagnosis

Audio dropouts occur when the real-time audio thread misses its hardware buffer deadline:

1. **Verify Buffer Size**: Check configured buffer frames (default `256` frames = 5.8ms at 44.1kHz). If underruns occur under heavy synth loads, increase to `512` frames.
2. **Audit Audio Thread Locks**:
   - Grep `src/core/` and `plugins/` for any `mutex.lock()` or `malloc()` calls occurring within `renderAudio` or `playNotes`.
   - Ensure all parameters are passed via lock-free ringbuffers.
3. **Inspect CPU Governor**:
   - On Linux, verify CPU scaling governor: `cpupower frequency-info` (ensure `performance` mode).

---

## 4. IPC Socket Protocol & Wire Tracing

To inspect malformed JSON-RPC frames or unexpected client disconnects:

```sh
# Using socat to proxy and log socket communication
socat -v UNIX-LISTEN:/tmp/muse-proxy.sock,fork UNIX-CONNECT:/tmp/muse.sock
```

Inspect wire messages:
- Confirm newline-delimited JSON payloads.
- Verify mandatory JSON-RPC 2.0 fields (`jsonrpc: "2.0"`, `id`, `method`, `params`).
- Check that error objects include standardized `code` and `message` properties.
