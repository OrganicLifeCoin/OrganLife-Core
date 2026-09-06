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

PQ masternode registry block validation is opt-in on disposable regtest nodes
only (`-nuparams=pq_masternodes:HEIGHT`). The height must be positive and PQ
payments must already be active at that height. It is disabled by default,
and cannot activate on public testnet or mainnet. Registration/update/revocation
and collateral-spend state is validated and undone with blocks. Startup checks
the stored activation height and registry tip; changing or disabling an indexed
activation requires an explicit rebuild.

This does not yet provide a usable masternode: registration transaction relay,
wallet collateral protection, operator RPC/Qt flows, service verification,
rewards and quorum finality remain unavailable. Do not use this opt-in mode with
a value-bearing wallet.

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
