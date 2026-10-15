# Playbook 01: Provisioning & Preflight

<!-- docs-scope: current-guide -->

Step-by-step developer and CI machine provisioning for building and testing HEAV3NS-MUSE across macOS and Linux.

---

## 1. System Requirements

- **C++ Compiler**: Clang 14+ (macOS) or GCC 11+ (Linux) with full C++17 support.
- **Build System**: CMake 3.20+ and Ninja (recommended) or Make.
- **GUI Framework**: Qt 5.15+ or Qt 6.x (only `QtCore`, `QtXml`, `QtNetwork` are required for headless builds).
- **Python**: Python 3.9+ for the studio test runner, client SDK, and governance CLI.
- **Node.js**: Node 18+ and npm for documentation tooling and script wrappers.

---

## 2. macOS Provisioning (Apple Silicon / Intel)

Install required dependencies via [Homebrew](https://brew.sh/):

```sh
# 1. Install core dependencies
brew update
brew install cmake ninja qt@5 pkg-config libsndfile libsamplerate fftw

# 2. Add Qt to PATH
export PATH="/opt/homebrew/opt/qt@5/bin:$PATH"
export CMAKE_PREFIX_PATH="/opt/homebrew/opt/qt@5"

# 3. Clone and configure
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEADLESS=ON
cmake --build build
```

---

## 3. Ubuntu / Debian Linux Provisioning

```sh
# 1. Update package lists
sudo apt-get update

# 2. Install build essentials and libraries
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  qtbase5-dev libqt5xmlpatterns5-dev \
  libsndfile1-dev libsamplerate0-dev libfftw3-dev \
  libasound2-dev libpulse-dev libjack-jackd2-dev \
  python3 python3-pip nodejs npm

# 3. Configure and compile
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHEADLESS=ON
cmake --build build
```

---

## 4. Preflight Verification Suite

Execute the preflight suite to certify the build environment:

```sh
# Run documentation governance checks
npm run docs:check

# Run studio protocol automated regression
python3 tests/studio_process_test.py --lmms build/lmms

# Verify roadmap consistency
npm run roadmap:check
```
