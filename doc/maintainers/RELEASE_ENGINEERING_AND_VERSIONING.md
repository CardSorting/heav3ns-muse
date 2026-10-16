# Release Engineering & Versioning

<!-- docs-scope: current-guide -->

Comprehensive release engineering protocol, versioning policies, binary packaging procedures, and deterministic rollback instructions for HEAV3NS-MUSE.

---

## 1. Versioning Policy & Semantic Guarantees

HEAV3NS-MUSE follows strict [Semantic Versioning 2.0.0](https://semver.org/):

- **MAJOR (`X.0.0`)**: Breaking alterations to the Native Studio Protocol JSON-RPC schemas, removal of supported audio driver backends, or incompatible C++ plugin ABI overhauls.
- **MINOR (`0.Y.0`)**: New studio capabilities, additional synth models, new perception pipeline feature extractors, or backwards-compatible protocol additions.
- **PATCH (`0.0.Z`)**: Bug fixes in DSP algorithms, real-time memory leak resolutions, documentation synchronization, and performance optimizations.

---

## 2. Pre-Release Verification Checklist

Before tagging or distributing any release candidate, maintainers must execute the full verification gate:

1. **Documentation & Knowledgebase Integrity**:
   ```sh
   npm run docs:check
   npm run docs:status -- --fail-on-due --fail-on-unreviewed
   ```
2. **Headless DSP & Studio Server Regression Suite**:
   ```sh
   cmake -B build -DCMAKE_BUILD_TYPE=Release -DHEADLESS=ON
   cmake --build build -j$(nproc || sysctl -n hw.ncpu)
   python3 tests/studio_process_test.py --lmms build/lmms
   ```
3. **Roadmap Verification**:
   ```sh
   npm run roadmap:check
   ```
4. **Clean Git Working Tree**:
   ```sh
   git status --porcelain
   # Must return zero unstaged or uncommitted files
   ```

---

## 3. Multi-Platform Binary Packaging

Packaging utilizes CMake and CPack to generate self-contained bundles with bundled plugins and presets:

| Platform | Target Package | Generation Command | Artifact Output |
| :--- | :--- | :--- | :--- |
| **macOS** | Signed DMG Bundle | `cpack -G DragNDrop` | `build/heav3ns-muse-macOS.dmg` |
| **Linux** | Portable AppImage | `cpack -G External` (via linuxdeploy) | `build/heav3ns-muse-x86_64.AppImage` |
| **Linux (Headless)**| Self-Contained Tarball | `cpack -G TGZ` | `build/heav3ns-muse-headless-linux.tar.gz` |
| **Windows** | Portable ZIP | `cpack -G ZIP` | `build/heav3ns-muse-win64.zip` |

---

## 4. Rollback Procedure

In the event of a critical regression (e.g., DSP audio corruption, memory leaks in the real-time loop, or socket protocol hangs):

1. **Identify Stable Commit**: Locate the previous verified release tag (`vX.Y.Z-1`).
2. **Hotfix vs Revert**:
   - If the root cause is a single isolated commit, apply `git revert <commit-sha>`.
   - If the issue is systemic across a minor release, roll back client SDK pin to the previous major/minor release.
3. **Record Incident in Rolling Log**:
   - Document the failure mode, affected users, and commit hash in [doc/roadmap/ROLLING_LOG.md](../roadmap/ROLLING_LOG.md).
   - Reopen the corresponding evidentiary gate in [doc/roadmap/register.json](../roadmap/register.json) using:
     ```sh
     npm run roadmap:roll -- --gate GATE-014 --status in-progress --note "Reopened due to release regression"
     ```
     Substitute the affected gate ID. The original completion event remains in history.
