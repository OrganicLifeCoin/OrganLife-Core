macOS Build Guide
=================

Requirements
------------

- macOS 12+ (Intel or Apple Silicon)
- Xcode Command Line Tools
- Homebrew
- 8GB+ RAM recommended
- 50GB+ free disk space

Install Prerequisites
---------------------

```bash
# Install Xcode Command Line Tools
xcode-select --install

# Install Homebrew (if not installed)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install build dependencies
brew install autoconf automake libtool pkg-config python3 cmake ninja

# Install Rust
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

Build (Release)
---------------

Static build for production use:

```bash
./build-depends.sh
```

Binaries in `src/`:
- `src/organiclifed`
- `src/organiclife-cli`
- `src/organiclife-tx`
- `src/qt/organiclife-qt`

Build (Development)
-------------------

Faster build for development:

```bash
# Note: Requires Berkeley DB from Homebrew
brew install berkeley-db@4

./build.sh
```

Binaries in `build/`:
- `build/organiclifed`
- `build/organiclife-qt`

Build Options
-------------

```bash
./build-depends.sh --no-gui       # Daemon only
./build-depends.sh --debug        # Debug build
./build-depends.sh --clean        # Clean rebuild
./build-depends.sh --jobs 4       # Limit parallel jobs
```

CMake Presets
-------------

With CMake 3.14+:

```bash
cmake --preset=vcpkg              # Configure
cmake --build --preset=vcpkg      # Build
```

Running
-------

```bash
# GUI wallet
./src/qt/organiclife-qt

# Daemon
./src/organiclifed -daemon
./src/organiclife-cli getblockchaininfo
```

Data Directory
--------------

`~/Library/Application Support/OrganicLife/`

Troubleshooting
---------------

**"cargo: command not found"**
```bash
source ~/.cargo/env
```

**Add to shell profile to persist:**
```bash
echo 'source ~/.cargo/env' >> ~/.zshrc
```

**Out of memory during build**
```bash
./build-depends.sh --jobs 2
```

**Clean everything and rebuild**
```bash
./build-depends.sh --clean
```

Apple Silicon Notes
-------------------

Both Intel and Apple Silicon are auto-detected. The build scripts will use the correct triplet (`arm64-osx` or `x64-osx`) automatically.

Code Signing (Optional)
-----------------------

For distribution without "unidentified developer" warnings:

```bash
codesign --deep --force --verify --verbose --sign "Developer ID" src/qt/organiclife-qt
```
