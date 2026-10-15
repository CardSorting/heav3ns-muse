# Playbook 01: Provisioning & Preflight

<!-- docs-scope: current-guide -->

Step-by-step developer and CI machine provisioning for building and testing HEAV3NS-MUSE across macOS and Linux.

---

## 1. System Requirements

- **C++ Compiler**: A compiler and standard library with C++20 support.
- **Build System**: CMake 3.20+ and Ninja (recommended) or Make.
- **Qt**: Qt 5.15+ with Core, Gui, Widgets, Xml, Svg, Network and Test. Qt 6 is experimental (`WANT_QT6=ON`). Headless studio execution uses the same native executable and build dependencies as the GUI.
- **Python**: Python 3.9+ for the studio test runner, client SDK, and governance CLI.
- **Node.js**: Node 18+ and npm for documentation tooling and script wrappers.

---

## 2. macOS Provisioning (Apple Silicon / Intel)

Install required dependencies via [Homebrew](https://brew.sh/):

```sh
# 1. Install core dependencies
brew install cmake ninja qt@5 pkg-config libsndfile libsamplerate fftw fltk
# Existing optional instrument, backend and codec support
brew install fluidsynth libgig stk lilv suil portaudio sdl2 lame libvorbis

# 2. Add Qt to PATH
export PATH="$(brew --prefix qt@5)/bin:$PATH"
export CMAKE_PREFIX_PATH="$(brew --prefix qt@5)"

# 3. From the repository root, initialize pinned dependencies and configure
git -c submodule.doc/wiki.update=none submodule update --init --recursive
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWANT_QT6=OFF -DSKIP_SUBMODULES=doc/wiki
cmake --build build/native --parallel 6
```

---

## 3. Ubuntu / Debian Linux Provisioning

```sh
# 1. Update package lists
sudo apt-get update

# 2. Install build essentials and libraries
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  qtbase5-dev libqt5svg5-dev qttools5-dev qttools5-dev-tools \
  libsndfile1-dev libsamplerate0-dev libfftw3-dev \
  libasound2-dev libpulse-dev libjack-jackd2-dev \
  python3 python3-pip nodejs npm

# 3. Configure and compile
git -c submodule.doc/wiki.update=none submodule update --init --recursive
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWANT_QT6=OFF -DSKIP_SUBMODULES=doc/wiki
cmake --build build/native --parallel 6
```

---

## 4. Preflight Verification Suite

Use a fresh build directory when changing generators. CMake generates the configuration/export headers and `compile_commands.json` there; do not add substitute headers under `include/`. The repository's clangd configuration reads `build/native/compile_commands.json`. There is no `HEADLESS` CMake option; `studio` selects headless mode at runtime. The optional upstream wiki is a separate repository, is not required by the native build, and is excluded here from this project's governed documentation checkout.

Execute the verification suite and inspect failures before claiming runtime readiness:

```sh
# Run documentation governance checks
npm run docs:check

# Run studio protocol automated regression
python3 tests/studio_process_test.py --lmms build/native/lmms --output build/studio-evidence

# Native C++ tests
ctest --test-dir build/native --output-on-failure --parallel 4 --no-tests=error

# Focused synthesis, save/render/analyze and restart regression
ctest --test-dir build/native --output-on-failure --no-tests=error -R '^StudioRenderProcessTest$'

# Verify roadmap consistency
npm run roadmap:check
```
