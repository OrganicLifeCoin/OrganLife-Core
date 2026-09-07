# PQ-only pre-launch network

OrganicLifeCoin testnet and regtest use ML-DSA-44 for spend authorization,
staking ownership, and proof-of-stake block signatures. Mainnet is disabled in
this pre-launch build. This is an incompatible clean chain; no old wallet,
address, transaction, or chain compatibility is provided.

## Wallet use

Encrypt and fully unlock a new wallet. The Qt wallet uses its normal Dashboard,
Send, Receive, and transaction-history screens; addresses and payments use the
PQ path automatically. The matching RPCs are:

- `getnewpqaddress backup_destination`
- `listpqaddresses`
- `listpqunspent`
- `sendpqtoaddress address amount backup_destination`
- `backupwallet destination`

Every new receive or change key is an independent ML-DSA-44 seed stored only in
the encrypted wallet. Address creation and payments complete a new binary wallet
backup before exposing the address or relaying the transaction. There is no
text private-key export, WIF, BIP32 seed, classical address, multisig, message
signing, shielded payment, or delegation. The familiar Qt Send screen is a PQ
frontend and accepts one recipient per transaction.

Automatic PQ snapshots require an encrypted wallet and a new file in a trusted
backup directory. They exclusively create the destination, flush it and compare
its contents with the checkpointed wallet before reporting success; POSIX also
flushes the parent directory. Existing files (including destination links) are
not overwritten. A failed attempt may leave an incomplete new file: do not use
it for recovery, and choose a new filename when retrying. The ordinary manual
`backupwallet` command retains its existing overwrite behavior. Filesystem or
hardware failure can still defeat recovery; these checks are not power-loss
certification or a substitute for an independently stored, tested backup.
Wallet metadata is not all encrypted. Choose a private backup directory when
metadata confidentiality matters; inherited Windows/macOS ACLs remain the
directory owner's policy, not an owner-only access guarantee from this API.

Addresses use canonical Bech32m with witness version 1, a 32-byte network-bound
public-key commitment, and the HRP `olcpqregtest` or `olcpqtest`. Legacy Bech32
checksums are rejected.

## Consensus profile

- Transaction version 3, type 8, payload version 1.
- TRANSFER mode has one or two ML-DSA-authorized PQ inputs and one or two PQ
  outputs.
- STAKE mode has one ML-DSA-authorized PQ input, one empty coinstake marker, and
  one PQ reward output.
- Coinbase rewards use PQ outputs; spending a matured reward requires ML-DSA.
- A PoS block signature contains its ML-DSA public key and signature. The key
  commitment must match the coinstake owner.
- Transaction and block signatures use separate regtest/testnet FIPS 204
  contexts and bind the network genesis.
- Non-PQ transactions and staking are invalid after activation. Activation also
  removes pending non-PQ mempool transactions and descendants.

Transaction, Merkle, block, and staking identifiers retain their 256-bit hash
functions. ML-DSA is a signature algorithm, not a replacement hash.

## Governance

Testnet and regtest governance is on-chain and PQ-only:

- `createpqproposal` creates a proposal with a PQ payment address.
- `creategovvotelock` locks PQ coins until after the proposal ends.
- `castgovvote` authorizes a yes or no vote with ML-DSA-44.
- `listgovlocks`, `getgovvotestatus`, `getnextsuperblock`, and `getbudgetinfo`
  expose the resulting chain state.

Legacy proposal broadcasts, masternode votes, and finalized-budget messages are
ignored while PQ governance is active. A deterministic ranking by net locked
coin votes selects proposals within the cycle budget. Proof-of-stake blocks
must include the exact selected PQ treasury payment, and reorganization or
reindex restores the proposal, lock, vote, and payout state from the chain.

## Current safety boundary

PQ masternode registry block validation and transaction relay are opt-in on disposable regtest nodes
only (`-nuparams=pq_masternodes:HEIGHT`). The height must be positive and PQ
payments must already be active at that height. It is disabled by default,
and cannot activate on public testnet or mainnet. Registration/update/revocation
and collateral-spend state is validated and undone with blocks. Startup checks
the stored activation height and registry tip; changing or disabling an indexed
activation requires an explicit rebuild.

