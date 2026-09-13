# OrganicLife Core

OrganicLife Core provides the node software and desktop wallet for OrganicLife Coin (OLC).
The project uses post-quantum (PQ) signatures for payments, staking, masternodes, and governance.
Its code comes from PIVX and Bitcoin.

## Network status

This README describes the v1.1.0 fresh-mainnet launch and the existing PQ testnet.
Mainnet launch is scheduled for 2026-09-16 14:00 UTC (16:00 Europe/Prague).
The node can start before launch for wallet and operator preparation.
It must not accept or create mainnet blocks before launch time.
Testnet keeps its existing pinned checkpoint.
Mainnet includes two bootstrap peers on port `43721`.

## Post-quantum security

ML-DSA-44 signatures authorize coin transfers and staking.
Operators use ML-DSA-44 to sign masternode messages and finality votes.
Governance votes also use ML-DSA-44.

PQ addresses use Bech32m with a version-1, 32-byte commitment to the public key.
The commitment binds the address to its network.
Mainnet addresses use `olcpq`, testnet addresses use `olcpqtest`, and regtest
addresses use `olcpqregtest`.

ML-DSA is a signature algorithm, not a hash function or an encryption algorithm.
Transaction identifiers, block identifiers, and Merkle trees retain their 256-bit hash functions.
Wallet encryption protects private keys at rest.

The active PQ profile does not accept legacy coin transfers or legacy staking.
It does not provide legacy wallet compatibility, shielded payments, multisig, or delegated staking.
A successful test suite is not cryptographic certification.

## Mainnet parameters

| Parameter | v1.1.0 mainnet configuration |
| --- | --- |
| Ticker | `OLC` |
| Genesis launch time | `2026-09-16 14:00:00 UTC` |
| Genesis block hash | `0000091cf3aeeed50f65d6e640a35029d7b541b4e42950232385b13e88a12fdb` |
| Target block interval | `2 minutes` |
| Maximum supply | `777,777,777 OLC` |
| Block-1 reward | `264,444,444.18 OLC`, paid to the miner selected by block 1 |
| PQ registry and service activation | Height `1` |
| PoS activation | Height `10,081` |
| Block subsidy | `10 OLC`, subject to the supply cap |
| Masternode collateral | `4,000 OLC` |
| Normal reward after PoS activation | `4 OLC` for the staker and `6 OLC` for one eligible masternode |
| Governance cycle | `10,080` blocks, approximately `14 days` |
| Budget per governance cycle | `27,777.5 OLC`, subject to the supply cap |
| P2P port | `43721` |
| RPC port | `43723` |

The supply cap includes the block-1 reward, ordinary block rewards, masternode rewards, and governance payments.
The miner of block 1 receives the block-1 reward through the PQ address selected
by that miner. The configuration does not assign this reward to a fixed address.

Miners receive transaction fees during the Proof of Work (PoW) phase.
After Proof of Stake (PoS) starts, the network burns transaction fees.
Block intervals are targets, not fixed schedules.

## Wallet

The Qt wallet uses the normal Dashboard, Send, Receive, and transaction-history screens.
PQ payments do not require a separate wallet screen.
The wallet also provides coin control and shows immature rewards.

Each receive key and change key has an independent ML-DSA-44 seed.
The encrypted wallet stores these seeds.
A legacy private-key export or a BIP32 seed cannot replace a PQ wallet backup.

Address creation and payments require a new encrypted wallet snapshot.
The wallet completes this snapshot before it shows a new address or sends a payment.
The Send screen accepts one recipient per transaction.

### Prepare the wallet

1. Encrypt the wallet.
2. Select a private directory for automatic wallet backups.
3. Fully unlock the wallet before you create an address or send a payment.
4. Use the Receive screen to create an address.
5. Use the Send screen to send coins.
6. For staking without payment access, unlock the wallet for staking only.

CAUTION: Keep an independent copy of your wallet backup.
Loss of the private keys can prevent access to your coins.

### Wallet RPCs

