OrganicLife Core
==============

Node daemon and Qt wallet for the OrganicLife Coin (OLC) network (PIVX/Bitcoin lineage).

Binaries
--------

- `organiclifed` - Full node daemon
- `organiclife-cli` - RPC command-line client  
- `organiclife-tx` - Transaction utility
- `organiclife-qt` - Qt GUI wallet

Mainnet Parameters
------------------

| Parameter | Value |
|--------|--------|
| Ticker | `OLC` |
| Genesis timestamp | `2026-08-02 12:00:00 UTC` |
| Genesis block hash | `0000012e114f3ce58cd05631b29091dc543db22061f852dc50b26967d082de6e` |
| Address prefixes | P2PKH starts with `o`, script `g`, staking `f` |
| P2P / RPC ports | `39616` / `39618` (testnet `49616` / `49618`) |
| BIP44 coin type | `5150` |
| Target block spacing | `2 minutes` |
| Supply cap | `777,777,777 OLC` |
| Height-1 reward | `264,444,444.18 OLC`, paid to the miner of block 1 |
| PoS activation | Height `10,081` (about `14 days` after genesis) |
| Block subsidy | `10 OLC` until the cap is reached |
| Masternode collateral | `4,000 OLC` |
| Post-PoS reward split | `4` to the staker, `6` to the masternode |
| Governance cycle | `10,080` blocks (`14 days`) |

Mainnet and testnet supply are capped in consensus at `777,777,777 OLC`, inclusive of the
height-1 reward, ordinary subsidies, masternode rewards, and governance payments. The
height-1 reward has no predetermined recipient: the valid block template pays whoever
mines block 1. Transaction fees are paid to miners during the PoW bootstrap phase and
burned once PoS is active. Post-v5.5 governance cycles can allocate up to `55,555 OLC`
per month (two 14-day cycles), subject to the same hard supply cap.

Early-network quorum policy
---------------------------

Mainnet and testnet intentionally use `LLMQ_TEST` for ChainLocks during early rollout:
3 members, a minimum of 2 participants, and a threshold of 2. The larger
`LLMQ_50_60` definition remains available but is not selected; its configured size is
50, minimum size 40, and signing threshold 30.

Height `100,000` is a review point, not an automatic activation. The project can assess
the observed masternode population there and choose an appropriate quorum in a future
explicit network upgrade. A spork does not automatically switch quorum type when a
participant count is reached, and no automatic one-way switch is currently configured.

Peer discovery before seed VPS hosts are available
---------------------------------------------------

This source tree intentionally has no mainnet or testnet DNS/fixed seeds yet. Early
testnet nodes must be given at least one reachable peer with `addnode=` or `-addnode`.
See [doc/seeders.md](doc/seeders.md). DNS and fixed seeds can be added after the VPS
listeners exist, without inventing placeholder production endpoints.

Masternodes (deterministic, since v1.1.0)
-----------------------------------------

OrganicLife uses deterministic masternodes (DMN) exclusively — the legacy broadcast-based
masternode system was removed. DMNs activate at genesis and are registered on-chain via
`protx` transactions; the 4,000 OLC collateral is embedded in the registration transaction.

Controller wallet setup (CLI):

```bash
# 1. generate the key set (owner/voting keys are stored in the wallet)
organiclife-cli createmasternodekey dmn mn1
# 2. add the returned confLine to masternode.conf:  alias IP:port operator_bls_key
# 3. register the masternode (embeds and locks the collateral)
organiclife-cli startmasternode alias false mn1
```

VPS setup: run the daemon with `-mnoperatorprivatekey=<operator_bls_key>` (one line in
organiclife.conf). No collateral txid/output index is needed in masternode.conf — the
collateral is created and locked by the registration itself.

Quick Start
-----------

For most users:

```bash
git clone <repository-url>
cd OrganicLifeCoin
./build-depends.sh
./src/qt/organiclife-qt
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
./src/qt/organiclife-qt          # build-depends.sh output
./build/organiclife-qt           # build.sh output
```

Daemon:
```bash
./src/organiclifed -daemon
./src/organiclife-cli getblockchaininfo
./src/organiclife-cli stop
```

Testnet:
```bash
./src/organiclifed -testnet -daemon
```

Data Directory
--------------

- macOS: `~/Library/Application Support/OrganicLife`
- Linux: `~/.organiclifecoin`

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

**"block index lacks cumulative issuance data"**

The hard-cap implementation stores cumulative issuance in every block index entry.
Development datadirs created by an older binary must be rebuilt once with `-reindex`.

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