Relay validates against confirmed registry state without updating it. MN fee
inputs and external collateral must be confirmed; dependent registry updates
wait for confirmation. One operation per registration may be pending, with
exclusive role keys, endpoints and collateral. A pending operation and an
ordinary collateral spend conflict in either arrival order. Ordinary children
may spend non-collateral change. Block connection and reorgs revalidate pending
operations and remove invalid descendants.

Automatic wallet funding/staking excludes confirmed and pending collateral,
including revoked registrations. Listings/total balance retain it; explicit
payment coin control may select confirmed collateral, but manual locks and
pending-operation relay conflicts still apply. Protection follows registry undo
and mempool removal without persistent automatic wallet locks.

The wallet core can construct and validate registration (internal or external
collateral), owner updates/rotation, service updates and revocation in this
opt-in mode. Owner and collateral signing use the fully unlocked encrypted
controller wallet; a separate caller-owned operator signer is required where
appropriate. Fees use eligible confirmed coins, never the registration's own
external bond. Change returns to the first fee-input key, without creating a
new key. Construction checks the confirmed registry but does not reserve inputs,
commit, broadcast, export keys or take backups. Operator provisioning and the
backup-before-publication RPC/Qt workflows still need integration; this is not
an unattended remote operator signing interface.

An isolated encrypted operator-record API now uses a separate storage KDF and
authenticated-data domain, version 2, and an explicit nonzero genesis binding.
It cannot be relabeled as a version-1 spending-wallet record. The existing
wallet record format and encryption remain unchanged. Provisioning must use a
fresh operator seed, an independent random 32-byte wrapping key, and trusted
network/genesis. This does not implement protected files, wrapping-key custody,
distribution, wallet
import, or rollback-safe finality signing. Never distribute a controller wallet
master key to an operator host.

For isolated regtest testing, a daemon can load pending operator credentials
using paired `-pqoperatorcredentials=/absolute/private/directory` and
`-pqoperatorid=REGISTRATION_TXID` options, each specified exactly once. The ID
must be nonzero and exactly 64 hexadecimal characters. PQ masternode activation
must be configured, and wallet-enabled builds require `-disablewallet` to keep
controller spending keys separate. The directory supplies fixed files
`olc-pq-operator-record` (1385 bytes) and `olc-pq-operator-key` (32 bytes).
The read-only loader rejects unsafe ownership, permissions, ACLs, links and
invalid encrypted records; directory ancestors and the service identity must
be trusted. Linux supports owner-private files or the narrowly validated
systemd credential ACL; macOS supports owner-private files without extended
ACLs. Other platforms fail closed.

Credentials load before RPC workers and are destroyed after worker shutdown,
including failed startup. `getpqoperatorinfo` returns only configured status
and the public registration/key identity. Unknown or unconfirmed registrations
can still sync: loaded credentials do not imply registry validity, signing
authority, authentication, service eligibility or finality readiness. No general
signing or plaintext private-key export RPC is provided. Provisioning, encrypted persistent
wrapping-key custody and backup/recovery remain separate unfinished work.

Controller recovery-key creation is available only with opt-in regtest
masternodes: `createpqoperator backup_destination` requires an encrypted, fully
unlocked wallet. It persists one pending independent operator seed, completes
an exclusive verified wallet snapshot, then marks the recovery key backed before
returning its public key. `listpqoperators` lists only backed recovery keys,
including while the wallet is locked; it is not a registration or service-status
query. A failed backup retains the private pending record, and the next creation
request retries that record before generating another. The latest key in a
restored snapshot is pending and needs a new snapshot before republication.

These controller records use a distinct version-3 recovery encryption domain
and a genesis-bound wallet DB namespace. They are authenticated when unlocking
and are not added to the wallet's spending keys. They must not be supplied to a server credential
directory: version-2 deployment records still require independently generated
wrapping keys. No private export, arbitrary signing, registration broadcast or
operator service start is exposed by these recovery RPCs. Native provisioning,
rotation and rollback-safe finality signing still need integration.

The controller can now prepare and broadcast registry lifecycle transactions on
opt-in regtest with `sendpqmasternode action options backup_destination`. The
third argument is a new encrypted wallet snapshot, completed before commit and
relay. It uses existing spending/owner keys and backed operator recovery keys;
no private key is accepted or returned. Change reuses an existing fee-input key.