| Command | Purpose |
| --- | --- |
| `getnewpqaddress` | Create a receive address with a new wallet backup |
| `listpqaddresses` | List PQ addresses |
| `listpqunspent` | List unspent PQ outputs |
| `sendpqtoaddress` | Send a PQ payment with a new wallet backup |
| `backupwallet` | Save a wallet backup |
| `getstakingstatus` | Show the staking status |

Use `organiclife-cli help COMMAND` for the arguments of each command.

## Masternodes

A masternode uses two separate roles.
The controller wallet holds the collateral and controls the funds.
The VPS runs the operator without a wallet.
Its credentials cannot spend the coins in the controller wallet.

Each masternode requires `4,000 OLC` in collateral.
Registration records the operator identity, service address, and payment address on the chain.
The controller creates the keys and collateral transaction through the wallet dialog.

### Create a masternode

The PQ wallet provides this workflow on mainnet and testnet.

1. Open the Masternodes screen in the controller wallet.
2. Select **Create Masternode**.
3. Enter a name.
4. Enter the numeric VPS IP address and service port.
5. Fully unlock the wallet.
6. If the wallet requests a backup directory, select a private directory.
7. Review the collateral, transaction fee, and backup destination.
8. Approve the registration transaction.
9. Wait for the registration to appear in a block.
10. Open the masternode information dialog.
11. Copy the server configuration to the clipboard.
12. Paste the text at the top of the private VPS configuration file.
13. Replace the old operator configuration for that node.
14. Restrict file access to the node account.
15. Restart the VPS node.
16. Clear the clipboard history.

The copied text contains only `pqoperatorid`, `pqoperatorconfig`, and `externalip`.
Keep the existing `disablewallet=1` setting and use the correct network and port.
This workflow does not require a separate credential-file transfer.
It does require an installed node and a reachable service port.

CAUTION: Keep operator credentials private.
Anyone with these credentials can operate the masternode.

CAUTION: Preserve the existing signing journal and checkpoint configuration.
An old journal backup can permit conflicting signatures.

Use one node instance and one data directory for each masternode.
Keep the collateral wallet off the VPS.

### Rewards and service

The network allocates one `6 OLC` masternode reward per eligible block, subject to the supply cap.
One selected masternode receives the whole reward.
The next selection advances through the eligible operators.
The reward is not `6 OLC` for every masternode in every block.

Signed service heartbeats establish recent activity for reward eligibility.
Registration alone does not guarantee payment.
Collateral maturity, service eligibility, and the consensus payment rules still apply.

## Finality

The PQ finality protocol uses signed votes from a registered operator committee.
A committee needs at least four members.
A four-member committee needs three signatures for a quorum.

Mainnet derives its first finality bootstrap from chain history after registry and
service activation. The initial committee requires at least four eligible operators.
Testnet keeps its configured checkpoint. Finality requires the correct historical committee.
Operator credentials alone do not grant voting authority.
Nodes retain finalized checkpoints and reject branches that conflict with them.

If voting becomes unavailable, finality can stop while staking and payments continue.
This separation prevents a missing finality quorum from blocking otherwise valid blocks.
It does not guarantee progress without eligible stakers or network connectivity.

CAUTION: Do not delete signing history to restart an operator.
A restart must preserve earlier votes and locks.

## Governance

Governance proposals and votes form part of the chain state.
Coin holders lock PQ coins and authorize votes with ML-DSA-44.
The network ranks proposals by net locked-coin votes within the cycle budget.

PoS blocks include the selected treasury payments.
Chain replay reconstructs proposals, vote locks, votes, and payments.
The PQ path does not use legacy BLS masternode votes or proposal broadcasts.

| Command | Purpose |
| --- | --- |
| `createpqproposal` | Create a proposal with a PQ payment address |
| `creategovvotelock` | Lock coins for a proposal vote |
| `castgovvote` | Cast a yes or no vote |
| `listgovlocks` | List governance coin locks |
| `getgovvotestatus` | Show the vote status |
| `getnextsuperblock` | Show the next treasury payment height |
| `getbudgetinfo` | Show budget information |

## Programs

