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
- `sendpqtoaddress address amount backup_destination ( selected_inputs )`
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

The optional `sendpqtoaddress` fourth argument selects exactly one or two unique
`{"txid":"64-character hex", "vout":0}` inputs. No other inputs are added.
Selecting a collateral output deliberately spends the bond and removes that
masternode; omitting the argument retains automatic collateral protection.
Malformed, unavailable, manually locked and insufficient selections fail without
relay. The encrypted wallet backup is still required before broadcast.

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
ACLs. Windows requires local fixed NTFS, ownership by the current process user,
and a nonempty DACL granting access only to that user or LocalSystem. It rejects
reparse points, multiple file links, device/UNC/alternate-stream paths, aliases,
wrong sizes and concurrent writers. The ordinary absolute drive path may contain
Unicode. Trusted ancestors and service identity are still required; the loader
does not repair permissions or provide protection from administrators or a
compromised service account. Other platforms fail closed.

The Windows policy uses handle-based
[GetSecurityInfo](https://learn.microsoft.com/en-us/windows/win32/api/aclapi/nf-aclapi-getsecurityinfo)
inspection rather than a path permission precheck. A standalone native reader
regression needs only the installed MinGW compiler (or the normal Windows test
build), not node/Rust dependencies:

```sh
x86_64-w64-mingw32-g++ -std=c++17 -O1 -DWIN32 -UNDEBUG -static -Isrc \
  src/test/pqcredentials_win_tests.cpp src/wallet/pqcredentials_win.cpp \
  src/support/lockedpool.cpp src/support/cleanse.cpp \
  -ladvapi32 -o pqcredentials_win_tests.exe
```

Run that executable on native Windows with `TEMP` on local NTFS. It creates only
randomly named synthetic fixtures, deletes successful fixtures, and reports any
unavailable symlink/alternate-owner checks explicitly. Cross-compilation alone
does not qualify runtime behavior. Its byte-reader checks do not replace full
wallet cryptography, daemon startup or deployment tests.

The operator, operator-recovery and finality-smoke functional tests also run on
native Windows with `test_runner.py --force --skipcache`. Their disposable
credential fixtures use Windows PowerShell to set and check protected owner-only
ACLs, including explicit Everyone-read access for the unsafe-permission rejection
case. Run them with the test directory on local NTFS; POSIX runs retain their
exact permission-mode checks. Successful startup and export still exercise the
daemon's real credential validation, not a replacement in the test helper.

The isolated `src/test/mldsa` CMake build additionally provides a Windows
`test_pqcredentials` target using the actual wallet encryption, credential
loader, ML-DSA signing and credential publisher. It checks wrong keys, networks,
genesis hashes, tampered fields, spending/recovery record domains, and clearing
an existing key after failed reload. Run both that target and `test_mldsa`
natively; a successful cross-build is not sufficient.
The MinGW credential target also intercepts native calls at link time to test
partial writes, failure of either file flush and failure of the final move.
Each failure must leave the destination absent, preserve an existing export
and remove its staging files. These hooks exist only in the test executable.

Windows controller credential export uses the same version-2 format as Linux
and macOS. It requires a private trusted local-NTFS parent, creates owner-only
protected ACLs before writing either file, flushes and verifies both files
(using the wallet's secure allocator for wrapping-key readback), and publishes
with a same-parent, non-replacing
[write-through directory move](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw).
Existing files, directories and links are never overwritten. Failed publication
cleans only the operation's staging files; do not use a failed destination.
The parent and ancestors must remain trusted through the final move, which
requires closing their read handles. These native API checks are not physical
power-loss certification or encrypted host custody; protect both exported files
on the operator host and retain the controller recovery backup.

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

The familiar Qt Masternodes page is available on activated test chains. It reads the current
confirmed registry (including registrations not controlled by the selected
wallet), shows immature/registered/revoked state, and offers registration,
service updates, payout updates/operator rotation and revocation. **Create Masternode**
reuses the original name/IP wizard; the port defaults to the network port and
can be changed for multiple operators on one VPS. It automatically prepares
distinct owner, collateral and payout addresses plus a backed operator identity,
then shows the collateral, fee and encrypted backup before submission. No manual
address or operator-key selection is required. Cancelling the input wizard writes
nothing. Declining the final fee confirmation sends no transaction, but retains
prepared keys and encrypted backups; these are not registered masternodes.
Names are local UI preferences, not consensus data, and may need restoring after
migrating the controller to another computer.

The original information dialog exports the selected operator's two secret files,
a non-secret `organiclifecoin.conf` and `README.txt` into a new private directory.
Only the non-secret configuration is copied to the clipboard. The generated
configuration isolates each operator's data and signing history, puts port settings
in the correct network section, disables the wallet and binds RPC to loopback.
It does not deploy or start a server, initialize signing history, or choose a
finality checkpoint. Follow the first-start sequence below and the exported README.
The advanced Operator keys dialog can create an identity after a verified encrypted recovery backup, or
export a backed identity into a new private credential directory. Its two files
are operator secrets, not wallet spending keys: securely transfer and seal both
on the operator host. Full-wallet `.dat` backups stay on the controller; they are
never part of the server export. Existing destinations are never overwritten. Failed
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

Before `pq_service` activation, service credit comes from certificate **carrier blocks** in the preceding
24 committed blocks on regtest, not the older blocks those certificates finalize.
A certificate in the block being paid cannot affect that block's payee. The
carrier index is undone with the carrier and reconstructed by reindex. With a
pinned bootstrap and any certificate in that window, eligible registrations
must have signed a certificate in the window or still be within registration
grace. If the window has no certificates, only the evidence requirement is
waived: revoked, immature and endpoint-less registrations remain ineligible.
`listpqmasternodes` reports eligibility for the next block, not a guarantee of
winning it; `last_cert_height` is the latest qualifying carrier height.
Certificate omission by block producers remains a fairness/qualification limit.

The opt-in registry/index schema is now version 4 (record version 3). Older experimental databases
require explicit reindex; there is no automatic reset or wallet deletion. A
history made under different payment/service rules may still fail replay, so
use a separate disposable regtest directory in that case. Reindex cannot make
a formerly valid unpaid block history satisfy the new rules. Version 1.1.7 schedules
public-testnet registry and service activation at height 3000; mainnet stays disabled.

The experimental regtest runtime now connects journaled prevote/precommit voting,
committee snapshots and saved finality anchors. Its pinned bootstrap must contain
exactly four operators matching the committee captured at that historical block;
later registration changes do not rewrite that checkpoint. Round catch-up requires
the full quorum. A single bounded future proposal can survive delivery before its
round, but cannot advance a round by itself. Public-network, service-fairness,
crash-recovery and cross-platform qualification remain incomplete. Do not use this
opt-in mode with a value-bearing wallet.

The highest verified prevote quorum proof is saved in the existing journal and
rechecked against the trusted committee after restart. Unpublished commit
certificates are recovered from the anchor store and reannounced at most once
every15 seconds so a restarting miner can recover a missed announcement.
Both chainstate rebuild and full reindex preserve stored anchor protection.

Finality persistence now requires a checked storage checkpoint first. Block
validation only stages an in-block certificate; the outer block connection
commits its state, flushes block/undo files and the block index, and syncs both
chainstate and EvoDB before writing the irreversible anchor. Voting-derived
finalization and initial checkpoint seeding use the same prerequisite, before
recording finalized signing history. A file/database flush failure prevents
publication and requests local shutdown; it is not blamed on the peer's block.
Verification-only block replay does not write irreversible anchors.

The `--crash-recovery` functional scenario abruptly kills a carrier node and a
voting node, then checks that ordinary restart retains their finalized blocks.
It also interrupts an actual coin-database batch, verifies that no new anchor
log record was written, and recovers by explicit reindex. The two state
databases are still not a single atomic transaction: an interrupted partial
flush deliberately refuses ordinary startup and requires rebuilding from the
saved block files. No wallet or finality history is automatically erased.
These process-crash checks are not physical power-loss or filesystem/hardware
durability certification.

For a new walletless operator identity, first sync its confirmed registration
without signing credentials. A network already publishing certificates needs
the approved `-pqbootstrap` checkpoint even for passive sync. Then restart with
the new credentials and without `-pqbootstrap`, call `initpqjournal` once, and
restart with the approved checkpoint after initialization succeeds. The same
sequence applies to a fresh key after rotation; catch up before provisioning.
The command refuses existing or
partial state. Normal startup requires both the `.journal` and `.lock` files;
it never creates missing signing history. Preserve both outside chainstate.
Do not reinitialize a used identity after loss, copy it to concurrent operators,
or resume an old backup: rotate the operator key through the controller if the
latest exclusive signing history cannot be proven. Local file locking cannot
detect cloned state on another host. Journal storage supports POSIX and local,
fixed NTFS on Windows; other Windows filesystems and reparse-point directories
are refused. Journal directories and their ancestors must be trusted: file locks
do not defend against a local actor who can replace or restore signing history.
Opened files must be regular, single-link files; temporary files are checked
before truncation. The separate marker remains exclusively held across file
replacement and after a storage failure. Windows replacement closes the old
append handle first, without releasing that marker. Flush/write-through errors
refuse signing; no unsupported-flush success fallback is used.
Recovery also flushes the validated journal before exposing cached signatures,
because complete records can remain only in the OS cache after a failed flush.
The Windows private credential reader, authenticated record loading and
exclusive publication have separate native tests; journal storage support alone
is not full Windows operator qualification. Complete Windows daemon/Qt startup,
protected host custody and the controller-to-operator provisioning workflow
still require native integration testing.
Journal growth is capped at1GiB; reaching the limit stops new signing, not PoS.
New proof records remain in format2; older builds that do not recognize them
fail closed. Never downgrade a used operator's signing state by deleting records.

`getpqoperatorinfo` reports journal availability and actual persisted progress;
zero retained-vote height after compaction does not mean the operator never voted.
`getpqfinalityinfo` reports stored anchor protection separately from bootstrap
validation and reports the actual runtime round, not an inferred signature.
The approved runtime policy is that loss of quorum pauses finality only; ordinary
PoS production must not wait for votes, and existing finalized anchors remain
protected. No finality-based staking gate is introduced here.

Fork choice excludes branches conflicting with a saved anchor even if they have
more work, without marking their blocks intrinsically invalid. Manual invalidation
at or below an active finalized anchor is refused before changing block flags or
disconnecting its unfinalized descendants. The `--conflicting-fork` finality
regression covers these protections, reconsideration, restart and both reindex
modes. A fully received carrier with a verified next-height certificate can also
select a lower-work branch through ordinary full block validation. Invalid
carriers cannot publish anchors, and failed validation restores ordinary fork
choice. The `--certified-fork` regression covers received-carrier adoption,
invalid certificates/carriers and restart. The `--peer-fork` variant reconnects
a higher-work minority that lacks the certified branch, downloads it through
ordinary GETBLOCKS/INV traffic and verifies adoption and restart. Headers-first
sync is disabled; no alternate certificate-request protocol is needed for this
case. A known header without block data can be requested and its body validated;
corrupt contents do not prevent a later valid body with the same header.
Gossip-only recovery before a carrier exists and arbitrary reordered-body
partition recovery are not yet qualified by these tests.

The six-node transition regression exercises four-to-five committee growth,
strict four-of-five finality (three do not suffice), operator key rotation,
new-journal rejoin and revocation. Its `--collateral-spend` variant spends the
fifth operator's actual bond, verifies the five-to-four historical committee
handoff, continued finality with the remaining four, registry removal and restart.
Falling below four configured registrations
leaves no next committee: finality stays at the preceding anchor while ordinary
payments, staking and the single network reward continue. Automatically crossing
that empty historical committee is deliberately prohibited. The separate
administrator-approved recovery path below is not public activation qualification.

### Independent service activity and emergency checkpoints (local qualification)

`-nuparams=pq_service:HEIGHT` activates independent signed heartbeats on regtest
only, in addition to `pq_masternodes`. Both public networks remain unchanged.
The activation height is part of registry-cache identity: changing it requires
an explicit reindex, not silent reuse of another rule set's state.

Walletless operators publish ML-DSA-44 proofs without fee funds, a bootstrap or
a finality journal. Each proof binds genesis, registration, current operator key,
registry sequence, and a recent ancestor's height/hash. A coinbase service
envelope carries up to 16 proofs and optionally the existing finality certificate.
The pending pool holds at most 400 identities; incoming verification attempts
are capped at 16 per second. Existing peer-send passes refresh and relay proofs.

After activation, rewards use these independent proofs rather than certificate
signer lists. Activity remains fresh for the service window (24 blocks on
regtest); registration and activation have the same grace period. Selection uses
parent state, preserves the existing whole-6-OLC rotation, and falls back to the
configured mature queue when the entire network lacks fresh activity. Revocation,
collateral maturity and service-endpoint requirements are never waived.
`listpqmasternodes` exposes `last_heartbeat_height`. Registry sequence changes
require new activity; block disconnect/reindex restores the corresponding state.
These are signed activity proofs, not proof of complete service or protection
against block-producer censorship and network-wide denial of service.

For an actual historical below-four dead end, an administrator may explicitly
configure `-pqemergencycheckpoint=HEIGHT:BLOCKHASH`. This is test-chain-only and
requires an existing durable anchor, a known below-minimum committee at its next
height, and restored chain-derived committees at the chosen checkpoint and its
next height. Both blocks must already be in the local validated chain. Healthy
backlogs, missing snapshots, wrong ancestry and unsigned peer-selected checkpoints
do not authorize recovery. No committee keys are accepted in this setting.

Operators must coordinate the same checkpoint outside the protocol. Startup
flushes history before atomically recording a durable recovery marker; it then
restores the rollbackable mirror. Old anchors, certificates and signing journals
are retained. Restart and either reindex mode replay the saved approval even
after the setting is removed. Repeating the same setting after further finality
is harmless; conflicting or unapproved older settings fail startup. A fresh node
must first validate the historical chain and establish its original trust root;
this is not an automatic catch-up or peer recovery mechanism. Staking and ordinary
payments continue without finality and do not require a checkpoint.

Local functional scenarios:

```sh
python3 test/functional/feature_pq_finality_smoke.py --service-heartbeats
python3 test/functional/feature_pq_finality_smoke.py --service-envelopes
python3 test/functional/feature_pq_finality_smoke.py --emergency-recovery
```

Finality peer-message dispatch retains a shared64-message FIFO, with the existing
drop-oldest overflow policy. Each driver pass consumes at most16 messages and401
declared signature slots (one maximum proof-bearing proposal). Remaining messages
are processed on later passes, releasing the chain lock between batches. All
signature checks remain; malformed proofs also consume their declared budget.
Round decisions still run after each batch. This is an inbox work bound, not a
wall-clock guarantee. With an empty inbox, polling uses the next unfinished
voting step's deadline, capped at500ms; completed steps do not keep polling
expired deadlines every10ms. Passive nodes also use the500ms idle cap.
Current-round received PREVOTEs persist their complete
quorum proof once per touched statement at batch end, before local decisions.
Pending proofs are also persisted before a future-round message can prune their
votes. Local votes and future-vote promotion retain immediate proof persistence;
a failed proof append poisons the journal and prevents further local signing.
Round-decision proof checks, journal syncs and chainstate
flushes remain separately unqualified under maximum load. It does not guarantee
delivery or finality under sustained queue overflow.

For native storage checks, `make -C src check-journal-io` exercises the configured
platform without starting a node. GNU-linker failure/crash checks can also be
built with existing depends (no new dependencies):

```
PQ_DEPENDS=/path/to/depends/x86_64-pc-linux-gnu bash contrib/devtools/test-pqjournal-native.sh
CXX=x86_64-w64-mingw32-g++ PQ_DEPENDS=/path/to/depends/x86_64-w64-mingw32 bash contrib/devtools/test-pqjournal-native.sh
```

The Linux command runs both isolated checks. Run both cross-built `.exe` files
on native Windows with `TEMP` on local NTFS; compilation alone is not validation.
The journal check uses actual framing, hashes and file I/O, deterministic fake
signature bytes and disabled logging, not wallet/cryptographic validation.
Calling the out-of-scope signature verifier aborts the check instead of accepting
fake signatures as valid.
Link-time wrappers inject append/temporary flush failure, partial writes, rename,
post-rename parent-sync and reopen failures, plus abrupt process exits after
durable append and replacement. Each case verifies exclusive access, refusal of
further signing, and retained votes, locks and proofs after reopening. These are
process-crash tests, not physical power-loss or outer chainstate/anchor atomicity
qualification. Failure injection is absent from production executables.

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
for secrets or finality.

### Public-testnet activation in 1.1.7

Registry transactions, the rotating whole-6-OLC network reward and signed service
heartbeats activate together at height 3000. Before that height, existing payments
and staking retain their rules. Regtest remains explicitly opt-in; mainnet remains
disabled. Operator recovery/export and the familiar Qt controller support testnet
keys and reject records from another network or genesis. Credentials alone are
not a registered masternode or permission to vote.

Upgrade every validator before height 3000, including wallet, seed and API/indexer
nodes. Preserve wallets, chain history and any signing journals. Replay existing
history with the candidate before rollout; changing registry cache identity can
require explicit reindex, never deletion of wallets or an automatic chain reset.
The controller holds four separate 4000-OLC bonds; VPS operators run walletless
with private operator-only credentials. Wait for four registrations to mature,
then coordinate one exact chain-validated bootstrap checkpoint. Do not configure
emergency recovery for initial deployment. Finality is optional for chain progress:
staking and payments continue when voting is unavailable.

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