- `register`: options require `collateral_address`, `owner_address`,
  `operator_publickey` and `payout_address`. Owner and collateral keys must be
  distinct wallet-owned keys, separate from the operator. By default the
  transaction creates the exact network collateral at output zero. Supply both
  `collateral_txid` and `collateral_vout` to use an existing confirmed bond,
  which is excluded from fee funding. Optional `service` is a numeric IP:port,
  including bracketed IPv6. `operator_reward` is 0..10000 basis points; a nonzero
  commission requires `operator_payout_address`.
- `update`: requires `registration`, `sequence`, `payout_address` and
  `operator_publickey`. Reusing the current operator changes the owner payout;
  a backed replacement operator rotates the key, revives a revoked registration
  and clears its endpoint/operator payout.
- `service`: requires `registration`, `sequence` and `service`. Omitting
  `operator_payout_address` preserves the current payout. After rotation, a
  commission-bearing operator must supply a new payout.
- `revoke`: requires `registration` and `sequence`; disables the registration
  without moving its collateral. Spending collateral removes the registration.

`sequence` is the next unsigned decimal **string**, not a JSON number. All
updates require confirmed prior state. `listpqmasternodes` works while locked
or without a wallet and returns the confirmed chain registry, not mempool or
service status. Inspect it and wallet transaction history before retrying a
request whose response was lost; do not blindly repeat collateral funding.
The RPC help contains the full option contract. Ordinary CLI JSON conversion
is supported for the options object.

The familiar Qt Masternodes page is available on regtest. It reads the current
confirmed registry (including registrations not controlled by the selected
wallet), shows immature/registered/revoked state, and offers registration,
service updates, payout updates/operator rotation and revocation. It uses
existing wallet addresses and backed operator identities. The Operator keys
dialog can create an identity after a verified encrypted recovery backup, or
export a backed identity into a new private credential directory. Its two files
are operator secrets, not wallet spending keys: securely transfer and seal both
on the operator host. Existing destinations are never overwritten. Failed
creation backups leave an unpublished pending identity for the next retry.
`createpqoperator` and `exportpqoperator` remain available through RPC.
Registration currently creates a new collateral output with
zero operator commission; existing bonds and commission
remain available through the RPC workflow. Coin Control selects up to two funding
inputs; with no selection the builder chooses automatically. Selected coins must
remain eligible, and neither an active bond nor the registration's external bond
can fund masternode fees. Selection clears after successful submission or a wallet
switch. Remote start/stop is not yet connected to this page.
Every transaction requires full unlock, a verified encrypted snapshot and a
fee/action confirmation before submission. Cancellation or a wallet switch
before confirmation does not submit a transaction. Registry refresh invalidates
row selections, and stale sequence checks remain enforced by the shared core.
The information dialog shows public data only, never private operator material.

Opt-in regtest blocks now enforce one whole masternode share per block, selected
from the parent registry. The nominal share is 6 OLC, funded from the existing
block subsidy, not additional issuance. The existing subsidy cap still applies.
An explicitly configured operator commission divides only the winning node's
share; zero commission sends the whole share to its controller payout address.
The queue selects the lowest effective last-paid/revival height, falling back to
registration height, with registration hash as the deterministic tie break.
Four stable eligible registrations therefore rotate A/B/C/D/A. Revoked,
unconfigured or immature registrations are excluded. Restoring an empty service
endpoint sets the revival height; an ordinary endpoint edit does not reset it.
`listpqmasternodes` exposes `last_paid_height` and `revived_height`.

PoW places the share after the miner's output. PoS places it in coinbase and
subtracts it from the stake return before signing. Governance remains separately
funded and validated, even when its recipient matches a masternode payout.
Missing, incorrect or noncanonical payouts fail block validation. Payment queue
changes and their undo are persisted with the registry, including same-block
registration updates and collateral spends.

This changes the opt-in registry record/schema to version 2 and changes its
block payment rules. Old active-regtest databases are rejected; reindex alone
cannot make a formerly valid unpaid block history satisfy the new rules. Use a
separate clean regtest data directory. Do not delete wallets to upgrade. Public
testnet/mainnet activation remains unchanged.

