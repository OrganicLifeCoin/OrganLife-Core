# PQ-only pre-launch network

OrganicLifeCoin testnet and regtest use ML-DSA-44 for spend authorization,
staking ownership, and proof-of-stake block signatures. Mainnet is disabled in
this pre-launch build. This is an incompatible clean chain; no old wallet,
address, transaction, or chain compatibility is provided.

## Wallet use

Encrypt and fully unlock a new wallet, then use:

- `getnewpqaddress backup_destination`
- `listpqaddresses`
- `listpqunspent`
- `sendpqtoaddress address amount backup_destination`
- `backupwallet destination`

Every new receive or change key is an independent ML-DSA-44 seed stored only in
the encrypted wallet. Address creation and payments complete a new binary wallet
backup before exposing the address or relaying the transaction. There is no
text private-key export, WIF, BIP32 seed, classical address, multisig, message
signing, shielded payment, delegation, or ordinary send path.

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

The testnet has a fresh genesis and network magic. Mainnet startup is refused.
Sapling parameters and tier-two services are not initialized, and their RPC
surfaces are absent. This remains test software until the complete local suite,
cross-platform builds, isolated public-testnet reset, and independent
cryptographic and consensus review are complete.

Primary local qualification commands are:

```sh
src/test/test_organiclife --run_test=mldsa_tests,pqkey_tests,pqtransaction_tests,pqtestnet_validation_tests,pqvalidation_tests,pqwallet_tests,pqwallet_testnet_tests,main_tests/block_signature_test
python3 test/functional/feature_pq_only.py
python3 test/functional/feature_pq_pos.py
python3 test/functional/feature_pq_governance.py
```

The functional tests use disposable local nodes and verify backup/restore,
restart, reindex, transfer, staking-only unlock, PoS signing, rollback, and
reconsideration. Governance coverage includes proposal persistence, PQ coin
locks, ML-DSA vote authorization, deterministic treasury payment, rollback,
reconsideration, and full reindex. Test success is not cryptographic
certification.
