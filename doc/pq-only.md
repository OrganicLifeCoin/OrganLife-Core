# PQ-only network reference

OrganicLifeCoin mainnet, testnet, and regtest use ML-DSA-44 for payments,
staking, operator signatures, and governance. This is a clean incompatible
chain. Legacy wallets, addresses, transactions, staking, multisig, shielded
payments, delegation, WIF, and BIP32 seeds are not accepted.

## Network profiles

### Mainnet

- Release profile: v1.1.0 fresh mainnet.
- Genesis launch: 2026-09-16 14:00 UTC (16:00 Europe/Prague).
- Genesis hash: `0000091cf3aeeed50f65d6e640a35029d7b541b4e42950232385b13e88a12fdb`.
- PQ address HRP: `olcpq`.
- P2P and RPC ports: `43721` and `43723`.
- PQ registry and service activation: height 1.
- PoS activation: height 10,081.
- Bootstrap peers: `2.29.11.56:43721` and `2.29.14.202:43721`.

The daemon can start before genesis for wallet and operator preparation. It must
not accept or create a non-genesis mainnet block before the launch time.
Mainnet and testnet use separate services and data directories on the bootstrap hosts.
Never use a testnet peer for mainnet.

The block-1 reward is `264,444,444.18 OLC`. The miner selected for block 1
chooses the PQ payout address. The chain does not assign this reward to a fixed
address. The supply cap is `777,777,777 OLC`.

Mainnet derives its first finality bootstrap from validated chain history after
registry and service activation. The first historical committee needs at least
four mature, non-revoked, service-configured operators. A missing quorum pauses
finality; it does not stop ordinary payments or PoS production.

### Testnet

Testnet retains its existing genesis, network magic, configured peers, and
pinned finality checkpoint. Its PQ address HRP is `olcpqtest`, P2P port is
`49716`, and RPC port is `49718`. Public testnet registry and service rules use
their configured activation height. Use a separate data directory, wallet, and
key set from mainnet.

### Regtest

Regtest uses HRP `olcpqregtest` and explicit local activation settings. The
regtest options described below are for disposable local qualification and must
not be used as mainnet recovery controls.

## Wallet and backup safety

Encrypt the wallet before creating an address or sending a payment. The Qt
Dashboard, Send, Receive, and history screens use the PQ path automatically.
Each receive and change key has an independent ML-DSA-44 seed stored in the
encrypted wallet. A text private-key export, WIF, or BIP32 seed cannot replace
a PQ wallet backup.

Address creation and payment complete a new binary wallet snapshot before the
address or transaction is exposed. Automatic snapshots create a new file in a
trusted backup directory, flush it, compare it with the wallet checkpoint, and
do not replace existing files or links. A failed snapshot may leave an
incomplete new file; do not use it for recovery. The ordinary `backupwallet`
RPC keeps its existing overwrite behavior.

Filesystem or hardware failure can still defeat recovery. These checks are not
power-loss certification. Keep an independent, tested encrypted backup. Wallet
metadata is not fully encrypted, so use a private backup directory when its
metadata requires protection. Platform ACL inheritance remains the owner's
responsibility.

Useful RPCs are:

- `getnewpqaddress backup_destination`
- `listpqaddresses`
- `listpqunspent`
- `sendpqtoaddress address amount backup_destination ( selected_inputs )`
- `backupwallet destination`
- `getstakingstatus`

## Operator and masternode safety

A controller wallet holds the collateral and controls funds. The operator host
runs without the controller wallet. Operator credentials authorize service and
signing actions; they cannot spend controller funds. Keep the collateral wallet
off the operator host.

Each registration requires `4,000 OLC` collateral, an operator identity, a
service endpoint, and a payout address. Registration, update, service, and
revocation state is validated against the confirmed registry and undone with
blocks. Collateral spends remove the registration. Revoked, immature, missing,
or endpoint-less registrations are not eligible.

The controller information dialog copies exactly these settings:

```text
pqoperatorid
pqoperatorconfig
externalip
```

Paste them into the existing operator configuration. Keep `disablewallet=1`,
the correct network selection, and the registered service port. Do not copy a
controller wallet. Protect the configuration and clear clipboard history.
Keep operator credentials private. Anyone holding them can operate the node,
but the credentials do not grant access to controller funds.

Use one node instance and data directory for each operator. Preserve the
operator signing journal and checkpoint history. Never delete, restore, clone,
or roll back journal state to bypass a signing refusal. Rotate the operator key
through the controller when its exclusive history cannot be proven.

Operator record files use authenticated network and genesis binding, an
independent wrapping key, and strict file checks. Protected files must have
trusted ancestors, safe ownership, safe permissions, and no unsafe links.
The loader rejects wrong-chain records, malformed payloads, and ambiguous sources.
It does not protect against a
privileged administrator or a compromised service account.

On Windows, operator files require local fixed NTFS, ownership by the process
user, a nonempty DACL granting access to that user or LocalSystem, and safe
single-link regular files. Reparse points, UNC paths, alternate streams,
device paths, aliases, wrong sizes, and concurrent writers are rejected.
Native Windows checks use handle-based security inspection. Cross-compilation
does not qualify runtime behavior; run the native executable on local NTFS.

