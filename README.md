CTEAM Core
==============

Node daemon and Qt wallet for the CTEAM network (PIVX/Bitcoin lineage).

Binaries
--------

- `cteamd` - Full node daemon
- `cteam-cli` - RPC command-line client  
- `cteam-tx` - Transaction utility
- `cteam-qt` - Qt GUI wallet

Mainnet Parameters
------------------

| Parameter | Value |
|--------|--------|
| Genesis timestamp | `2026-03-16 00:00:00 UTC` |
| Target block spacing | `2 minutes` |
| Supply cap | `177,600,000 CTEAM` |
| Premine | `102,632,000 CTEAM` at height `1` |
| Remaining fixed subsidy | `74,968,000 CTEAM` (`7,496,800` blocks, about `28.53` years at 2 minute spacing) |
| PoS activation | Height `10,081` (about `14 days` after genesis) |
| Block subsidy | `10 CTEAM` until the cap is reached |
| Masternode collateral | `4,000 CTEAM` |
| Post-PoS reward split | `4` to the staker, `6` to the masternode |
| Governance cycle | `10,080` blocks (`14 days`) |

Mainnet supply is capped in consensus. Transaction fees are paid to miners during the PoW bootstrap phase and burned once PoS is active. Post-v5.5 governance cycles can allocate up to `100,800 CTEAM` per full 14-day cycle.

Quick Start
-----------

For most users:

```bash
git clone <repository-url>
cd CTEAM
./build-depends.sh
./src/qt/cteam-qt
```

Build Scripts
-------------

Two build methods are available:

| Script | Type | Use Case | Output |
|--------|------|----------|--------|
| `build-depends.sh` | Static/Release | Production builds, distribution | `src/` |
| `build.sh` | Dynamic/Dev | Development, faster iteration | `build/` |

**Use `build-depends.sh`** for production wallets (static linking, portable binaries).
**Use `build.sh`** for development work (faster builds, uses shared libraries).

Build Options
-------------

Both scripts support:

```bash
--no-gui       # Daemon only (faster)
--debug        # Debug build
--clean        # Clean rebuild
--jobs N       # Parallel jobs (default: auto)
```

Examples:

```bash
./build-depends.sh --no-gui           # Daemon only
./build-depends.sh --jobs 2           # Limit to 2 jobs (low RAM)
./build.sh --debug                    # Debug build
```

Prerequisites
-------------

macOS:
```bash
xcode-select --install
brew install autoconf automake libtool pkg-config cmake
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

Ubuntu/Debian:
```bash
sudo apt-get install build-essential git autoconf automake libtool pkg-config python3 curl cmake ninja-build
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

Fedora:
```bash
sudo dnf install gcc-c++ git autoconf automake libtool pkgconfig python3 curl cmake ninja-build
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh
source ~/.cargo/env
```

See `doc/build-*.md` for detailed OS-specific instructions.

Running
-------

GUI wallet:
```bash
./src/qt/cteam-qt          # build-depends.sh output
./build/cteam-qt           # build.sh output
```

Daemon:
```bash
./src/cteamd -daemon
./src/cteam-cli getblockchaininfo
./src/cteam-cli stop
```

Testnet:
```bash
./src/cteamd -testnet -daemon
```

Data Directory
--------------

- macOS: `~/Library/Application Support/CTEAM`
- Linux: `~/.cteam`

Backup `wallet.dat` - it contains your private keys.

Troubleshooting
---------------

**"command not found: cargo"**
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

Advanced Build
--------------

CMake directly:
```bash
cmake -B build
cmake --build build -j 8
```

With presets (CMake 3.14+):
```bash
cmake --preset=vcpkg
cmake --build --preset=vcpkg -j 8
```

Manual autotools:
```bash
HOST="$(./depends/config.guess)"
make -C depends -j"$(nproc)" HOST="$HOST"
./autogen.sh
CONFIG_SITE="$(pwd)/depends/$HOST/share/config.site" ./configure
make -j"$(nproc)"
```

Documentation
-------------

- `doc/build-easy.md` - Detailed build guide
- `doc/build-macos.md` - macOS specific instructions
- `doc/build-linux.md` - Linux specific instructions  
- `doc/build-windows.md` - Windows (WSL) instructions
- `doc/build-unix.md` - Traditional Unix build

Qt Versions
-----------

Both build scripts use Qt6 by default. Qt5 is supported via manual configure:
```bash
./configure --with-gui=qt5
```

License
-------

MIT License. See `COPYING`.
