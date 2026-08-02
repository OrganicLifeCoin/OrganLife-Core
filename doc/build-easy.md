Easy Build Guide
================

Quick overview of build methods for OrganicLife.

Build Methods
-------------

| Script | Type | Best For | Output |
|--------|------|----------|--------|
| `build-depends.sh` | Static/Release | Production, distribution | `src/` |
| `build.sh` | Dynamic/Dev | Development, fast iteration | `build/` |

Use `build-depends.sh` for production wallets.
Use `build.sh` for development work.

Quick Start
-----------

Production build:
```bash
./build-depends.sh
./src/qt/organiclife-qt
```

Development build:
```bash
./build.sh
./build/organiclife-qt
```

Prerequisites
-------------

See OS-specific guides:
- `doc/build-macos.md`
- `doc/build-linux.md`
- `doc/build-windows.md`

Common requirements:
- Git, C++ compiler, CMake
- Autotools (autoconf, automake, libtool)
- Rust (cargo)

Build Options
-------------

Both scripts support:

```bash
--no-gui       # Daemon only (faster)
--debug        # Debug build
--clean        # Clean rebuild
--jobs N       # Parallel jobs (default: auto)
--help         # Show all options
```

CMake Presets
-------------

With CMake 3.14+:

```bash
cmake --preset=vcpkg              # Configure release
cmake --build --preset=vcpkg -j8  # Build

# Available presets:
# vcpkg        - Release with GUI
# vcpkg-debug  - Debug build
# vcpkg-no-gui - Daemon only
```

Build Outputs
-------------

`build-depends.sh` output in `src/`:
```
src/organiclifed        - Daemon
src/organiclife-cli     - CLI client
src/organiclife-tx      - Transaction utility
src/qt/organiclife-qt   - GUI wallet
```

`build.sh` output in `build/`:
```
build/organiclifed      - Daemon
build/organiclife-qt    - GUI wallet (convenience copy)
```

Running
-------

```bash
# GUI wallet
./src/qt/organiclife-qt

# Daemon
./src/organiclifed -daemon
./src/organiclife-cli getblockchaininfo
./src/organiclife-cli stop

# Testnet
./src/organiclifed -testnet -daemon
```

Data Directories
----------------

- macOS: `~/Library/Application Support/OrganicLife/`
- Linux: `~/.organiclife/`
- Windows: `%APPDATA%\OrganicLife\`

Backup `wallet.dat` - contains private keys.

Troubleshooting
---------------

**"cargo: command not found"**
```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

**Out of memory**
```bash
./build-depends.sh --jobs 2
```

**macOS: "Berkeley DB not found" (build.sh only)**
```bash
brew install berkeley-db@4
```

**Clean rebuild**
```bash
./build-depends.sh --clean
```

Build System Details
--------------------

`build-depends.sh` uses the `depends/` directory - a traditional Bitcoin/PIVX build system that compiles all dependencies from source with static linking. First build takes 30-60 minutes but creates portable, self-contained binaries.

`build.sh` uses Microsoft's vcpkg package manager for faster development builds with dynamic linking.
