# Maintainer CLI Cheatsheet

<!-- docs-scope: current-guide -->

Private CLI navigation cheatsheet for HEAV3NS-MUSE maintainers. Covers CMake builds, headless studio server operations, Python SDK verification, knowledgebase governance, and auto-rolling roadmap commands.

---

## 1. Build & Compilation (C++ / CMake)

| Task | Command | Notes |
| :--- | :--- | :--- |
| **Configure Headless (Debug)** | `cmake -B build -DCMAKE_BUILD_TYPE=Debug -DHEADLESS=ON` | Enables debug symbols, asserts, and ASan |
| **Configure Headless (Release)** | `cmake -B build -DCMAKE_BUILD_TYPE=Release -DHEADLESS=ON` | Full optimizations, zero GUI dependencies |
| **Build Core & Plugins** | `cmake --build build -j$(sysctl -n hw.ncpu || nproc)` | Multi-threaded compilation |
| **Run Native CTest Suite** | `ctest --test-dir build --output-on-failure` | Executes C++ unit tests |
| **Clean Build Tree** | `rm -rf build` | Resets build artifacts |

---

## 2. Headless Studio Server & Python SDK

| Task | Command | Notes |
| :--- | :--- | :--- |
| **Launch Headless Studio** | `./build/lmms --studio --socket /tmp/muse.sock` | Starts in-process studio on Unix socket |
| **Launch on Loopback TCP** | `./build/lmms --studio --port 7777` | Starts TCP server on `127.0.0.1:7777` |
| **Run Studio Integration Test** | `python3 tests/studio_process_test.py --lmms build/lmms` | Spawns headless host, runs 12+ capability tests |
| **Python SDK Quick Inspection** | `python3 -c "import muse; client = muse.StudioClient(); print(client.capabilities())"` | Zero-dependency verification |
| **Test Audio Perception Export** | `python3 tests/audio_perception_test.py --socket /tmp/muse.sock` | Tests Mel-spectrogram & stem extraction |

---

## 3. Documentation & Knowledgebase Governance

| Task | Command | Notes |
| :--- | :--- | :--- |
| **Validate Full Docs Corpus** | `npm run docs:check` | Validates scopes, links, anchors, ADRs, reviews |
| **Synchronize Catalogs** | `npm run docs:sync` | Regenerates `CATALOG.md`, `DOMAINS.md`, `DECISIONS.md` |
| **Inspect Review Freshness** | `npm run docs:status` | Shows stale, due, and reviewed status per domain |
| **Analyze Source Impact** | `npm run docs:impact` | Shows affected domains for current working-tree |
| **Record Domain Review** | `npm run docs:review -- --domain <id> --reviewer "<name>" --note "<note>"` | Updates SHA-256 fingerprints in `reviews.json` |

---

## 4. Auto-Rolling Roadmap Commands

| Task | Command | Notes |
| :--- | :--- | :--- |
| **Check Roadmap Integrity** | `npm run roadmap:check` | Validates registered gates, horizon state, generated views and referenced completion receipts |
| **Synchronize Roadmap Views** | `npm run roadmap:sync` | Recalculates horizon state and updates `ROADMAP.md` and `OPEN_ITEMS.md`; never executes tests |
| **Advance a Roadmap Gate** | Follow the [completion procedure](../how-to/change-with-documentation.md#close-a-roadmap-gate) | `roll --status completed` requires a registered `--receipt` and criterion review note |
| **Inspect Gate Status** | Read [open actions](../roadmap/OPEN_ITEMS.md) and run `python3 scripts/roadmap-sync.py check` | Check generated state and referenced receipts; verification commands are not executed |

---

## 5. Formatting & Code Quality

| Task | Command | Notes |
| :--- | :--- | :--- |
| **Format C++ Code** | `find src include -name '*.cpp' -o -name '*.h' | xargs clang-format -i` | Adheres to `.clang-format` |
| **Static Analysis (Clang-Tidy)** | `clang-tidy -p build src/core/*.cpp` | Catches memory leaks, thread concurrency bugs |
| **Lint Python Scripts** | `python3 -m pycodestyle scripts/*.py tests/*.py` | Validates Python PEP-8 compliance |