## Transactions and consensus

PQ transfers use transaction version 3, type 8, and payload version 1. Transfer
mode has one or two ML-DSA inputs and one or two PQ outputs. Stake mode has one
ML-DSA input, an empty coinstake marker, a staker output, and an optional masternode output. Coinbase and
stake rewards use PQ outputs, and matured rewards require ML-DSA authorization.

Transaction, block, Merkle, and staking identifiers retain 256-bit hashes.
ML-DSA is a signature algorithm, not a hash or encryption algorithm. Transaction
and block signatures bind the network genesis and use separate PQ contexts.
Non-PQ payments and staking are invalid after their network activation height;
pending invalid descendants are removed from the mempool.

Masternode fee inputs and external collateral must be confirmed. One operation
per registration may be pending. Registration updates, collateral spends,
ordinary child transactions, block connection, reorganization, and mempool
removal preserve the registry invariants. Automatic wallet funding and staking
exclude collateral, including revoked collateral. Explicit coin control may
select confirmed collateral and deliberately remove that registration.

The optional `sendpqtoaddress` input list selects one or two unique confirmed
inputs. No other inputs are added. Malformed, unavailable, locked, or
insufficient selections fail before relay. Controller registration requires a
verified encrypted wallet snapshot. The Qt dialog asks for fee and action approval.

Masternode rewards use one whole `6 OLC` network share per eligible block,
subject to the supply cap. One selected operator receives the share. Selection
uses deterministic eligible-operator state; registration alone does not ensure
payment. Service activity, collateral maturity, endpoint status, and revocation
remain consensus requirements.

## Governance

Governance is on-chain and PQ-only on all activated profiles. `createpqproposal`
creates a proposal with a PQ payment address. `creategovvotelock` locks PQ coins
until the proposal ends. `castgovvote` authorizes a vote with ML-DSA-44.
`listgovlocks`, `getgovvotestatus`, `getnextsuperblock`, and `getbudgetinfo`
report the resulting state.

Legacy proposal broadcasts, masternode votes, and finalized-budget messages are
ignored while PQ governance is active. Net locked-coin votes rank proposals
within the cycle budget. PoS blocks must include the exact selected PQ treasury
payment. Reorganization and reindex restore proposals, locks, votes, and
payments from the chain.

## Finality and service activity

Finality uses signed votes from a registered operator committee. Four eligible
members require three signatures for quorum. Operator credentials do not grant
voting authority. Nodes retain finalized checkpoints and reject conflicting
branches. Voting loss pauses finality, not ordinary payments or staking.

The first mainnet committee comes from validated historical registry state.
Later registrations do not replace that initial bootstrap committee. Before a
valid quorum certificate, the derived checkpoint remains rollbackable. After a
certificate, durable anchor protection applies. A reorganization that changes
an operator's signing context must not reuse its earlier votes or locks.

Service heartbeats and operator proofs bind the genesis, registration, operator
key, registry sequence, and a recent ancestor. Revocation, rotation, collateral
immaturity, and stale endpoints invalidate service eligibility. Service proofs
do not prove endpoint ownership, encryption, rewards, or committee membership.

## Regtest qualification controls

The following controls are for disposable regtest qualification:

- `-nuparams=pq_masternodes:HEIGHT` selects a positive registry activation height.
- `-nuparams=pq_service:HEIGHT` selects service-heartbeat activation.
- `-pqautobootstrap=1` exercises automatic finality bootstrap locally.

On test chains, `-pqemergencycheckpoint=HEIGHT:BLOCKHASH` supports approved recovery
from a historical committee with fewer than four members and an existing durable anchor.

These controls require local validated history and do not authorize peer-selected
checkpoints, mainnet recovery, or bypassing journal protection. Emergency
recovery retains anchors, certificates, and signing journals. Healthy backlogs,
missing snapshots, wrong ancestry, and unsigned checkpoints do not qualify.

Regtest fault-injection and native platform tests cover wallet snapshots,
credential ACLs, journal append and flush failures, replacement failures,
reindex, restart, and process crashes. They are process-crash checks, not
physical power-loss, cryptographic certification, or deployment qualification.
Run Windows credential and journal executables on native Windows with `TEMP` on
local NTFS. Cross-built binaries require native runtime checks.

## Recovery invariants

Startup refuses inconsistent UTXO, EvoDB, chainstate, credential, or journal
metadata. It does not silently rebuild or delete wallet data. After a wallet
backup, use explicit `-reindex` or `-reindex-chainstate` to rebuild from existing
block files. Reindex does not reset the chain or remove the wallet.

Signing requires the existing journal and lock files. Missing or partial files
refuse signing. Journal growth limits stop new signing, not PoS. Preserve the
files outside chainstate and rotate the operator key when exclusive history is
lost. These rules protect against accidental reuse; they do not defend against
a privileged host administrator or cloned state on another host.