| Program | Purpose |
| --- | --- |
| `organiclifed` | Node daemon |
| `organiclife-cli` | RPC command-line client |
| `organiclife-tx` | Transaction utility |
| `organiclife-qt` | Desktop wallet |

## Build from source

Read the build guide for your operating system before you install dependencies.

- [General build guide](doc/build-easy.md)
- [Linux](doc/build-linux.md)
- [macOS](doc/build-macos.md)
- [Windows and WSL](doc/build-windows.md)
- [Unix](doc/build-unix.md)

### Download the source

```bash
git clone https://github.com/OrganicLifeCoin/OrganLife-Core.git OrganicLifeCoin
cd OrganicLifeCoin
```

### Build a development wallet

```bash
./build.sh
```

The development script uses vcpkg and writes its output to `build/`.

### Build with static dependencies

```bash
./scripts/build-depends.sh --jobs 2
```

The depends script writes the daemon and command-line programs to `src/`.
It writes the Qt wallet to `src/qt/`.

| Script | Purpose |
| --- | --- |
| `build.sh` | Development build |
| `scripts/build-depends.sh` | Build with static dependencies |
| `scripts/build-depends-aarch64.sh` | ARM64 cross-build |
| `scripts/build-depends-windows.sh` | Windows cross-build |
| `scripts/build_all.sh` | Create Linux and Windows release packages |
| `scripts/build_mac.sh` | Create universal macOS packages |

The Linux and Windows packaging script writes packages to `dist/`.
Cross-builds require the target toolchains and dependencies.
A successful cross-build does not replace runtime tests on the target platform.

The v1.1.0 macOS packages are unsigned and do not have Apple notarization.
macOS can block the first launch of these packages.

Use `--help` for the supported flags of a build script.

## Run a node

Mainnet uses the default network and starts on P2P port `43721` and RPC port `43723`.
The testnet uses P2P port `49716` and RPC port `49718`.
Both networks include the seed addresses `2.29.11.56` and `2.29.14.202` on their separate P2P ports.
The mainnet and testnet services use separate data directories.

### Desktop wallet

For the mainnet depends build, run:

```bash
./src/qt/organiclife-qt
```

For testnet, run:

```bash
./src/qt/organiclife-qt -testnet
```

For the mainnet development build, run:

```bash
./build/organiclife-qt
```

For testnet, run:

```bash
./build/organiclife-qt -testnet
```

### Daemon

Run the mainnet daemon:

```bash
./src/organiclifed -daemon
```

Run the testnet daemon explicitly with `-testnet`:

```bash
./src/organiclifed -testnet -daemon
```

Read mainnet chain information:

```bash
./src/organiclife-cli getblockchaininfo
```

Read testnet chain information explicitly:

```bash
./src/organiclife-cli -testnet getblockchaininfo
```

Stop the testnet daemon:

```bash
./src/organiclife-cli -testnet stop
```

### Configuration and backups

The configuration file is `organiclifecoin.conf`.

| System | Default data directory |
| --- | --- |
| Linux | `~/.organiclifecoin` |
| macOS | `~/Library/Application Support/OrganicLife` |
| Windows | `%APPDATA%\OrganicLifeCoin` |

Testnet stores its network data in the `testnet` subdirectory.
Use separate data directories, wallets, and keys for mainnet and testnet.
The `-datadir` argument selects a different data directory.

CAUTION: Do not expose the RPC port to the public internet.
RPC access can control the node and wallet.

CAUTION: Do not copy a live wallet database as a recovery backup.
Use the wallet backup function.

## Tests

After a test-enabled build, run the core tests:

```bash
./src/test/test_organiclife
```

Run the Qt tests:

```bash
QT_QPA_PLATFORM=offscreen ./src/qt/test/test_organiclife-qt
```

The functional tests use disposable local nodes.

Read [the functional test guide](test/functional/README.md) for the test runner.
Read [the PQ implementation notes](doc/pq-only.md) for protocol details and historical activation stages.

## License

The project uses the MIT License.

Read [COPYING](COPYING) for the license text.