This still does not provide fully operational masternodes: a configured endpoint
is not evidence of online service, and offline nodes are not yet automatically
excluded. Quorum service enforcement and finality runtime remain unavailable.
Public-network and cross-platform qualification remain pending. Do not use this
opt-in mode with a value-bearing wallet.

The isolated operator-authentication component signs a fixed, versioned proof
with the registered ML-DSA operator key. It binds genesis, registration, operator
key, both connection challenges and signer direction using a separate signature
context. Its connection-owned session allows one attempt, rejects malformed or
ineligible identities, and rechecks the current registry before returning an
authenticated identity. Rotation, revocation, removal or loss of maturity clears
that identity permanently. It writes no registry state.

Opt-in regtest now exchanges this identity on ordinary peer connections.
Configured operators initiate after VERSION/VERACK and initial synchronization;
ordinary nodes respond but need not sign. Each direction sends at most one
`pqhello` (exactly65 bytes: version1, random32-byte challenge,32-byte tip hash)
and one `pqauth` (the existing2453-byte proof, no vector/length prefix).
TCP direction determines initiator/responder, regardless of hello order. The
tip is an untrusted synchronization hint, not a signed anchor or authority:
different tips leave the connection usable but unauthenticated until reconnect.
Signers require matching current registry/key, active network, maturity and no
revocation. Incoming proofs use the same current confirmed state checks.

The authentication window is30 seconds from local hello, measured by a monotonic
clock. Missing proof is normal; malformed, duplicate, out-of-order or late PQ
frames disconnect. Ordinary traffic remains available after the window closes.
A shared fixed-window ceiling allows16 signing/verification attempts per second under the
existing chain lock, with no verification queue. Exhaustion leaves the affected
connection unauthenticated; reconnect to retry. This is not production traffic
or adversarial resource qualification. State is bounded by existing peer limits.

`getpeerinfo` exposes `pq_registration` only while its session still matches the
active confirmed registry. Send processing also rechecks it. On recheck, loss of
maturity, rotation, revocation or registry rollback invalidates the session permanently;
restoring the registration requires a new connection. Multiple connections with
the same identity are allowed, but receive no additional authority or weight.
Legacy MNAUTH fields and privileges are not populated. This proves key control,
not encryption, endpoint ownership, service, rewards or committee membership.
A transparent intermediary can forward the handshake; it is not channel binding
for secrets or finality. Public activation is unchanged.

The testnet has a fresh genesis and network magic. Mainnet startup is refused.
Sapling parameters and tier-two services are not initialized, and their RPC
surfaces are absent. This remains test software until the complete local suite,
cross-platform builds, isolated public-testnet reset, and independent
cryptographic and consensus review are complete.

Startup refuses inconsistent UTXO and EvoDB tips, an interrupted PQ UTXO batch,
or malformed/unknown chainstate tip metadata before attempting replay. It does
not automatically rebuild these databases, including when unknown metadata
prevents identifying whether PQ was active. Valid pre-PQ replay is unchanged.
After backing up the wallet, explicitly restart with `-reindex` to
rebuild from the existing block files. `-reindex-chainstate` also rebuilds EvoDB
so governance state is reconstructed together with the UTXO set. These options
do not reset the chain or delete the wallet.

Primary local qualification commands are:

```sh
src/test/test_organiclife --run_test=mldsa_tests,pqkey_tests,pqtransaction_tests,pqtestnet_validation_tests,pqvalidation_tests,pqwallet_tests,pqwallet_testnet_tests,main_tests/block_signature_test
python3 test/functional/feature_pq_only.py
python3 test/functional/feature_pq_pos.py
python3 test/functional/feature_pq_governance.py
python3 test/functional/feature_pq_startup.py
python3 test/functional/feature_pq_registry.py
python3 test/functional/feature_pq_operator.py
python3 test/functional/feature_pq_operator_recovery.py
python3 test/functional/feature_pq_pos.py --pq-registry
```

The functional tests use disposable local nodes and verify backup/restore,
restart, reindex, transfer, staking-only unlock, PoS signing, rollback, and
reconsideration. Governance coverage includes proposal persistence, PQ coin
locks, ML-DSA vote authorization, deterministic treasury payment, rollback,
reconsideration, and both reindex modes. Startup tests cover stale indexes,
repeated refusal without changing block files, interrupted coin-batch process
exit, and explicit rebuild recovery. Test success is not cryptographic
certification.
