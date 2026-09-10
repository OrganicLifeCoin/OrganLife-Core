// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <test/test_organiclife.h>
#include <evo/pqmasternode.h>
#include <evo/pqmnauth.h>
#include <pqservice.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/merkle.h>
#include <netbase.h>
#include <validation.h>
#include <txdb.h>
#include <blockassembler.h>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <type_traits>
#include <wallet/pqkey.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#endif
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#include <interfaces/wallet.h>
#include <coincontrol.h>
#endif

namespace {
template<typename Base> struct MNSetupT : Base {
    std::array<mldsa44::Key, 9> keys;
    CCoinsView base;
    CCoinsViewCache view{&base};
    uint32_t nextInput{1};
    const COutPoint collateral{uint256S("aa"), 0};
    MNSetupT() : Base(CBaseChainParams::REGTEST) {
        for (size_t i = 0; i < keys.size(); ++i) {
            std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
            seed[0] = i + 20; // Public deterministic test material only.
            BOOST_REQUIRE(keys[i].SetSeed(seed));
        }
        view.AddCoin(collateral, Coin(CTxOut(Params().GetConsensus().nMNCollateralAmt, pq::GetScript(ID(2))), 1, false, false), false);
    }
    pq::KeyID ID(size_t key) { return *pq::GetID(keys[key].GetPublicKey(), Params().NetworkIDString()); }
    pqmnauth::Transcript AuthTranscript() {
        return {Params().GetConsensus().hashGenesisBlock, uint256S("11"), uint256S("22"), true};
    }
    pqmnauth::Proof AuthProof(const pqmnauth::Transcript& transcript, const uint256& id, size_t signer = 1) {
        pqmnauth::Proof proof; proof.registration = id;
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(keys[signer].Sign(pqmnauth::Message(transcript, id, keys[signer].GetPublicKey()),
                                       pqmnauth::Context(), signature));
        std::copy(signature.begin(), signature.end(), proof.signature.begin());
        return proof;
    }
    pqmn::Payload Registration() {
        pqmn::Payload op;
        op.collateral = collateral;
        op.owner = keys[0].GetPublicKey(); op.operatorKey = keys[1].GetPublicKey();
        op.collateralKey = keys[2].GetPublicKey(); op.payout = ID(0);
        op.operatorReward = 250; op.operatorPayout = ID(1);
        BOOST_REQUIRE(Lookup("127.0.0.1:51476", op.service, 0, false));
        return op;
    }
    void FeeSign(CMutableTransaction& tx, pq::Payload& payload, size_t key = 3) {
        tx.extraPayload = pq::EncodePayload(payload);
        const std::vector<CTxOut> prev{view.AccessCoin(tx.vin[0].prevout).out};
        const auto message = pq::SignatureMessage(tx, prev, payload, Params().GetConsensus().hashGenesisBlock, 0);
        BOOST_REQUIRE(!message.empty());
        std::vector<unsigned char> sig;
        BOOST_REQUIRE(keys[key].Sign(message, *pq::SignatureContext(Params().NetworkIDString()), sig));
        std::copy(sig.begin(), sig.end(), payload.authorizations[0].signature.begin());
        tx.extraPayload = pq::EncodePayload(payload);
    }
    CMutableTransaction Transaction(pqmn::Payload op, size_t operatorKey = 1, bool internal = false,
                                    size_t ownerKey = 0, size_t collateralKey = 2, COutPoint feeInput = {}) {
        CMutableTransaction tx;
        tx.nVersion = 3; tx.nType = CTransaction::PQ; tx.sapData = nullopt;
        tx.vin.emplace_back(feeInput.IsNull() ? COutPoint(uint256S("bb"), nextInput++) : feeInput);
        const auto amount = Params().GetConsensus().nMNCollateralAmt;
        if (feeInput.IsNull()) view.AddCoin(tx.vin[0].prevout, Coin(CTxOut(amount + 10 * COIN, pq::GetScript(ID(3))), 1, false, false), false);
        const CAmount available = view.AccessCoin(tx.vin[0].prevout).out.nValue;
        if (internal) {
            op.collateral = COutPoint(uint256(), 0);
            tx.vout.emplace_back(amount, pq::GetScript(ID(2)));
            tx.vout.emplace_back(available - amount - COIN, pq::GetScript(ID(3)));
        } else tx.vout.emplace_back(available - COIN, pq::GetScript(ID(3)));
        pq::Payload payload; payload.mode = pq::MASTERNODE; payload.authorizations.resize(1);
        payload.authorizations[0].public_key = keys[3].GetPublicKey();
        payload.data = pqmn::Encode(op); BOOST_REQUIRE(!payload.data.empty());
        tx.extraPayload = pq::EncodePayload(payload); BOOST_REQUIRE(!tx.extraPayload->empty());
        const auto message = pqmn::SigningMessage(tx, {view.AccessCoin(tx.vin[0].prevout).out}, Params().GetConsensus().hashGenesisBlock);
        BOOST_REQUIRE(!message.empty());
        const auto sign = [&](size_t key, pqmn::Role role, pqmn::Signature& out) {
            std::vector<unsigned char> sig;
            BOOST_REQUIRE(keys[key].Sign(message, pqmn::Context(role), sig));
            std::copy(sig.begin(), sig.end(), out.begin());
        };
        if (op.action == pqmn::Action::REGISTER || op.action == pqmn::Action::UPDATE) sign(ownerKey, pqmn::Role::OWNER, op.ownerSignature);
        if (op.action != pqmn::Action::UPDATE || operatorKey != 1) sign(operatorKey, pqmn::Role::OPERATOR, op.operatorSignature);
        if (op.action == pqmn::Action::REGISTER) sign(collateralKey, pqmn::Role::COLLATERAL, op.collateralSignature);
        payload.data = pqmn::Encode(op); FeeSign(tx, payload);
        return tx;
    }
};
using MNSetup = MNSetupT<BasicTestingSetup>;

struct RegistryBlock {
    CBlock block;
    uint256 hash;
    CBlockIndex index;
    RegistryBlock(const CBlockIndex& parent, const std::vector<CTransactionRef>& transactions) {
        block.hashPrevBlock = parent.GetBlockHash(); block.nTime = parent.nTime + 1;
        CMutableTransaction coinbase; coinbase.vin.resize(1); coinbase.vout.emplace_back(0, pq::GetScript(pq::KeyID{}));
        coinbase.vin[0].scriptSig = CScript() << (parent.nHeight + 1);
        block.vtx.push_back(MakeTransactionRef(coinbase));
        block.vtx.insert(block.vtx.end(), transactions.begin(), transactions.end());
        block.hashMerkleRoot = BlockMerkleRoot(block); hash = block.GetHash();
        index = CBlockIndex(block); index.phashBlock = &hash;
        index.pprev = const_cast<CBlockIndex*>(&parent); index.nHeight = parent.nHeight + 1;
    }
};
}

BOOST_FIXTURE_TEST_SUITE(pqmasternode_tests, MNSetup)

#if defined(__linux__) || defined(__APPLE__)
BOOST_AUTO_TEST_CASE(local_operator_loads_pending_identity_without_registry_authority)
{
    static_assert(!std::is_copy_constructible<pqmnauth::LocalOperator>::value, "Operator secrets must not be copied");
    static_assert(!std::is_copy_assignable<pqmnauth::LocalOperator>::value, "Operator secrets must not be assigned");
    const auto directory = SetDataDir("pq-operator");
    BOOST_REQUIRE_EQUAL(chmod(directory.c_str(), 0700), 0);
    pqwallet::SecureBytes seed(32, 0), wrapping(32, 42); seed[0] = 21;
    pqwallet::Record record; std::string reason;
    BOOST_REQUIRE(pqwallet::EncryptOperatorSeed(seed, wrapping, "regtest", Params().GetConsensus().hashGenesisBlock, record));
    CDataStream bytes(SER_DISK, 0); bytes << record;
    const auto write = [&](const char* name, const char* data, size_t size) {
        const auto path = directory / name;
        fsbridge::ofstream file(path, std::ios::binary);
        file.write(data, size); file.close(); BOOST_REQUIRE(file.good());
        BOOST_REQUIRE_EQUAL(chmod(path.c_str(), 0400), 0);
    };
    write("olc-pq-operator-record", bytes.data(), bytes.size());
    write("olc-pq-operator-key", reinterpret_cast<const char*>(wrapping.data()), wrapping.size());
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
    const auto id = uint256S("1234");
    auto loaded = pqmnauth::LocalOperator::Load(directory, Params(), id, reason);
    BOOST_REQUIRE_MESSAGE(loaded != nullptr, reason);
    BOOST_CHECK(loaded->Registration() == id);
    BOOST_CHECK(loaded->PublicKey() == keys[1].GetPublicKey());
    BOOST_CHECK(reason.empty());
    BOOST_CHECK(!pqmnauth::LocalOperator::Load(directory, Params(), uint256(), reason));
    BOOST_CHECK_EQUAL(reason, "PQ operator registration must be nonzero");
    for (const auto& network : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET}) {
        const auto params = CreateChainParams(network);
        BOOST_CHECK(!pqmnauth::LocalOperator::Load(directory, *params, id, reason));
        BOOST_CHECK_EQUAL(reason, "Could not load private PQ operator credentials");
    }
    for (const int height : {0, -1}) {
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, height);
        BOOST_CHECK(!pqmnauth::LocalOperator::Load(directory, Params(), id, reason));
        BOOST_CHECK_EQUAL(reason, "PQ operator credentials require scheduled PQ masternode activation");
    }
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
    BOOST_CHECK(!pqmnauth::LocalOperator::Load(directory / "missing", Params(), id, reason));
    BOOST_CHECK_EQUAL(reason, "Could not load private PQ operator credentials");
    // A failed independent load does not alter the existing immutable identity.
    BOOST_CHECK(loaded->Registration() == id);
    BOOST_CHECK(loaded->PublicKey() == keys[1].GetPublicKey());
}
#endif

BOOST_AUTO_TEST_CASE(operator_authentication_roundtrip_and_budget)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration()));
    BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    pqmnauth::Transcript transcript{Params().GetConsensus().hashGenesisBlock, uint256S("11"), uint256S("22"), true};
    const auto message = pqmnauth::Message(transcript, reg.GetHash(), keys[1].GetPublicKey());
    BOOST_CHECK_EQUAL(message.size(), 1 + 4 * 32 + mldsa44::PUBLIC_KEY_SIZE + 1);
    BOOST_CHECK(pqmnauth::Context().size() != 0);
    pqmnauth::Proof proof; proof.registration = reg.GetHash();
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(keys[1].Sign(message, pqmnauth::Context(), signature));
    std::copy(signature.begin(), signature.end(), proof.signature.begin());
    const auto bytes = pqmnauth::Encode(proof);
    BOOST_CHECK_EQUAL(bytes.size(), pqmnauth::PROOF_SIZE);
    pqmnauth::Proof decoded;
    BOOST_CHECK(pqmnauth::Decode(bytes, decoded));
    BOOST_CHECK(decoded.registration == reg.GetHash());
    BOOST_CHECK(decoded.signature == proof.signature);
    pqmnauth::Session session(transcript);
    BOOST_CHECK(session.Current(index, 34, 15, reason).IsNull());
    BOOST_CHECK(session.Authenticate(bytes, index, 34, 15, reason));
    BOOST_CHECK(reason.empty());
    BOOST_CHECK(session.Current(index, 34, 15, reason) == reg.GetHash());
    BOOST_CHECK(!session.Authenticate(bytes, index, 34, 15, reason));
    BOOST_CHECK(session.Current(index, 34, 15, reason).IsNull());
    pqmn::Record record;
    BOOST_REQUIRE(index.Get(reg.GetHash(), record));
    BOOST_CHECK_EQUAL(record.sequence, 0U);
    BOOST_REQUIRE(index.Undo(reg.GetHash(), reason)); // Authentication created no undo/state.
    BOOST_CHECK(index.List().empty());
}

BOOST_AUTO_TEST_CASE(operator_authentication_canonical_wire_and_transcript)
{
    auto transcript = AuthTranscript(); transcript.genesis = uint256S("33");
    const auto id = uint256S("44");
    const auto proof = AuthProof(transcript, id);
    const auto bytes = pqmnauth::Encode(proof);
    BOOST_REQUIRE_EQUAL(bytes.size(), pqmnauth::PROOF_SIZE);
    std::vector<unsigned char> expected(pqmnauth::PROOF_SIZE, 0);
    expected[0] = 1; expected[1] = 0x44;
    std::copy(proof.signature.begin(), proof.signature.end(), expected.begin() + 33);
    BOOST_CHECK(bytes == expected);
    expected.assign(1 + 4 * 32 + mldsa44::PUBLIC_KEY_SIZE + 1, 0);
    expected[0] = 1; expected[1] = 0x33; expected[33] = 0x44;
    std::copy(keys[1].GetPublicKey().begin(), keys[1].GetPublicKey().end(), expected.begin() + 65);
    expected[65 + mldsa44::PUBLIC_KEY_SIZE] = 0x11;
    expected[97 + mldsa44::PUBLIC_KEY_SIZE] = 0x22;
    expected.back() = 1;
    BOOST_CHECK(pqmnauth::Message(transcript, id, keys[1].GetPublicKey()) == expected);
    for (size_t size = 0; size < bytes.size(); ++size) {
        auto decoded = proof;
        BOOST_CHECK(!pqmnauth::Decode({bytes.data(), size}, decoded));
        BOOST_CHECK(decoded.registration.IsNull());
        BOOST_CHECK(decoded.signature == pqmn::Signature{});
    }
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto bad = bytes;
        if (mutation == 0) bad.push_back(0);
        if (mutation == 1) bad[0] = 2;
        if (mutation == 2) std::fill(bad.begin() + 1, bad.begin() + 33, 0);
        if (mutation == 3) bad.resize(100000, 0);
        auto decoded = proof;
        BOOST_CHECK(!pqmnauth::Decode(bad, decoded));
        BOOST_CHECK(decoded.registration.IsNull());
        BOOST_CHECK(decoded.signature == pqmn::Signature{});
    }
    auto decoded = proof;
    BOOST_CHECK(!pqmnauth::Decode({static_cast<const unsigned char*>(nullptr), pqmnauth::PROOF_SIZE}, decoded));
    BOOST_CHECK(decoded.registration.IsNull());
    BOOST_CHECK(pqmnauth::Encode(pqmnauth::Proof{}).empty());
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto bad = transcript; auto key = keys[1].GetPublicKey(); auto registration = id;
        if (mutation == 0) bad.genesis.SetNull();
        if (mutation == 1) bad.initiatorChallenge.SetNull();
        if (mutation == 2) bad.responderChallenge.SetNull();
        if (mutation == 3) bad.responderChallenge = bad.initiatorChallenge;
        if (mutation == 4) registration.SetNull();
        if (mutation == 5) key.fill(0);
        BOOST_CHECK(pqmnauth::Message(bad, registration, key).empty());
    }
}

BOOST_AUTO_TEST_CASE(operator_authentication_rejects_replay_reflection_and_role_substitution)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    const auto transcript = AuthTranscript();
    const auto proof = AuthProof(transcript, reg.GetHash());
    for (int mutation = 0; mutation < 9; ++mutation) {
        auto other = transcript; auto bad = proof;
        if (mutation == 0) other.genesis = uint256S("88");
        if (mutation == 1) other.initiatorChallenge = uint256S("88");
        if (mutation == 2) other.responderChallenge = uint256S("88");
        if (mutation == 3) other.signerIsInitiator = false;
        if (mutation == 4) std::swap(other.initiatorChallenge, other.responderChallenge);
        if (mutation == 5) bad.registration = uint256S("99");
        if (mutation == 6) bad.signature[0] ^= 1;
        if (mutation == 7) bad = AuthProof(transcript, reg.GetHash(), 0); // Owner is not operator.
        if (mutation == 8) bad = AuthProof(transcript, reg.GetHash(), 2); // Nor is collateral key.
        pqmnauth::Session session(other);
        BOOST_CHECK(!session.Authenticate(pqmnauth::Encode(bad), index, 34, 15, reason));
        BOOST_CHECK(!reason.empty());
        BOOST_CHECK(session.Current(index, 34, 15, reason).IsNull());
        BOOST_CHECK(!session.Authenticate(pqmnauth::Encode(proof), index, 34, 15, reason));
    }
    for (auto role : {pqmn::Role::OWNER, pqmn::Role::OPERATOR, pqmn::Role::COLLATERAL}) {
        auto bad = proof; std::vector<unsigned char> signature;
        BOOST_REQUIRE(keys[1].Sign(pqmnauth::Message(transcript, reg.GetHash(), keys[1].GetPublicKey()), pqmn::Context(role), signature));
        std::copy(signature.begin(), signature.end(), bad.signature.begin());
        pqmnauth::Session session(transcript);
        BOOST_CHECK(!session.Authenticate(pqmnauth::Encode(bad), index, 34, 15, reason));
    }
    // The reverse direction is allowed, but requires its own signature.
    auto reverse = transcript; reverse.signerIsInitiator = false;
    pqmnauth::Session session(reverse);
    BOOST_CHECK(session.Authenticate(pqmnauth::Encode(AuthProof(reverse, reg.GetHash())), index, 34, 15, reason));
}

BOOST_AUTO_TEST_CASE(operator_authentication_tracks_registry_lifecycle)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    const auto transcript = AuthTranscript(); const auto bytes = pqmnauth::Encode(AuthProof(transcript, reg.GetHash()));
    for (const auto policy : {std::make_pair(33U, 15U), std::make_pair(19U, 1U), std::make_pair(34U, 0U)}) {
        pqmnauth::Session session(transcript);
        BOOST_CHECK(!session.Authenticate(bytes, index, policy.first, policy.second, reason));
        BOOST_CHECK(session.Current(index, 34, 15, reason).IsNull());
    }
    pqmnauth::Session session(transcript);
    BOOST_REQUIRE(session.Authenticate(bytes, index, 34, 15, reason));
    pqmn::Payload op; op.action = pqmn::Action::UPDATE; op.registration = reg.GetHash(); op.sequence = 1;
    op.operatorKey = keys[1].GetPublicKey(); op.payout = ID(5);
    const CTransaction payout(Transaction(op)); BOOST_REQUIRE(index.Apply(payout, view, 35, reason));
    BOOST_CHECK(session.Current(index, 35, 15, reason) == reg.GetHash());
    op.sequence = 2; op.operatorKey = keys[4].GetPublicKey();
    const CTransaction rotation(Transaction(op, 4)); BOOST_REQUIRE(index.Apply(rotation, view, 36, reason));
    BOOST_CHECK(session.Current(index, 36, 15, reason).IsNull());
    pqmnauth::Session retired(transcript);
    BOOST_CHECK(!retired.Authenticate(bytes, index, 36, 15, reason));
    pqmnauth::Session rotated(transcript);
    BOOST_REQUIRE(rotated.Authenticate(pqmnauth::Encode(AuthProof(transcript, reg.GetHash(), 4)), index, 36, 15, reason));
    op = {}; op.action = pqmn::Action::REVOKE; op.registration = reg.GetHash(); op.sequence = 3;
    const CTransaction revoke(Transaction(op, 4)); BOOST_REQUIRE(index.Apply(revoke, view, 37, reason));
    BOOST_CHECK(rotated.Current(index, 37, 15, reason).IsNull());
    pqmnauth::Session revoked(transcript);
    BOOST_CHECK(!revoked.Authenticate(pqmnauth::Encode(AuthProof(transcript, reg.GetHash(), 4)), index, 37, 15, reason));
    BOOST_REQUIRE(index.Undo(revoke.GetHash(), reason)); BOOST_REQUIRE(index.Undo(rotation.GetHash(), reason));
    BOOST_CHECK(session.Current(index, 35, 15, reason).IsNull()); // Undo must not revive a cleared session.
    pqmnauth::Session restored(transcript); BOOST_REQUIRE(restored.Authenticate(bytes, index, 35, 15, reason));
    BOOST_CHECK(restored.Current(index, 33, 15, reason).IsNull()); // Reorg below maturity.
    BOOST_CHECK(restored.Current(index, 35, 15, reason).IsNull());
    pqmnauth::Session spent(transcript); BOOST_REQUIRE(spent.Authenticate(bytes, index, 35, 15, reason));
    CMutableTransaction spend; spend.nVersion = 3; spend.nType = CTransaction::PQ; spend.sapData = nullopt;
    spend.vin.emplace_back(collateral); spend.vout.emplace_back(Params().GetConsensus().nMNCollateralAmt - COIN, pq::GetScript(ID(0)));
    pq::Payload transfer; transfer.authorizations.resize(1); transfer.authorizations[0].public_key = keys[2].GetPublicKey();
    FeeSign(spend, transfer, 2);
    BOOST_REQUIRE(index.Apply(spend, view, 36, reason));
    BOOST_CHECK(spent.Current(index, 36, 15, reason).IsNull());
    BOOST_REQUIRE(index.Undo(spend.GetHash(), reason));
    BOOST_CHECK(spent.Current(index, 35, 15, reason).IsNull());
    BOOST_REQUIRE(index.Undo(payout.GetHash(), reason));
    BOOST_REQUIRE(index.Undo(reg.GetHash(), reason));
    pqmnauth::Session missing(transcript);
    BOOST_CHECK(!missing.Authenticate(bytes, index, 34, 15, reason));
}

BOOST_AUTO_TEST_CASE(operator_authentication_malformed_attempt_exhausts_budget)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    const auto transcript = AuthTranscript(); const auto bytes = pqmnauth::Encode(AuthProof(transcript, reg.GetHash()));
    pqmnauth::Session malformed(transcript);
    BOOST_CHECK(!malformed.Authenticate({static_cast<const unsigned char*>(nullptr), pqmnauth::PROOF_SIZE}, index, 34, 15, reason));
    BOOST_CHECK(!reason.empty());
    BOOST_CHECK(!malformed.Authenticate(bytes, index, 34, 15, reason));
    BOOST_CHECK(malformed.Current(index, 34, 15, reason).IsNull());
    // A new connection with fresh local challenges requires a new proof.
    auto fresh = transcript; fresh.initiatorChallenge = uint256S("33");
    pqmnauth::Session reconnect(fresh);
    BOOST_CHECK(!reconnect.Authenticate(bytes, index, 34, 15, reason));
    pqmnauth::Session valid(fresh);
    BOOST_CHECK(valid.Authenticate(pqmnauth::Encode(AuthProof(fresh, reg.GetHash())), index, 34, 15, reason));
}

BOOST_AUTO_TEST_CASE(operator_authentication_corrupt_storage_fails_closed)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    pqmn::Record record; BOOST_REQUIRE(index.Get(reg.GetHash(), record));
    const auto transcript = AuthTranscript(); const auto bytes = pqmnauth::Encode(AuthProof(transcript, reg.GetHash()));
    // Real serialized storage, not a wrong C++ type in the typed transaction cache.
    CEvoDB database(1 << 20, true, true); pqmn::Index isolated(database, Params());
    const auto key = std::make_pair(std::make_pair(std::string("pqmn1r"), transcript.genesis), reg.GetHash());
    BOOST_REQUIRE(database.GetRawDB().Write(key, record));
    pqmnauth::Session current(transcript);
    BOOST_REQUIRE(current.Authenticate(bytes, isolated, 34, 15, reason));
    BOOST_REQUIRE(database.GetRawDB().Write(key, std::string("broken")));
    pqmnauth::Session corrupt(transcript);
    bool accepted = true;
    BOOST_CHECK_NO_THROW(accepted = corrupt.Authenticate(bytes, isolated, 34, 15, reason));
    BOOST_CHECK(!accepted); BOOST_CHECK_EQUAL(reason, "pq-auth-registry-unavailable");
    uint256 identity = reg.GetHash();
    BOOST_CHECK_NO_THROW(identity = current.Current(isolated, 34, 15, reason));
    BOOST_CHECK(identity.IsNull()); BOOST_CHECK_EQUAL(reason, "pq-auth-registry-unavailable");
    BOOST_REQUIRE(database.GetRawDB().Write(key, record));
    BOOST_CHECK(!corrupt.Authenticate(bytes, isolated, 34, 15, reason));
    BOOST_CHECK(current.Current(isolated, 34, 15, reason).IsNull());
}

BOOST_AUTO_TEST_CASE(operator_authentication_requires_registry_network)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    const auto transcript = AuthTranscript();
    auto wrongNetwork = transcript; wrongNetwork.genesis = uint256S("99");
    pqmnauth::Session wrong(wrongNetwork);
    // A valid signature on a wrong-network transcript must not authorize this registry.
    BOOST_CHECK(!wrong.Authenticate(pqmnauth::Encode(AuthProof(wrongNetwork, reg.GetHash())), index, 34, 15, reason));
    BOOST_CHECK_EQUAL(reason, "pq-auth-wrong-network");
    pqmnauth::Session current(transcript);
    BOOST_REQUIRE(current.Authenticate(pqmnauth::Encode(AuthProof(transcript, reg.GetHash())), index, 34, 15, reason));
    // Even a copied record under another chain namespace cannot preserve identity.
    auto otherParams = CreateChainParams(CBaseChainParams::TESTNET);
    pqmn::Index other(*evoDb, *otherParams);
    pqmn::Record record; BOOST_REQUIRE(index.Get(reg.GetHash(), record));
    const auto key = std::make_pair(std::make_pair(std::string("pqmn1r"), otherParams->GetConsensus().hashGenesisBlock), reg.GetHash());
    evoDb->Write(key, record);
    BOOST_CHECK(current.Current(other, 34, 15, reason).IsNull());
    BOOST_CHECK_EQUAL(reason, "pq-auth-wrong-network");
    BOOST_CHECK(current.Current(index, 34, 15, reason).IsNull());
}

BOOST_AUTO_TEST_CASE(read_only_validation_preserves_outer_transaction)
{
    LOCK(cs_main);
    pqmn::Index index(*evoDb, Params()); std::string reason;
    auto outer = evoDb->BeginTransaction();
    const CTransaction reg(Transaction(Registration()));
    pqmn::Record preview;
    BOOST_REQUIRE(index.Check(reg, view, 20, preview, reason));
    BOOST_CHECK(preview.collateral == collateral);
    BOOST_CHECK(index.List().empty());
    BOOST_CHECK(!index.Undo(reg.GetHash(), reason));
    BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    pqmn::Payload update; update.action = pqmn::Action::UPDATE;
    update.registration = reg.GetHash(); update.sequence = 1;
    update.operatorKey = keys[1].GetPublicKey(); update.payout = ID(4);
    const CTransaction tx(Transaction(update));
    BOOST_REQUIRE(index.Check(tx, view, 21, preview, reason));
    BOOST_CHECK_EQUAL(preview.sequence, 1U);
    pqmn::Record actual;
    BOOST_REQUIRE(index.Get(reg.GetHash(), actual));
    BOOST_CHECK_EQUAL(actual.sequence, 0U);
    BOOST_CHECK(!index.Undo(tx.GetHash(), reason));
    update.sequence = 2;
    BOOST_CHECK(!index.Check(Transaction(update), view, 21, preview, reason));
    BOOST_CHECK(SerializeHash(preview) == SerializeHash(pqmn::Record{}));
    outer->Commit();
    BOOST_REQUIRE(index.Get(reg.GetHash(), actual));
    BOOST_CHECK_EQUAL(actual.sequence, 0U);
}

BOOST_AUTO_TEST_CASE(reserved_envelope_parses_but_is_disabled_before_activation)
{
    pq::Payload payload; payload.mode = pq::MASTERNODE; payload.authorizations.resize(1); payload.data = {1};
    BOOST_CHECK(!pq::EncodePayload(payload).empty());
    auto tx = Transaction(Registration());
    std::string reason;
    BOOST_CHECK(pq::CheckStructure(tx, Params(), reason));
    for (const std::string network : {"regtest"}) {
        const auto params = CreateChainParams(network);
        for (int height : {0, 1, 100, 2999})
            BOOST_CHECK(!pq::CheckContext(tx, *params, height, reason));
    }
    BOOST_CHECK_EQUAL(pq::GetSigOpCost(tx), 4 * pq::SIGOP_COST);
}

BOOST_AUTO_TEST_CASE(registry_context_requires_pq_activation_after_payments)
{
    const auto tx = Transaction(Registration()); std::string reason;
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 20);
    BOOST_CHECK(!pq::CheckContext(tx, Params(), 19, reason));
    BOOST_CHECK(pq::CheckContext(tx, Params(), 20, reason));
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 21);
    BOOST_CHECK(!pq::CheckContext(tx, Params(), 21, reason)); // Invalid activation ordering.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    BOOST_CHECK(pq::CheckContext(tx, Params(), 20, reason));
    for (int invalid : {-1, 0}) {
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, invalid);
        BOOST_CHECK(!pq::CheckContext(tx, Params(), 20, reason));
    }
    for (const std::string network : {"main"}) {
        auto params = CreateChainParams(network);
        params->UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 20);
        BOOST_CHECK(pq::MasternodesActive(*params, 20));
        BOOST_CHECK(pq::CheckContext(tx, *params, 20, reason));
    }
}

BOOST_AUTO_TEST_CASE(public_testnet_and_mainnet_activate_registry_and_service)
{
    const auto testnet = CreateChainParams("test");
    const auto tx = Transaction(Registration());
    std::string reason;
    BOOST_CHECK_EQUAL(testnet->GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_MASTERNODES].nActivationHeight, 3000);
    BOOST_CHECK_EQUAL(testnet->GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_SERVICE].nActivationHeight, 3000);
    for (int height : {-1, 0, 1, 2919, 2999}) {
        BOOST_CHECK(!pq::MasternodesActive(*testnet, height));
        BOOST_CHECK(!pqservice::Active(*testnet, height));
        BOOST_CHECK(!pq::CheckContext(tx, *testnet, height, reason));
    }
    for (int height : {3000, 3001, 1000000}) {
        BOOST_CHECK(pq::MasternodesActive(*testnet, height));
        BOOST_CHECK(pqservice::Active(*testnet, height));
        BOOST_CHECK(pq::CheckContext(tx, *testnet, height, reason));
    }
    auto mainnet = CreateChainParams("main");
    mainnet->UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 1);
    mainnet->UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
    mainnet->UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_SERVICE, 1);
    BOOST_CHECK(pq::MasternodesActive(*mainnet, 3000));
    BOOST_CHECK(pqservice::Active(*mainnet, 3000));
    BOOST_CHECK(pq::CheckContext(tx, *mainnet, 3000, reason));
}

BOOST_AUTO_TEST_CASE(register_external_internal_collateral_and_undo)
{
    LOCK(cs_main);
    pqmn::Index index(*evoDb, Params()); std::string reason;
    for (bool internal : {false, true}) {
        auto transaction = evoDb->BeginTransaction();
        const CTransaction tx(Transaction(Registration(), 1, internal));
        BOOST_REQUIRE(index.Apply(tx, view, 20, reason));
        pqmn::Record record; BOOST_REQUIRE(index.Get(tx.GetHash(), record));
        const COutPoint expected = internal ? COutPoint(tx.GetHash(), 0) : collateral;
        BOOST_CHECK(record.collateral == expected);
        BOOST_CHECK(record.owner == keys[0].GetPublicKey());
        BOOST_CHECK(record.operatorKey == keys[1].GetPublicKey());
        BOOST_CHECK(!record.MatureAt(33, 15)); BOOST_CHECK(record.MatureAt(34, 15));
        BOOST_CHECK(!record.MatureAt(19, 1)); BOOST_CHECK(!record.MatureAt(20, 0));
        uint256 found; BOOST_REQUIRE(index.FindCollateral(expected, found));
        BOOST_CHECK(found == tx.GetHash()); BOOST_CHECK_EQUAL(index.List().size(), 1U);
        BOOST_REQUIRE(index.Undo(tx.GetHash(), reason));
        BOOST_CHECK(index.List().empty()); BOOST_CHECK(!index.FindCollateral(expected, found));
        BOOST_CHECK(!index.Undo(tx.GetHash(), reason));
        transaction->Rollback();
    }
}

BOOST_AUTO_TEST_CASE(owner_updates_operator_service_revocation_and_sequence)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    pqmn::Payload op; op.action = pqmn::Action::UPDATE; op.registration = reg.GetHash(); op.sequence = 1;
    op.operatorKey = keys[4].GetPublicKey(); op.payout = ID(5);
    const CTransaction update(Transaction(op, 4)); BOOST_REQUIRE(index.Apply(update, view, 21, reason));
    pqmn::Record record; BOOST_REQUIRE(index.Get(reg.GetHash(), record));
    BOOST_CHECK(record.service == CService()); BOOST_CHECK(record.operatorPayout == pq::KeyID{});
    BOOST_CHECK(record.payout == ID(5)); BOOST_CHECK(record.operatorKey == keys[4].GetPublicKey());
    BOOST_CHECK(!index.Apply(Transaction(op, 4), view, 21, reason)); // Stale sequence.
    BOOST_CHECK(!index.Undo(reg.GetHash(), reason)); // Out-of-order undo.
    op = {}; op.action = pqmn::Action::SERVICE; op.registration = reg.GetHash(); op.sequence = 2;
    BOOST_REQUIRE(Lookup("127.0.0.1:51477", op.service, 0, false)); op.operatorPayout = ID(4);
    BOOST_CHECK(!index.Apply(Transaction(op, 1), view, 22, reason)); // Retired key.
    const CTransaction service(Transaction(op, 4)); BOOST_REQUIRE(index.Apply(service, view, 22, reason));
    op = {}; op.action = pqmn::Action::REVOKE; op.registration = reg.GetHash(); op.sequence = 3;
    const CTransaction revoke(Transaction(op, 4)); BOOST_REQUIRE(index.Apply(revoke, view, 23, reason));
    BOOST_REQUIRE(index.Get(reg.GetHash(), record)); BOOST_CHECK(record.revoked); BOOST_CHECK(record.service == CService());
    BOOST_REQUIRE(index.Undo(revoke.GetHash(), reason));
    BOOST_REQUIRE(index.Undo(service.GetHash(), reason));
    BOOST_REQUIRE(index.Undo(update.GetHash(), reason));
    BOOST_REQUIRE(index.Get(reg.GetHash(), record)); BOOST_CHECK_EQUAL(record.sequence, 0U);
    BOOST_CHECK(record.operatorKey == keys[1].GetPublicKey());
    BOOST_REQUIRE(index.Undo(reg.GetHash(), reason)); BOOST_CHECK(index.List().empty());
}

BOOST_AUTO_TEST_CASE(malformed_data_rejected_and_output_cleared)
{
    const auto good = pqmn::Encode(Registration()); BOOST_REQUIRE(!good.empty());
    pqmn::Payload decoded;
    BOOST_REQUIRE(pqmn::Decode(good, decoded)); BOOST_CHECK(pqmn::Encode(decoded) == good);
    for (size_t size : {size_t(0), size_t(1), good.size()-1}) {
        decoded = Registration();
        BOOST_CHECK(!pqmn::Decode({good.data(), size}, decoded));
        BOOST_CHECK(decoded.owner == mldsa44::PublicKey{});
    }
    auto bad = good; bad.push_back(0); BOOST_CHECK(!pqmn::Decode(bad, decoded));
    bad = good; bad[0] = 2; BOOST_CHECK(!pqmn::Decode(bad, decoded));
    bad = good; bad[1] = 255; BOOST_CHECK(!pqmn::Decode(bad, decoded));
    bad.assign(pq::MAX_MASTERNODE_DATA_SIZE + 1, 0); BOOST_CHECK(!pqmn::Decode(bad, decoded));
}

BOOST_AUTO_TEST_CASE(role_fee_and_transaction_mutations_leave_no_state)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    for (int mutation = 0; mutation < 9; ++mutation) {
        auto tx = Transaction(Registration()); pq::Payload payload;
        BOOST_REQUIRE(pq::DecodePayload(tx, payload)); pqmn::Payload op;
        BOOST_REQUIRE(pqmn::Decode(payload.data, op));
        if (mutation == 0) op.ownerSignature[0] ^= 1;
        if (mutation == 1) op.operatorSignature[0] ^= 1;
        if (mutation == 2) op.collateralSignature[0] ^= 1;
        if (mutation == 3) op.payout[0] ^= 1;
        if (mutation == 4) ++op.operatorReward;
        if (mutation == 5) ++tx.nLockTime;
        if (mutation == 6) ++tx.vin[0].nSequence;
        if (mutation == 7) --tx.vout[0].nValue;
        payload.data = pqmn::Encode(op); FeeSign(tx, payload);
        if (mutation == 8) {
            payload.authorizations[0].signature[0] ^= 1;
            tx.extraPayload = pq::EncodePayload(payload);
        }
        BOOST_CHECK(!index.Apply(tx, view, 20, reason)); BOOST_CHECK(!reason.empty());
        BOOST_CHECK(index.List().empty());
    }
}

BOOST_AUTO_TEST_CASE(unique_keys_collateral_and_endpoints)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction first(Transaction(Registration())); BOOST_REQUIRE(index.Apply(first, view, 20, reason));
    BOOST_CHECK(!index.Apply(Transaction(Registration()), view, 20, reason));
    for (int collision = 0; collision < 4; ++collision) {
        auto op = Registration();
        const size_t owner = collision == 0 ? 0 : 5;
        const size_t oper = collision == 1 ? 1 : 6;
        const size_t col = collision == 2 ? 2 : 7;
        op.owner = keys[owner].GetPublicKey(); op.operatorKey = keys[oper].GetPublicKey(); op.collateralKey = keys[col].GetPublicKey();
        op.collateral = COutPoint(uint256S("cc"), collision);
        view.AddCoin(op.collateral, Coin(CTxOut(Params().GetConsensus().nMNCollateralAmt, pq::GetScript(ID(col))), 1, false, false), false);
        if (collision != 3) BOOST_REQUIRE(Lookup("127.0.0.1:51478", op.service, 0, false));
        BOOST_CHECK(!index.Apply(Transaction(op, oper, false, owner, col), view, 20, reason));
        BOOST_CHECK_EQUAL(index.List().size(), 1U);
    }
}

BOOST_AUTO_TEST_CASE(collateral_spend_removal_owner_protection_and_rollback)
{
    LOCK(cs_main);
    pqmn::Index index(*evoDb, Params()); std::string reason;
    {
        auto transaction = evoDb->BeginTransaction();
        const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
        CMutableTransaction spend; spend.nVersion = 3; spend.nType = CTransaction::PQ; spend.sapData = nullopt;
        spend.vin.emplace_back(collateral); spend.vout.emplace_back(Params().GetConsensus().nMNCollateralAmt - COIN, pq::GetScript(ID(0)));
        pq::Payload payload; payload.authorizations.resize(1);
        payload.authorizations[0].public_key = keys[1].GetPublicKey(); FeeSign(spend, payload, 1);
        BOOST_CHECK(!index.Apply(spend, view, 21, reason)); // An operator cannot spend collateral.
        payload.authorizations[0].public_key = keys[2].GetPublicKey(); FeeSign(spend, payload, 2);
        const CTransaction valid(spend); BOOST_REQUIRE(index.Apply(valid, view, 21, reason));
        BOOST_CHECK(index.List().empty());
        BOOST_REQUIRE(index.Undo(valid.GetHash(), reason)); BOOST_CHECK_EQUAL(index.List().size(), 1U);
        transaction->Rollback();
    }
    BOOST_CHECK(index.List().empty());
}

BOOST_AUTO_TEST_CASE(disk_reopen_preserves_registry_and_undo)
{
    LOCK(cs_main); SetDataDir("pqmn-registry");
    const CTransaction reg(Transaction(Registration())); std::string reason;
    {
        CEvoDB database(1 << 20, false, true); pqmn::Index index(database, Params());
        auto transaction = database.BeginTransaction(); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
        transaction->Commit(); BOOST_REQUIRE(database.CommitRootTransaction());
    }
    {
        CEvoDB database(1 << 20, false, false); pqmn::Index index(database, Params());
        pqmn::Record record; BOOST_REQUIRE(index.Get(reg.GetHash(), record));
        BOOST_CHECK_EQUAL(index.List().size(), 1U);
        auto transaction = database.BeginTransaction(); BOOST_REQUIRE(index.Undo(reg.GetHash(), reason));
        transaction->Commit(); BOOST_REQUIRE(database.CommitRootTransaction());
    }
    {
        CEvoDB database(1 << 20, false, false); pqmn::Index index(database, Params());
        BOOST_CHECK(index.List().empty()); BOOST_CHECK(!index.Undo(reg.GetHash(), reason));
    }
}

BOOST_AUTO_TEST_CASE(undo_cannot_take_properties_reused_by_a_later_registration)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction first(Transaction(Registration())); BOOST_REQUIRE(index.Apply(first, view, 20, reason));
    CMutableTransaction spend; spend.nVersion = 3; spend.nType = CTransaction::PQ; spend.sapData = nullopt;
    spend.vin.emplace_back(collateral); spend.vout.emplace_back(Params().GetConsensus().nMNCollateralAmt - COIN, pq::GetScript(ID(0)));
    pq::Payload payload; payload.authorizations.resize(1); payload.authorizations[0].public_key = keys[2].GetPublicKey();
    FeeSign(spend, payload, 2); const CTransaction spent(spend);
    BOOST_REQUIRE(index.Apply(spent, view, 21, reason));
    const CTransaction second(Transaction(Registration(), 1, true)); BOOST_REQUIRE(index.Apply(second, view, 22, reason));
    BOOST_CHECK(!index.Undo(spent.GetHash(), reason));
    const auto records = index.List(); BOOST_REQUIRE_EQUAL(records.size(), 1U); BOOST_CHECK(records[0].first == second.GetHash());
    BOOST_REQUIRE(index.Undo(second.GetHash(), reason));
    BOOST_REQUIRE(index.Undo(spent.GetHash(), reason));
    BOOST_REQUIRE(index.Undo(first.GetHash(), reason));
    BOOST_CHECK(index.List().empty());
}

BOOST_AUTO_TEST_CASE(fee_signatures_bind_role_proofs_and_roles_bind_network_context_and_prevouts)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto tx = Transaction(Registration()); pq::Payload payload;
        BOOST_REQUIRE(pq::DecodePayload(tx, payload)); pqmn::Payload op; BOOST_REQUIRE(pqmn::Decode(payload.data, op));
        auto prevouts = std::vector<CTxOut>{view.AccessCoin(tx.vin[0].prevout).out};
        if (mutation == 0) {
            op.operatorSignature[0] ^= 1;
            payload.data = pqmn::Encode(op); tx.extraPayload = pq::EncodePayload(payload);
            BOOST_CHECK(!pq::VerifyInputs(tx, prevouts, Params(), reason));
            BOOST_CHECK_EQUAL(reason, "bad-pq-signature");
        } else if (mutation == 1 || mutation == 2) {
            const auto message = pqmn::SigningMessage(tx, prevouts, mutation == 1 ? uint256S("ff") : Params().GetConsensus().hashGenesisBlock);
            std::vector<unsigned char> sig;
            BOOST_REQUIRE(keys[0].Sign(message, pqmn::Context(mutation == 2 ? pqmn::Role::COLLATERAL : pqmn::Role::OWNER), sig));
            std::copy(sig.begin(), sig.end(), op.ownerSignature.begin()); payload.data = pqmn::Encode(op);
        } else {
            prevouts[0].nValue += COIN;
            view.SpendCoin(tx.vin[0].prevout);
            BOOST_REQUIRE(view.AccessCoin(tx.vin[0].prevout).IsSpent());
            view.AddCoin(tx.vin[0].prevout, Coin(prevouts[0], 1, false, false), false);
        }
        FeeSign(tx, payload);
        BOOST_REQUIRE(pq::VerifyInputs(tx, prevouts, Params(), reason));
        BOOST_CHECK(!index.Apply(tx, view, 20, reason)); BOOST_CHECK(index.List().empty());
    }
}

BOOST_AUTO_TEST_CASE(owner_can_change_payout_without_operator_and_revocation_requires_rotation)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    pqmn::Payload op; op.action = pqmn::Action::UPDATE; op.registration = reg.GetHash(); op.sequence = 1;
    op.operatorKey = keys[1].GetPublicKey(); op.payout = ID(5);
    BOOST_REQUIRE(index.Apply(Transaction(op), view, 21, reason));
    pqmn::Record record; BOOST_REQUIRE(index.Get(reg.GetHash(), record));
    BOOST_CHECK(record.service == Registration().service); BOOST_CHECK(record.payout == ID(5));
    op = {}; op.action = pqmn::Action::REVOKE; op.registration = reg.GetHash(); op.sequence = 2;
    BOOST_REQUIRE(index.Apply(Transaction(op), view, 22, reason));
    op.action = pqmn::Action::SERVICE; op.sequence = 3; op.service = Registration().service; op.operatorPayout = ID(1);
    BOOST_CHECK(!index.Apply(Transaction(op), view, 23, reason));
    op = {}; op.action = pqmn::Action::UPDATE; op.registration = reg.GetHash(); op.sequence = 3;
    op.operatorKey = keys[4].GetPublicKey(); op.payout = ID(0);
    BOOST_REQUIRE(index.Apply(Transaction(op, 4), view, 23, reason));
    BOOST_REQUIRE(index.Get(reg.GetHash(), record)); BOOST_CHECK(!record.revoked); BOOST_CHECK(record.service == CService());
}

BOOST_AUTO_TEST_CASE(missing_collateral_prevents_updates)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    view.SpendCoin(collateral); BOOST_REQUIRE(view.AccessCoin(collateral).IsSpent());
    pqmn::Payload op; op.action = pqmn::Action::UPDATE; op.registration = reg.GetHash(); op.sequence = 1;
    op.operatorKey = keys[1].GetPublicKey(); op.payout = ID(5);
    BOOST_CHECK(!index.Apply(Transaction(op), view, 21, reason));
    pqmn::Record record; BOOST_REQUIRE(index.Get(reg.GetHash(), record)); BOOST_CHECK_EQUAL(record.sequence, 0U);
}

BOOST_AUTO_TEST_CASE(corrupt_database_records_fail_closed)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    const auto prefix = std::make_pair(std::string("pqmn1r"), Params().GetConsensus().hashGenesisBlock);
    evoDb->Write(std::make_pair(prefix, reg.GetHash()), std::string("broken"));
    pqmn::Record record;
    BOOST_CHECK_THROW(index.Get(reg.GetHash(), record), std::runtime_error);
    BOOST_CHECK_THROW(index.List(), std::runtime_error);
    BOOST_CHECK_THROW(index.Undo(reg.GetHash(), reason), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(commission_requires_payout_but_unconfigured_endpoint_is_allowed)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    auto op = Registration(); op.operatorPayout = {};
    BOOST_CHECK(!index.Apply(Transaction(op), view, 20, reason));
    BOOST_CHECK(index.List().empty());
    op.operatorReward = 0; op.service = {};
    const CTransaction tx(Transaction(op)); BOOST_REQUIRE(index.Apply(tx, view, 20, reason));
    pqmn::Record record; BOOST_REQUIRE(index.Get(tx.GetHash(), record));
    BOOST_CHECK(record.service == CService()); BOOST_CHECK(record.operatorPayout == pq::KeyID{});
}

BOOST_AUTO_TEST_CASE(corrupt_reverse_property_cannot_be_erased)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    const auto key = std::make_pair(std::make_pair(std::string("pqmn1k"), Params().GetConsensus().hashGenesisBlock), ID(0));
    evoDb->Write(key, uint256S("abcdef"));
    BOOST_CHECK_THROW(index.Undo(reg.GetHash(), reason), std::runtime_error);
    pqmn::Record record; BOOST_CHECK(index.Get(reg.GetHash(), record));
    uint256 owner; BOOST_REQUIRE(evoDb->Read(key, owner)); BOOST_CHECK(owner == uint256S("abcdef"));
}

BOOST_AUTO_TEST_CASE(masternode_limits_do_not_expand_existing_modes)
{
    pq::Payload payload; payload.mode = pq::MASTERNODE; payload.authorizations.resize(2);
    payload.data.assign(pq::MAX_MASTERNODE_DATA_SIZE, 1);
    CMutableTransaction tx; tx.nVersion = 3; tx.nType = CTransaction::PQ; tx.sapData = nullopt;
    tx.vin.emplace_back(COutPoint(uint256S("01"), 0)); tx.vin.emplace_back(COutPoint(uint256S("02"), 0));
    tx.vout.emplace_back(COIN, pq::GetScript(ID(0))); tx.extraPayload = pq::EncodePayload(payload);
    BOOST_REQUIRE(!tx.extraPayload->empty()); std::string reason;
    BOOST_CHECK(pq::CheckStructure(tx, Params(), reason)); BOOST_CHECK(!pq::CheckContext(tx, Params(), 100, reason));
    BOOST_CHECK_EQUAL(pq::GetSigOpCost(tx), 5 * pq::SIGOP_COST);
    payload.data.push_back(1); BOOST_CHECK(pq::EncodePayload(payload).empty());
    payload.data.pop_back(); payload.authorizations.resize(3); BOOST_CHECK(pq::EncodePayload(payload).empty());
    payload.authorizations.resize(1); payload.mode = pq::GOVERNANCE_PROPOSAL;
    BOOST_CHECK(pq::EncodePayload(payload).empty()); // MN data cap does not apply to governance.
    payload.mode = pq::TRANSFER; BOOST_CHECK(pq::EncodePayload(payload).empty());
    payload.mode = pq::STAKE; BOOST_CHECK(pq::EncodePayload(payload).empty());
    pqmn::Payload decoded;
    BOOST_CHECK(!pqmn::Decode({static_cast<const unsigned char*>(nullptr), size_t(11318)}, decoded));
}

BOOST_AUTO_TEST_CASE(operator_cannot_update_owner_fields_and_bad_collateral_is_rejected)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    for (int change = 0; change < 3; ++change) {
        auto coin = view.AccessCoin(collateral); view.SpendCoin(collateral);
        if (change == 0) coin.out.nValue--;
        if (change == 1) coin.out.scriptPubKey = pq::GetScript(ID(5));
        if (change == 2) coin.nHeight = 21;
        view.AddCoin(collateral, std::move(coin), false);
        BOOST_CHECK(!index.Apply(Transaction(Registration()), view, 20, reason));
        BOOST_CHECK(index.List().empty()); view.SpendCoin(collateral);
        view.AddCoin(collateral, Coin(CTxOut(Params().GetConsensus().nMNCollateralAmt, pq::GetScript(ID(2))), 1, false, false), false);
    }
    const CTransaction reg(Transaction(Registration())); BOOST_REQUIRE(index.Apply(reg, view, 20, reason));
    pqmn::Payload op; op.action = pqmn::Action::UPDATE; op.registration = reg.GetHash(); op.sequence = 1;
    op.operatorKey = keys[1].GetPublicKey(); op.payout = ID(5);
    BOOST_CHECK(!index.Apply(Transaction(op, 1, false, 1), view, 21, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pqmn-owner-signature");
    pqmn::Record record; BOOST_REQUIRE(index.Get(reg.GetHash(), record)); BOOST_CHECK(record.payout == ID(0));
}

BOOST_AUTO_TEST_CASE(two_collateral_spends_and_registration_have_bounded_persistent_undo)
{
    LOCK(cs_main); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    const CTransaction first(Transaction(Registration())); BOOST_REQUIRE(index.Apply(first, view, 20, reason));
    auto other = Registration(); other.owner = keys[5].GetPublicKey(); other.operatorKey = keys[6].GetPublicKey(); other.collateralKey = keys[7].GetPublicKey();
    other.collateral = COutPoint(uint256S("dd"), 0); BOOST_REQUIRE(Lookup("127.0.0.1:51479", other.service, 0, false));
    const auto amount = Params().GetConsensus().nMNCollateralAmt;
    view.AddCoin(other.collateral, Coin(CTxOut(amount, pq::GetScript(ID(7))), 1, false, false), false);
    const CTransaction second(Transaction(other, 6, false, 5, 7)); BOOST_REQUIRE(index.Apply(second, view, 20, reason));

    CMutableTransaction tx; tx.nVersion = 3; tx.nType = CTransaction::PQ; tx.sapData = nullopt;
    tx.vin.emplace_back(collateral); tx.vin.emplace_back(other.collateral);
    tx.vout.emplace_back(amount, pq::GetScript(ID(2))); tx.vout.emplace_back(amount - COIN, pq::GetScript(ID(3)));
    const std::vector<CTxOut> prev{view.AccessCoin(collateral).out, view.AccessCoin(other.collateral).out};
    auto op = Registration(); op.collateral = COutPoint(uint256(), 0);
    pq::Payload payload; payload.mode = pq::MASTERNODE; payload.authorizations.resize(2);
    payload.authorizations[0].public_key = keys[2].GetPublicKey(); payload.authorizations[1].public_key = keys[7].GetPublicKey();
    payload.data = pqmn::Encode(op); tx.extraPayload = pq::EncodePayload(payload);
    const auto message = pqmn::SigningMessage(tx, prev, Params().GetConsensus().hashGenesisBlock);
    BOOST_REQUIRE(!message.empty());
    const auto roleSign = [&](size_t key, pqmn::Role role, pqmn::Signature& out) {
        std::vector<unsigned char> sig; BOOST_REQUIRE(keys[key].Sign(message, pqmn::Context(role), sig));
        std::copy(sig.begin(), sig.end(), out.begin());
    };
    roleSign(0, pqmn::Role::OWNER, op.ownerSignature); roleSign(1, pqmn::Role::OPERATOR, op.operatorSignature); roleSign(2, pqmn::Role::COLLATERAL, op.collateralSignature);
    payload.data = pqmn::Encode(op); tx.extraPayload = pq::EncodePayload(payload);
    for (size_t i = 0; i < 2; ++i) {
        std::vector<unsigned char> sig;
        BOOST_REQUIRE(keys[i ? 7 : 2].Sign(pq::SignatureMessage(tx, prev, payload, Params().GetConsensus().hashGenesisBlock, i),
                                        *pq::SignatureContext(Params().NetworkIDString()), sig));
        std::copy(sig.begin(), sig.end(), payload.authorizations[i].signature.begin());
    }
    tx.extraPayload = pq::EncodePayload(payload); const CTransaction combined(tx);
    BOOST_REQUIRE(index.Apply(combined, view, 21, reason)); BOOST_CHECK_EQUAL(index.List().size(), 1U);
    transaction->Commit(); BOOST_REQUIRE(evoDb->CommitRootTransaction());
    transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE(index.Undo(combined.GetHash(), reason)); BOOST_CHECK_EQUAL(index.List().size(), 2U);
    BOOST_REQUIRE(index.Undo(second.GetHash(), reason)); BOOST_REQUIRE(index.Undo(first.GetHash(), reason));
    BOOST_CHECK(index.List().empty());
}

BOOST_AUTO_TEST_CASE(declared_data_length_is_bounded_before_allocation)
{
    for (uint8_t mode : {pq::MASTERNODE, pq::GOVERNANCE_PROPOSAL}) {
        CMutableTransaction tx; tx.nVersion = 3; tx.nType = CTransaction::PQ; tx.sapData = nullopt;
        std::vector<unsigned char> bytes(3 + pq::AUTH_SIZE, 0);
        bytes[0] = 1; bytes[1] = mode; bytes[2] = 1;
        // Canonical CompactSize claims 32767 data bytes, but supplies none.
        bytes.insert(bytes.end(), {0xfd, 0xff, 0x7f}); tx.extraPayload = bytes;
        pq::Payload decoded;
        BOOST_CHECK(!pq::DecodePayload(tx, decoded));
        BOOST_CHECK(decoded.data.empty());
    }
}

BOOST_AUTO_TEST_CASE(block_lifecycle_orders_updates_and_rolls_back_validation)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto setup = evoDb->BeginTransaction(); setup->Commit();
    const auto reg = MakeTransactionRef(Transaction(Registration()));
    pqmn::Payload op; op.action = pqmn::Action::UPDATE; op.registration = reg->GetHash(); op.sequence = 1;
    op.operatorKey = keys[4].GetPublicKey(); op.payout = ID(5);
    RegistryBlock block(parent, {reg, MakeTransactionRef(Transaction(op, 4))});
    pqmn::Index index(*evoDb, Params()); std::string reason;
    {
        auto transaction = evoDb->BeginTransaction();
        BOOST_REQUIRE_MESSAGE(index.ConnectBlock(block.block, block.index, view, 20, reason), reason);
        pqmn::Record record; BOOST_REQUIRE(index.Get(reg->GetHash(), record));
        BOOST_CHECK_EQUAL(record.sequence, 1U); BOOST_CHECK(record.operatorKey == keys[4].GetPublicKey());
        BOOST_CHECK(view.HaveCoin(reg->vin[0].prevout)); // Disposable overlay did not spend caller's coins.
        BOOST_CHECK(view.GetBestBlock() == parentHash); BOOST_CHECK(evoDb->VerifyBestBlock(parentHash));
    }
    BOOST_CHECK(index.List().empty());
    auto transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(block.block, block.index, view, 20, reason), reason);
    evoDb->WriteBestBlock(block.hash); transaction->Commit();
    transaction = evoDb->BeginTransaction();
    view.SetBestBlock(block.hash);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(block.block, block.index, view, 20, reason), reason);
    BOOST_CHECK(index.List().empty());
    evoDb->WriteBestBlock(parentHash); view.SetBestBlock(parentHash); transaction->Commit();
    transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE(index.ConnectBlock(block.block, block.index, view, 20, reason));
}

BOOST_AUTO_TEST_CASE(block_lifecycle_rejects_late_invalid_update_without_committed_state)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto setup = evoDb->BeginTransaction(); setup->Commit();
    const auto reg = MakeTransactionRef(Transaction(Registration()));
    pqmn::Payload op; op.action = pqmn::Action::UPDATE; op.registration = reg->GetHash(); op.sequence = 2;
    op.operatorKey = keys[4].GetPublicKey(); op.payout = ID(5);
    RegistryBlock invalid(parent, {reg, MakeTransactionRef(Transaction(op, 4))});
    pqmn::Index index(*evoDb, Params()); std::string reason;
    {
        auto transaction = evoDb->BeginTransaction();
        BOOST_CHECK(!index.ConnectBlock(invalid.block, invalid.index, view, 20, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pqmn-sequence");
    }
    BOOST_CHECK(index.List().empty());
    RegistryBlock valid(parent, {reg}); auto transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(valid.block, valid.index, view, 20, reason), reason);
}

BOOST_AUTO_TEST_CASE(block_lifecycle_rejects_stale_or_missing_tips)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto setup = evoDb->BeginTransaction(); setup->Commit();
    RegistryBlock first(parent, {}); RegistryBlock second(first.index, {});
    pqmn::Index index(*evoDb, Params()); std::string reason;
    auto transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE(index.ConnectBlock(first.block, first.index, view, 20, reason));
    BOOST_CHECK(!index.ConnectBlock(first.block, first.index, view, 20, reason));
    BOOST_CHECK(!index.ConnectBlock(second.block, second.index, view, 20, reason));
    view.SetBestBlock(first.hash); // Coins ahead of EvoDB after interrupted flush.
    BOOST_CHECK(!index.ConnectBlock(second.block, second.index, view, 20, reason));
    evoDb->WriteBestBlock(first.hash);
    BOOST_REQUIRE(index.ConnectBlock(second.block, second.index, view, 20, reason));
    evoDb->WriteBestBlock(second.hash);
    view.SetBestBlock(second.hash);
    BOOST_CHECK(!index.DisconnectBlock(first.block, first.index, view, 20, reason));
    BOOST_REQUIRE(index.DisconnectBlock(second.block, second.index, view, 20, reason));
    evoDb->WriteBestBlock(first.hash);
    view.SetBestBlock(first.hash);
    BOOST_REQUIRE(index.DisconnectBlock(first.block, first.index, view, 20, reason));
    // A missing registry cannot be silently initialized above activation.
    BOOST_CHECK(!index.ConnectBlock(second.block, second.index, view, 20, reason));
}

BOOST_AUTO_TEST_CASE(block_lifecycle_sees_same_block_collateral_and_spend_order)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto setup = evoDb->BeginTransaction(); setup->Commit();
    auto funding = Transaction(Registration());
    funding.vout[0] = CTxOut(Params().GetConsensus().nMNCollateralAmt, pq::GetScript(ID(2)));
    pq::Payload fee; fee.authorizations.resize(1); fee.authorizations[0].public_key = keys[3].GetPublicKey();
    FeeSign(funding, fee); const auto funded = MakeTransactionRef(funding);
    auto op = Registration(); op.collateral = COutPoint(funded->GetHash(), 0);
    const auto reg = MakeTransactionRef(Transaction(op));
    CMutableTransaction spend; spend.nVersion = 3; spend.nType = CTransaction::PQ; spend.sapData = nullopt;
    spend.vin.emplace_back(op.collateral); spend.vout.emplace_back(funding.vout[0].nValue - COIN, pq::GetScript(ID(3)));
    fee.authorizations[0].public_key = keys[2].GetPublicKey();
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(keys[2].Sign(pq::SignatureMessage(spend, {funding.vout[0]}, fee, Params().GetConsensus().hashGenesisBlock, 0),
                              *pq::SignatureContext(Params().NetworkIDString()), signature));
    std::copy(signature.begin(), signature.end(), fee.authorizations[0].signature.begin());
    spend.extraPayload = pq::EncodePayload(fee); const auto spent = MakeTransactionRef(spend);
    pqmn::Index index(*evoDb, Params()); std::string reason;
    for (const auto& transactions : std::vector<std::vector<CTransactionRef>>{{reg, funded}, {funded, spent, reg}, {funded, funded}}) {
        RegistryBlock invalid(parent, transactions); auto transaction = evoDb->BeginTransaction();
        BOOST_CHECK(!index.ConnectBlock(invalid.block, invalid.index, view, 20, reason));
    }
    BOOST_CHECK(index.List().empty());
    RegistryBlock valid(parent, {funded, reg, spent}); auto transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(valid.block, valid.index, view, 20, reason), reason);
    BOOST_CHECK(index.List().empty()); BOOST_CHECK(!view.HaveCoin(op.collateral));
    evoDb->WriteBestBlock(valid.hash); transaction->Commit();
    transaction = evoDb->BeginTransaction();
    view.SetBestBlock(valid.hash);
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(valid.block, valid.index, view, 20, reason), reason);
    BOOST_CHECK(index.List().empty());
}

BOOST_AUTO_TEST_CASE(block_lifecycle_disk_flush_windows_require_matching_tips)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    const auto reg = MakeTransactionRef(Transaction(Registration()));
    RegistryBlock first(parent, {reg}); RegistryBlock second(first.index, {});
    for (int flushed = 0; flushed < 3; ++flushed) {
        SetDataDir("pqmn-flush-window-" + std::to_string(flushed));
        {
            CEvoDB database(1 << 20, false, true);
            CCoinsViewDB coinDatabase(1 << 20, false, true); CCoinsViewCache coins(&coinDatabase);
            coins.AddCoin(collateral, Coin(view.AccessCoin(collateral)), false);
            coins.AddCoin(reg->vin[0].prevout, Coin(view.AccessCoin(reg->vin[0].prevout)), false);
            coins.SetBestBlock(parentHash); BOOST_REQUIRE(coins.Flush());
            auto transaction = database.BeginTransaction(); database.WriteBestBlock(parentHash);
            transaction->Commit(); BOOST_REQUIRE(database.CommitRootTransaction());
            transaction = database.BeginTransaction(); pqmn::Index index(database, Params()); std::string reason;
            BOOST_REQUIRE(index.ConnectBlock(first.block, first.index, coins, 20, reason));
            for (const auto& tx : first.block.vtx) UpdateCoins(*tx, coins, first.index.nHeight);
            coins.SetBestBlock(first.hash); database.WriteBestBlock(first.hash); transaction->Commit();
            // Controlled persistence windows, not a process-kill/power-loss simulation.
            if (flushed >= 1) BOOST_REQUIRE(coins.Flush());
            if (flushed >= 2) BOOST_REQUIRE(database.CommitRootTransaction());
        }
        {
            CEvoDB database(1 << 20, false, false);
            CCoinsViewDB coinDatabase(1 << 20, false, false); CCoinsViewCache coins(&coinDatabase);
            pqmn::Index index(database, Params()); std::string reason; pqmn::Record record;
            BOOST_CHECK_EQUAL(index.Get(reg->GetHash(), record), flushed == 2);
            BOOST_CHECK(coinDatabase.GetHeadBlocks().empty());
            auto transaction = database.BeginTransaction();
            const RegistryBlock& next = flushed == 0 ? first : second;
            BOOST_CHECK_EQUAL(index.ConnectBlock(next.block, next.index, coins, 20, reason), flushed != 1);
            if (flushed == 1) BOOST_CHECK_EQUAL(reason, "bad-pqmn-chain-tip");
            if (flushed == 2) {
                database.WriteBestBlock(second.hash);
                coins.SetBestBlock(second.hash);
                BOOST_REQUIRE(index.DisconnectBlock(second.block, second.index, coins, 20, reason));
                database.WriteBestBlock(first.hash);
                coins.SetBestBlock(first.hash);
                BOOST_REQUIRE(index.DisconnectBlock(first.block, first.index, coins, 20, reason));
                BOOST_CHECK(index.List().empty());
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(block_lifecycle_rejects_preexisting_or_corrupt_registry_state)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto setup = evoDb->BeginTransaction(); setup->Commit();
    RegistryBlock first(parent, {}); RegistryBlock second(first.index, {});
    pqmn::Index index(*evoDb, Params()); std::string reason;
    for (char kind : {'c', 'k', 'r', 's', 'u'}) {
        auto transaction = evoDb->BeginTransaction();
        evoDb->Write(std::make_pair(std::make_pair(std::string("pqmn1") + kind, Params().GetConsensus().hashGenesisBlock), uint256()), uint256());
        BOOST_CHECK(!index.ConnectBlock(first.block, first.index, view, 20, reason));
        BOOST_CHECK_EQUAL(reason, "bad-pqmn-registry-not-empty");
    }
    auto transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE(index.ConnectBlock(first.block, first.index, view, 20, reason));
    view.SetBestBlock(first.hash); evoDb->WriteBestBlock(first.hash);
    const auto marker = std::make_pair(std::string("pqmn1b"), Params().GetConsensus().hashGenesisBlock);
    evoDb->Write(marker, std::string("broken"));
    BOOST_CHECK_THROW(index.ConnectBlock(second.block, second.index, view, 20, reason), std::runtime_error);
    BOOST_CHECK_THROW(index.DisconnectBlock(first.block, first.index, view, 20, reason), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(block_lifecycle_disconnect_requires_matching_coin_tip)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto transaction = evoDb->BeginTransaction();
    RegistryBlock block(parent, {}); pqmn::Index index(*evoDb, Params()); std::string reason;
    BOOST_REQUIRE(index.ConnectBlock(block.block, block.index, view, 20, reason));
    evoDb->WriteBestBlock(block.hash); // Registry ahead of coins.
    BOOST_CHECK(!index.DisconnectBlock(block.block, block.index, view, 20, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pqmn-chain-tip");
}

BOOST_AUTO_TEST_CASE(block_lifecycle_rejects_persisted_coin_transition_heads)
{
    LOCK(cs_main); SetDataDir("pqmn-interrupted-heads");
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    RegistryBlock block(parent, {});
    struct CoinDB : CCoinsViewDB { using CCoinsViewDB::CCoinsViewDB; using CCoinsViewDB::db; };
    {
        CoinDB database(1 << 20, false, true);
        // Persist the transition metadata written by the first partial coin batch.
        // This is a crafted on-disk fixture, not a crash-injected BatchWrite run.
        BOOST_REQUIRE(database.db.Write('H', std::vector<uint256>{block.hash, parentHash}));
    }
    CoinDB database(1 << 20, false, false); CCoinsViewCache coins(&database);
    BOOST_REQUIRE_EQUAL(coins.GetHeadBlocks().size(), 2U);
    coins.SetBestBlock(parentHash); // A cached tip must not hide an unfinished flush.
    evoDb->WriteBestBlock(parentHash); auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params()); std::string reason;
    BOOST_CHECK(!index.ConnectBlock(block.block, block.index, coins, 20, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pqmn-chain-tip");
    // Build consistent registry state using a separate clean view, then check disconnect too.
    transaction->Rollback(); transaction = evoDb->BeginTransaction();
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    BOOST_REQUIRE(index.ConnectBlock(block.block, block.index, view, 20, reason));
    evoDb->WriteBestBlock(block.hash); coins.SetBestBlock(block.hash);
    BOOST_CHECK(!index.DisconnectBlock(block.block, block.index, coins, 20, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pqmn-chain-tip");
}

BOOST_AUTO_TEST_CASE(block_lifecycle_rejects_wrong_network_height_and_block_identity)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto transaction = evoDb->BeginTransaction();
    RegistryBlock block(parent, {}); pqmn::Index index(*evoDb, Params()); std::string reason;
    for (int firstHeight : {-1, 0, 21})
        BOOST_CHECK(!index.ConnectBlock(block.block, block.index, view, firstHeight, reason));
    struct UnknownNetwork : CChainParams {
        UnknownNetwork() : CChainParams(*CreateChainParams(CBaseChainParams::MAIN)) { strNetworkID = "unknown"; }
        const CCheckpointData& Checkpoints() const override { return Params().Checkpoints(); }
    } unknown;
    pqmn::Index unknownIndex(*evoDb, unknown);
    BOOST_CHECK(!unknownIndex.ConnectBlock(block.block, block.index, view, 20, reason));
    for (int mutation = 0; mutation < 4; ++mutation) {
        CBlockIndex invalid = block.index;
        if (mutation == 0) invalid.phashBlock = nullptr;
        if (mutation == 1) invalid.pprev = nullptr;
        if (mutation == 2) ++invalid.nHeight;
        if (mutation == 3) invalid.phashBlock = &parentHash;
        BOOST_CHECK(!index.ConnectBlock(block.block, invalid, view, 20, reason));
        BOOST_CHECK(!index.DisconnectBlock(block.block, invalid, view, 20, reason));
    }
    CBlock invalid = block.block; invalid.hashPrevBlock = uint256S("ffff");
    BOOST_CHECK(!index.ConnectBlock(invalid, block.index, view, 20, reason));
    BOOST_REQUIRE(index.ConnectBlock(block.block, block.index, view, 20, reason));
}

BOOST_AUTO_TEST_CASE(block_lifecycle_failed_disconnect_rolls_back_earlier_undo)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto transaction = evoDb->BeginTransaction();
    const auto reg = MakeTransactionRef(Transaction(Registration()));
    pqmn::Payload op; op.action = pqmn::Action::UPDATE; op.registration = reg->GetHash(); op.sequence = 1;
    op.operatorKey = keys[4].GetPublicKey(); op.payout = ID(5);
    RegistryBlock block(parent, {reg, MakeTransactionRef(Transaction(op, 4))});
    pqmn::Index index(*evoDb, Params()); std::string reason;
    BOOST_REQUIRE(index.ConnectBlock(block.block, block.index, view, 20, reason));
    evoDb->WriteBestBlock(block.hash); view.SetBestBlock(block.hash);
    evoDb->Erase(std::make_pair(std::make_pair(std::string("pqmn1u"), Params().GetConsensus().hashGenesisBlock), reg->GetHash()));
    transaction->Commit();
    transaction = evoDb->BeginTransaction();
    BOOST_CHECK(!index.DisconnectBlock(block.block, block.index, view, 20, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pqmn-missing-undo");
    transaction->Rollback();
    pqmn::Record record; BOOST_REQUIRE(index.Get(reg->GetHash(), record));
    BOOST_CHECK_EQUAL(record.sequence, 1U); BOOST_CHECK(record.operatorKey == keys[4].GetPublicKey());
    uint256 marker;
    BOOST_REQUIRE(evoDb->Read(std::make_pair(std::string("pqmn1b"), Params().GetConsensus().hashGenesisBlock), marker));
    BOOST_CHECK(marker == block.hash); BOOST_CHECK(evoDb->VerifyBestBlock(block.hash));
}

BOOST_AUTO_TEST_CASE(startup_replay_checks_clean_coin_and_evo_tips)
{
    LOCK(cs_main); SetDataDir("pq-startup-clean");
    const uint256 firstHash = uint256S("1234"), secondHash = uint256S("5678");
    CBlockIndex first, second; first.phashBlock = &firstHash; first.nHeight = 1;
    second.phashBlock = &secondHash; second.nHeight = 2; second.pprev = &first;
    BOOST_REQUIRE(mapBlockIndex.emplace(firstHash, &first).second);
    BOOST_REQUIRE(mapBlockIndex.emplace(secondHash, &second).second);
    struct Cleanup { const uint256 a, b; ~Cleanup() { mapBlockIndex.erase(a); mapBlockIndex.erase(b); } } cleanup{firstHash, secondHash};
    CCoinsViewDB database(1 << 20, true, true); CCoinsViewCache coins(&database);
    bool requiresReindex{true};
    BOOST_CHECK(ReplayBlocks(Params(), &database, requiresReindex)); // Fresh/rebuilt databases.
    BOOST_CHECK(!requiresReindex);
    for (char kind : {'a', 'b'}) {
        const auto key = std::make_pair(std::string("pqmn1") + kind, Params().GetConsensus().hashGenesisBlock);
        BOOST_REQUIRE(evoDb->GetRawDB().Write(key, std::string("x")));
        for (int attempt = 0; attempt < 2; ++attempt) {
            BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
            BOOST_CHECK(evoDb->GetRawDB().Exists(key));
        }
        BOOST_REQUIRE(evoDb->GetRawDB().Erase(key));
    }
    {
        auto transaction = evoDb->BeginTransaction(); evoDb->WriteBestBlock(firstHash);
        BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); // Empty coins cannot conceal an old index.
        BOOST_CHECK(requiresReindex);
    }
    coins.SetBestBlock(secondHash); BOOST_REQUIRE(coins.Flush());
    for (int state = 0; state < 4; ++state) {
        auto transaction = evoDb->BeginTransaction();
        if (state == 0) evoDb->Erase(EVODB_BEST_BLOCK);
        if (state == 1) evoDb->WriteBestBlock(firstHash);
        if (state == 2) evoDb->WriteBestBlock(uint256S("ffff"));
        // Corrupt serialized storage, not the typed transaction cache.
        if (state == 3) BOOST_REQUIRE(evoDb->GetRawDB().Write(EVODB_BEST_BLOCK, std::string("broken")));
        BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex));
        BOOST_CHECK(requiresReindex);
        BOOST_CHECK(database.GetBestBlock() == secondHash);
        BOOST_CHECK(database.GetHeadBlocks().empty());
        if (state == 3) BOOST_REQUIRE(evoDb->GetRawDB().Erase(EVODB_BEST_BLOCK));
    }
    auto transaction = evoDb->BeginTransaction(); evoDb->WriteBestBlock(secondHash);
    BOOST_CHECK(ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(!requiresReindex);
    coins.SetBestBlock(firstHash); BOOST_REQUIRE(coins.Flush()); // EvoDB ahead of coins.
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
    coins.SetBestBlock(uint256S("abcd")); BOOST_REQUIRE(coins.Flush());
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); // Unknown coin tip.
    coins.SetBestBlock(secondHash); BOOST_REQUIRE(coins.Flush());
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 2);
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); // Missing active registry.
    BOOST_CHECK(requiresReindex);
    const auto activation = std::make_pair(std::string("pqmn1a"), Params().GetConsensus().hashGenesisBlock);
    const auto marker = std::make_pair(std::string("pqmn1b"), Params().GetConsensus().hashGenesisBlock);
    const auto schema = std::make_pair(std::string("pqmn1v"), Params().GetConsensus().hashGenesisBlock);
    const auto serviceActivation = std::make_pair(std::string("pqmn1j"), Params().GetConsensus().hashGenesisBlock);
    evoDb->Write(activation, 2); evoDb->Write(marker, secondHash);
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex); // Schema is mandatory.
    evoDb->Write(schema, uint8_t{3});
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
    evoDb->Write(schema, uint8_t{4});
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
    evoDb->Write(serviceActivation, Params().GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_SERVICE].nActivationHeight);
    BOOST_CHECK(ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(!requiresReindex);
    evoDb->Write(schema, uint8_t{5});
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
    evoDb->Write(schema, uint8_t{4});
    evoDb->Write(marker, firstHash);
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
    evoDb->Write(marker, secondHash);
    for (int changed : {-1, 0, 1, 3}) {
        UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, changed);
        BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
    }
}

BOOST_AUTO_TEST_CASE(startup_replay_refuses_partial_pq_state_before_writes)
{
    LOCK(cs_main); SetDataDir("pq-startup-heads");
    const uint256 firstHash = uint256S("1234"), secondHash = uint256S("5678");
    CBlockIndex first, second; first.phashBlock = &firstHash; first.nHeight = 1;
    second.phashBlock = &secondHash; second.nHeight = 2; second.pprev = &first;
    BOOST_REQUIRE(mapBlockIndex.emplace(firstHash, &first).second);
    BOOST_REQUIRE(mapBlockIndex.emplace(secondHash, &second).second);
    struct Cleanup { const uint256 a, b; ~Cleanup() { mapBlockIndex.erase(a); mapBlockIndex.erase(b); } } cleanup{firstHash, secondHash};
    struct CoinDB : CCoinsViewDB { using CCoinsViewDB::CCoinsViewDB; using CCoinsViewDB::db; };
    CoinDB database(1 << 20, true, true);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 2);
    for (const auto& heads : std::vector<std::vector<uint256>>{{secondHash, firstHash}, {firstHash, secondHash}, {secondHash, secondHash},
             {secondHash, uint256()}, {secondHash}, {uint256S("abcd"), firstHash}, {secondHash, uint256S("abcd")}}) {
        BOOST_REQUIRE(database.db.Write('H', heads));
        auto transaction = evoDb->BeginTransaction(); evoDb->WriteBestBlock(firstHash);
        bool requiresReindex{false};
        BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex));
        BOOST_CHECK(requiresReindex);
        BOOST_CHECK(database.GetHeadBlocks() == heads);
        BOOST_CHECK(database.GetBestBlock().IsNull()); BOOST_CHECK(evoDb->VerifyBestBlock(firstHash));
    }
    // Changing both local activation settings must not make existing registry state replayable.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 100);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 100);
    const std::vector<uint256> heads{firstHash, firstHash};
    BOOST_REQUIRE(database.db.Write('H', heads));
    auto transaction = evoDb->BeginTransaction(); evoDb->WriteBestBlock(firstHash);
    evoDb->Write(std::make_pair(std::string("pqmn1a"), Params().GetConsensus().hashGenesisBlock), 1);
    evoDb->Write(std::make_pair(std::string("pqmn1b"), Params().GetConsensus().hashGenesisBlock), firstHash);
    bool requiresReindex{false};
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
    BOOST_CHECK(database.GetHeadBlocks() == heads); BOOST_CHECK(database.GetBestBlock().IsNull());
}

BOOST_AUTO_TEST_CASE(startup_replay_preserves_pre_pq_path_and_checks_v6_tip)
{
    LOCK(cs_main); SetDataDir("pq-startup-legacy");
    const uint256 hash = uint256S("1234"); CBlockIndex tip;
    tip.phashBlock = &hash; tip.nHeight = 1;
    BOOST_REQUIRE(mapBlockIndex.emplace(hash, &tip).second);
    struct Cleanup { const uint256 hash; ~Cleanup() { mapBlockIndex.erase(hash); } } cleanup{hash};
    struct CoinDB : CCoinsViewDB { using CCoinsViewDB::CCoinsViewDB; using CCoinsViewDB::db; };
    CoinDB database(1 << 20, true, true); CCoinsViewCache coins(&database);
    coins.SetBestBlock(hash); BOOST_REQUIRE(coins.Flush());
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 100);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, 100);
    bool requiresReindex{true};
    BOOST_CHECK(ReplayBlocks(Params(), &database, requiresReindex));
    BOOST_CHECK(!requiresReindex); // Pre-index clean tips do not require an EvoDB marker.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_V6_0, 1);
    BOOST_CHECK(!ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(requiresReindex);
    auto transaction = evoDb->BeginTransaction(); evoDb->WriteBestBlock(hash);
    BOOST_CHECK(ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(!requiresReindex);
    // An interrupted pre-PQ flush already at its target still follows legacy replay.
    BOOST_REQUIRE(database.db.Erase('B'));
    BOOST_REQUIRE(database.db.Write('H', std::vector<uint256>{hash, hash}));
    BOOST_CHECK(ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(!requiresReindex);
    BOOST_CHECK(database.GetHeadBlocks().empty()); BOOST_CHECK(database.GetBestBlock() == hash);
    BOOST_CHECK(evoDb->VerifyBestBlock(hash));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(pqmn_runtime_tests, MNSetupT<TestingSetup>)

BOOST_AUTO_TEST_CASE(pending_claim_resource_samples)
{
    auto candidate = BlockAssembler(Params(), false).CreateNewBlock(pq::GetScript(ID(3)), nullptr, false, nullptr, true);
    BOOST_REQUIRE(candidate);
    auto block = std::make_shared<CBlock>(candidate->block);
    BOOST_REQUIRE(SolveBlock(block, 1));
    BOOST_REQUIRE(ProcessNewBlock(block, nullptr));
    LOCK(cs_main);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 2);
    const auto emptyBytes = mempool.DynamicMemoryUsage();
    for (uint32_t n = 1; n <= 1000; ++n) {
        for (size_t role = 0; role < 3; ++role) {
            std::array<unsigned char, mldsa44::SEED_SIZE> seed{};
            seed[0] = role; seed[1] = n & 255; seed[2] = n >> 8; seed[3] = 0xcc;
            BOOST_REQUIRE(keys[role].SetSeed(seed));
        }
        auto op = Registration(); op.service = CService();
        op.collateral = COutPoint(uint256S("dd"), n);
        Coin coin(CTxOut(Params().GetConsensus().nMNCollateralAmt, pq::GetScript(ID(2))), 1, false, false);
        view.AddCoin(op.collateral, Coin(coin), false);
        pcoinsTip->AddCoin(op.collateral, std::move(coin), false);
        const auto tx = MakeTransactionRef(Transaction(op));
        pcoinsTip->AddCoin(tx->vin[0].prevout, Coin(view.AccessCoin(tx->vin[0].prevout)), false);
        CValidationState state;
        BOOST_REQUIRE_MESSAGE(AcceptToMemoryPool(mempool, state, tx, false, nullptr), state.GetRejectReason());
        if (n == 1 || n == 100 || n == 1000) {
            for (int sample = 0; sample < 3; ++sample) {
                const auto start = std::chrono::steady_clock::now();
                std::string reason;
                BOOST_REQUIRE(mempool.CheckPQMN(*tx, *pcoinsTip, 2, reason));
                const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
                BOOST_TEST_MESSAGE("PQ MN pending=" << n << " sample=" << sample << " check_us=" << micros
                    << " pool_bytes=" << mempool.DynamicMemoryUsage());
            }
        }
    }
    BOOST_CHECK_EQUAL(mempool.size(), 1000U);
    BOOST_CHECK(mempool.DynamicMemoryUsage() > emptyBytes);
    mempool.clear();
    BOOST_CHECK_EQUAL(mempool.DynamicMemoryUsage(), emptyBytes);
    // M1 diagnostic sample with real signatures and synthetic confirmed UTXOs,
    // not public-network capacity, maximum configured pool or ARM qualification.
}

BOOST_AUTO_TEST_CASE(relay_collateral_property_conflicts_and_removal)
{
    auto candidate = BlockAssembler(Params(), false).CreateNewBlock(pq::GetScript(ID(3)), nullptr, false, nullptr, true);
    BOOST_REQUIRE(candidate);
    auto block = std::make_shared<CBlock>(candidate->block);
    BOOST_REQUIRE(SolveBlock(block, 1));
    BOOST_REQUIRE(ProcessNewBlock(block, nullptr));
    LOCK(cs_main);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 2);
    pcoinsTip->AddCoin(collateral, Coin(view.AccessCoin(collateral)), false);
    const auto funded = [&](const pqmn::Payload& op, size_t operatorKey = 1, size_t ownerKey = 0, size_t collateralKey = 2) {
        auto tx = Transaction(op, operatorKey, false, ownerKey, collateralKey);
        pcoinsTip->AddCoin(tx.vin[0].prevout, Coin(view.AccessCoin(tx.vin[0].prevout)), false);
        return MakeTransactionRef(tx);
    };
    const auto admit = [&](const CTransactionRef& tx) {
        CValidationState state;
        const bool accepted = AcceptToMemoryPool(mempool, state, tx, false, nullptr);
        BOOST_CHECK_MESSAGE(accepted, state.GetRejectReason());
        return accepted;
    };
    const auto reject = [&](const CTransactionRef& tx, const std::string& expected) {
        CValidationState state;
        BOOST_CHECK(!AcceptToMemoryPool(mempool, state, tx, false, nullptr));
        BOOST_CHECK_EQUAL(state.GetRejectReason(), expected);
        BOOST_CHECK(!mempool.exists(tx->GetHash()));
    };
    const auto transfer = [&](const COutPoint& input, size_t key) {
        CMutableTransaction tx; tx.nVersion = 3; tx.nType = CTransaction::PQ; tx.sapData = nullopt;
        tx.vin.emplace_back(input);
        tx.vout.emplace_back(view.AccessCoin(input).out.nValue - COIN, pq::GetScript(ID(3)));
        pq::Payload payload; payload.authorizations.resize(1);
        payload.authorizations[0].public_key = keys[key].GetPublicKey();
        FeeSign(tx, payload, key);
        return MakeTransactionRef(tx);
    };
    const auto reg = funded(Registration());
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 3);
    reject(reg, "bad-pq-masternode-not-active");
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 2);
    auto bad = CMutableTransaction(*reg); pq::Payload envelope; pqmn::Payload operation;
    BOOST_REQUIRE(pq::DecodePayload(bad, envelope)); BOOST_REQUIRE(pqmn::Decode(envelope.data, operation));
    operation.ownerSignature[0] ^= 1; envelope.data = pqmn::Encode(operation); FeeSign(bad, envelope);
    reject(MakeTransactionRef(bad), "bad-pqmn-registration-signature");
    BOOST_REQUIRE(admit(reg));
    BOOST_CHECK(mempool.IsPQMNCollateral(collateral));
    pqmn::Index registry(*evoDb, Params()); pqmn::Record record;
    BOOST_CHECK(!registry.Get(reg->GetHash(), record));
    reject(funded(Registration()), "pqmn-operation-pending");

    const COutPoint secondCollateral(uint256S("cc"), 0);
    view.AddCoin(secondCollateral, Coin(CTxOut(Params().GetConsensus().nMNCollateralAmt,
        pq::GetScript(ID(6))), 1, false, false), false);
    pcoinsTip->AddCoin(secondCollateral, Coin(view.AccessCoin(secondCollateral)), false);
    auto other = Registration(); other.collateral = secondCollateral;
    other.owner = keys[4].GetPublicKey(); other.operatorKey = keys[5].GetPublicKey(); other.collateralKey = keys[6].GetPublicKey();
    reject(funded(other, 5, 4, 6), "pqmn-service-pending");
    BOOST_REQUIRE(Lookup("127.0.0.1:51477", other.service, 0, false));
    other.operatorKey = keys[1].GetPublicKey();
    reject(funded(other, 1, 4, 6), "pqmn-key-pending");
    other.operatorKey = keys[5].GetPublicKey();
    const auto unrelated = funded(other, 5, 4, 6);
    BOOST_REQUIRE(admit(unrelated));

    const auto spend = transfer(collateral, 2);
    reject(spend, "pqmn-collateral-pending");
#ifdef ENABLE_WALLET
    CWallet wallet("pending-collateral", WalletDatabase::CreateDummy());
    BOOST_CHECK(wallet.IsPQCollateral(collateral));
#endif
    mempool.removeRecursive(*reg);
    BOOST_CHECK(!mempool.IsPQMNCollateral(collateral));
    BOOST_REQUIRE(admit(spend));
    reject(reg, "pqmn-collateral-spent-in-mempool");
    mempool.removeRecursive(*spend);
    BOOST_REQUIRE(admit(reg));
    const COutPoint change(reg->GetHash(), 0);
    view.AddCoin(change, Coin(reg->vout[0], MEMPOOL_HEIGHT, false, false), false);
    const auto child = transfer(change, 3);
    BOOST_REQUIRE(admit(child)); // Non-collateral change may have ordinary children.
    // Use the child's still-unspent output as a fee input, independent of any registration dependency.
    const COutPoint childChange(child->GetHash(), 0);
    view.AddCoin(childChange, Coin(child->vout[0], MEMPOOL_HEIGHT, false, false), false);
    const auto unconfirmed = MakeTransactionRef(Transaction(other, 5, false, 4, 6, childChange));
    reject(unconfirmed, "bad-pqmn-input");
    mempool.removeRecursive(*reg, MemPoolRemovalReason::EXPIRY);
    BOOST_CHECK(!mempool.exists(child->GetHash()));
    BOOST_CHECK(!mempool.IsPQMNCollateral(collateral));
    BOOST_CHECK(mempool.exists(unrelated->GetHash()));
    BOOST_CHECK(mempool.IsPQMNCollateral(secondCollateral));
    mempool.TrimToSize(0);
    BOOST_CHECK(!mempool.IsPQMNCollateral(secondCollateral));
    mempool.clear(); // Reset rolling fee policy before separately exercising clear's claim cleanup.
    BOOST_REQUIRE(admit(reg));
    mempool.clear();
    BOOST_CHECK(!mempool.IsPQMNCollateral(collateral));
#ifdef ENABLE_WALLET
    BOOST_CHECK(!wallet.IsPQCollateral(collateral));
#endif
    BOOST_CHECK(registry.List().empty());
    BOOST_REQUIRE(admit(reg));
    const auto otherSpend = transfer(secondCollateral, 6);
    BOOST_REQUIRE(admit(otherSpend));
    const auto reverseKey = std::make_pair(std::make_pair(std::string("pqmn1c"),
        Params().GetConsensus().hashGenesisBlock), collateral);
    BOOST_REQUIRE(evoDb->GetRawDB().Write(reverseKey, std::string("x")));
    std::string reason; bool accepted = true;
    BOOST_CHECK_NO_THROW(accepted = mempool.CheckPQMN(*reg, *pcoinsTip, 2, reason));
    BOOST_CHECK(!accepted);
    BOOST_CHECK_EQUAL(reason, "pqmn-registry-unavailable");
    BOOST_REQUIRE(evoDb->GetRawDB().Erase(reverseKey));
    auto unavailable = std::move(evoDb);
    accepted = true;
    BOOST_CHECK_NO_THROW(accepted = mempool.CheckPQMN(*reg, *pcoinsTip, 2, reason));
    BOOST_CHECK(!accepted);
    BOOST_CHECK_NO_THROW(mempool.removeForReorg(pcoinsTip.get(), 2, STANDARD_LOCKTIME_VERIFY_FLAGS));
    evoDb = std::move(unavailable);
    BOOST_CHECK(!mempool.exists(reg->GetHash()));
    BOOST_CHECK(!mempool.IsPQMNCollateral(collateral));
    BOOST_CHECK(mempool.exists(otherSpend->GetHash())); // Unrelated payments are not cleared.
}

#ifdef ENABLE_WALLET
BOOST_AUTO_TEST_CASE(wallet_masternode_construction)
{
    struct DiskWallet : CWallet {
        using CWallet::CWallet;
        ~DiskWallet() { GetDBHandle().Flush(true); }
    } wallet("pqmn-builder", WalletDatabase::Create(GetDataDir() / "pqmn-builder"));
    bool firstRun;
    BOOST_REQUIRE_EQUAL(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    const SecureString passphrase = "pqmn-builder-test-only";
    BOOST_REQUIRE(wallet.EncryptWallet(passphrase));
    BOOST_REQUIRE(wallet.Unlock(passphrase));
    for (size_t role : {0U, 2U, 3U}) {
        std::string address;
        BOOST_REQUIRE(wallet.GeneratePQAddress(address));
        BOOST_REQUIRE(wallet.GetPQKey(address, keys[role]));
    }
    auto candidate = BlockAssembler(Params(), false).CreateNewBlock(pq::GetScript(ID(3)), nullptr, false, nullptr, true);
    BOOST_REQUIRE(candidate);
    auto block = std::make_shared<CBlock>(candidate->block);
    BOOST_REQUIRE(SolveBlock(block, 1));
    BOOST_REQUIRE(ProcessNewBlock(block, nullptr));
    LOCK2(cs_main, wallet.cs_wallet);
    const CAmount amount = Params().GetConsensus().nMNCollateralAmt;
    CMutableTransaction source;
    source.vin.emplace_back(uint256S("8765"), 0);
    source.vout.emplace_back(amount, pq::GetScript(ID(2)));
    source.vout.emplace_back(amount + 10 * COIN, pq::GetScript(ID(3)));
    const auto sourceRef = MakeTransactionRef(source);
    wallet.SetLastBlockProcessed(chainActive.Tip());
    BOOST_REQUIRE(wallet.AddToWalletIfInvolvingMe(sourceRef,
        {CWalletTx::Status::CONFIRMED, 1, chainActive[1]->GetBlockHash(), 0}, true));
    for (uint32_t i = 0; i < 2; ++i)
        pcoinsTip->AddCoin(COutPoint(sourceRef->GetHash(), i), Coin(source.vout[i], 1, false, false), false);
    auto op = Registration(); op.collateral = COutPoint(sourceRef->GetHash(), 0);
    const auto addresses = wallet.GetPQAddresses();
    const auto walletSize = wallet.mapWallet.size();
    CTransactionRef tx; CAmount fee = 0; std::string reason;
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(op, &keys[1], tx, fee, reason)); // Inactive.
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(op, &keys[1], tx, fee, reason), reason);
    BOOST_REQUIRE(tx); BOOST_REQUIRE_EQUAL(tx->vin.size(), 1U);
    BOOST_CHECK(tx->vin[0].prevout == COutPoint(sourceRef->GetHash(), 1));
    BOOST_REQUIRE_EQUAL(tx->vout.size(), 1U);
    BOOST_CHECK(tx->vout[0].scriptPubKey == pq::GetScript(ID(3)));
    BOOST_CHECK_EQUAL(source.vout[1].nValue - tx->GetValueOut(), fee);
    BOOST_CHECK(fee > 0 && fee <= maxTxFee);
    pqmn::Index registry(*evoDb, Params()); pqmn::Record checked;
    auto transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE_MESSAGE(registry.Check(*tx, *pcoinsTip, 2, checked, reason), reason);
    BOOST_CHECK(registry.List().empty());
    BOOST_CHECK(!mempool.exists(tx->GetHash()));
    BOOST_CHECK_EQUAL(wallet.mapWallet.size(), walletSize);
    BOOST_CHECK(wallet.GetPQAddresses() == addresses); // No unbacked change key.
    const auto external = tx;
    CCoinControl selection;
    selection.Select(COutPoint(uint256S("deadbeef"), 0));
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(op, &keys[1], tx, fee, reason, &selection));
    BOOST_CHECK(!tx);
    selection.SetNull();
    selection.Select(op.collateral);
    selection.fAllowOtherInputs = true;
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(op, &keys[1], tx, fee, reason, &selection));
    BOOST_CHECK(!tx); // The external bond cannot silently be replaced by a fee input.
    selection.SetNull();
    selection.Select(COutPoint(sourceRef->GetHash(), 1));
    selection.nMinimumTotalFee = COIN;
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(op, &keys[1], tx, fee, reason, &selection), reason);
    BOOST_REQUIRE_EQUAL(tx->vin.size(), 1U);
    BOOST_CHECK(tx->vin[0].prevout == COutPoint(sourceRef->GetHash(), 1));
    BOOST_CHECK(fee >= COIN);
    selection.Select(COutPoint(uint256S("deadbeef"), 0));
    selection.Select(COutPoint(uint256S("deadbeef"), 1));
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(op, &keys[1], tx, fee, reason, &selection));
    selection.SetNull();
    selection.Select(op.collateral);
    selection.fAllowOtherInputs = true;
    Coin removed;
    pcoinsTip->SpendCoin(op.collateral, &removed);
    BOOST_REQUIRE(!removed.IsSpent());
    auto selectedInternal = op; selectedInternal.collateral = COutPoint(uint256(), 0);
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(selectedInternal, &keys[1], tx, fee, reason, &selection));
    BOOST_CHECK(!tx); // A stale selected coin cannot be silently substituted with another coin.
    pcoinsTip->AddCoin(op.collateral, std::move(removed), false);
    selection.SetNull();
    const auto failure = [&](const pqmn::Payload& operation, const mldsa44::Key* signer) {
        tx = external; fee = COIN; reason.clear();
        BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(operation, signer, tx, fee, reason));
        BOOST_CHECK(!tx); BOOST_CHECK_EQUAL(fee, 0); BOOST_CHECK(!reason.empty());
        BOOST_CHECK(wallet.GetPQAddresses() == addresses);
        BOOST_CHECK_EQUAL(wallet.mapWallet.size(), walletSize);
    };
    failure(op, nullptr);
    failure(op, &keys[4]);
    auto invalid = op; invalid.owner = keys[5].GetPublicKey(); failure(invalid, &keys[1]);
    invalid = op; invalid.collateralKey = keys[5].GetPublicKey(); failure(invalid, &keys[1]);
    invalid = op; invalid.owner = op.collateralKey; failure(invalid, &keys[1]);
    invalid = op; invalid.operatorReward = 10001; failure(invalid, &keys[1]);
    invalid = op; invalid.collateral = COutPoint(uint256S("dead"), 0); failure(invalid, &keys[1]);
    invalid = op; invalid.action = static_cast<pqmn::Action>(255); failure(invalid, &keys[1]);
    BOOST_REQUIRE(wallet.Lock()); failure(op, &keys[1]);
    BOOST_REQUIRE(wallet.Unlock(passphrase));
    wallet.fWalletUnlockStaking = true; failure(op, &keys[1]); wallet.fWalletUnlockStaking = false;
    wallet.LockCoin(COutPoint(sourceRef->GetHash(), 1));
    selection.SetNull();
    selection.Select(COutPoint(sourceRef->GetHash(), 1));
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(op, &keys[1], tx, fee, reason, &selection));
    BOOST_CHECK(!tx);
    BOOST_CHECK_EQUAL(reason, "A selected coin is spent, locked, immature or no longer confirmed");
    failure(op, &keys[1]); // Only the registration's own collateral remains available.
    wallet.UnlockCoin(COutPoint(sourceRef->GetHash(), 1));
    const auto priorMaxFee = maxTxFee; maxTxFee = 1;
    failure(op, &keys[1]); maxTxFee = priorMaxFee;
    auto missingRegistry = std::move(evoDb);
    failure(op, &keys[1]); evoDb = std::move(missingRegistry);
    auto internal = op; internal.collateral = COutPoint(uint256(), 0);
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(internal, &keys[1], tx, fee, reason), reason);
    BOOST_REQUIRE_EQUAL(tx->vout.size(), 2U);
    BOOST_CHECK(tx->vout[0] == CTxOut(amount, pq::GetScript(ID(2))));
    BOOST_CHECK(tx->vout[1].scriptPubKey == pq::GetScript(ID(3)));
    BOOST_REQUIRE(registry.Check(*tx, *pcoinsTip, 2, checked, reason));
    BOOST_CHECK(checked.collateral == COutPoint(tx->GetHash(), 0));
    internal.collateral.n = 1; failure(internal, &keys[1]);
    BOOST_CHECK(registry.List().empty());
    // The ordinary payment entry point must not bypass the dedicated role signer.
    BOOST_CHECK(!wallet.CreatePQTransaction({source.vout[0]}, pq::MASTERNODE,
        pqmn::Encode(op), tx, fee, reason));
    tx = external;
    BOOST_REQUIRE(registry.Apply(*tx, *pcoinsTip, 2, reason));
    const auto registration = tx->GetHash();
    selection.Select(op.collateral);
    selection.fAllowOtherInputs = true;
    auto protectedInternal = internal; protectedInternal.collateral.n = 0;
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(protectedInternal, &keys[1], tx, fee, reason, &selection));
    BOOST_CHECK(!tx); // Explicit selection never allows an active bond to fund a masternode fee.
    BOOST_CHECK_EQUAL(reason, "A selected coin is spent, locked, immature or no longer confirmed");
    pqmn::Payload update; update.action = pqmn::Action::UPDATE;
    update.registration = registration; update.sequence = 1;
    update.operatorKey = keys[1].GetPublicKey(); update.payout = ID(3);
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(update, nullptr, tx, fee, reason), reason);
    BOOST_REQUIRE(registry.Check(*tx, *pcoinsTip, 2, checked, reason));
    BOOST_CHECK(checked.payout == ID(3));
    update.operatorKey = keys[4].GetPublicKey();
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(update, nullptr, tx, fee, reason));
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(update, &keys[4], tx, fee, reason), reason);
    BOOST_REQUIRE(registry.Apply(*tx, *pcoinsTip, 2, reason));
    const auto rotation = tx->GetHash();
    failure(update, &keys[4]); // Confirmed sequence has advanced.
    pqmn::Payload service; service.action = pqmn::Action::SERVICE;
    service.registration = registration; service.sequence = 2;
    service.service = op.service; service.operatorPayout = ID(4);
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(service, &keys[1], tx, fee, reason));
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(service, &keys[4], tx, fee, reason), reason);
    BOOST_REQUIRE(registry.Apply(*tx, *pcoinsTip, 2, reason));
    const auto configured = tx->GetHash();
    pqmn::Payload revoke; revoke.action = pqmn::Action::REVOKE;
    revoke.registration = registration; revoke.sequence = 3;
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(revoke, &keys[4], tx, fee, reason), reason);
    BOOST_REQUIRE(registry.Check(*tx, *pcoinsTip, 2, checked, reason));
    BOOST_CHECK(checked.revoked);
    BOOST_CHECK(wallet.GetPQAddresses() == addresses);
    BOOST_CHECK_EQUAL(wallet.mapWallet.size(), walletSize);
    BOOST_CHECK_EQUAL(mempool.size(), 0U);
    CTransactionRef payment;
    CCoinControl paymentControl;
    paymentControl.destPQChange = pq::EncodeAddress(ID(3), Params().NetworkIDString());
    BOOST_REQUIRE(wallet.CreatePQTransaction(paymentControl.destPQChange, COIN, payment, fee, reason, &paymentControl));
    CValidationState state;
    BOOST_REQUIRE_MESSAGE(AcceptToMemoryPool(mempool, state, payment, false, nullptr), state.GetRejectReason());
    failure(revoke, &keys[4]); // Pending spend of the only free fee input.
    mempool.clear();
    BOOST_REQUIRE(registry.Undo(configured, reason));
    BOOST_REQUIRE(registry.Undo(rotation, reason));
    BOOST_REQUIRE(registry.Undo(registration, reason));
    // Two confirmed inputs are required to create the internal bond. No fresh key.
    CMutableTransaction split;
    split.vin.emplace_back(uint256S("8766"), 0);
    split.vout.assign(2, CTxOut(amount / 2 + COIN, pq::GetScript(ID(3))));
    const auto splitRef = MakeTransactionRef(split);
    BOOST_REQUIRE(wallet.AddToWalletIfInvolvingMe(splitRef,
        {CWalletTx::Status::CONFIRMED, 1, chainActive[1]->GetBlockHash(), 0}, true));
    for (uint32_t i = 0; i < 2; ++i) {
        pcoinsTip->AddCoin(COutPoint(splitRef->GetHash(), i), Coin(split.vout[i], 1, false, false), false);
        wallet.LockCoin(COutPoint(sourceRef->GetHash(), i));
    }
    internal.collateral.n = 0;
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(internal, &keys[1], tx, fee, reason), reason);
    BOOST_REQUIRE_EQUAL(tx->vin.size(), 2U);
    BOOST_REQUIRE_EQUAL(tx->vout.size(), 2U);
    BOOST_REQUIRE(registry.Check(*tx, *pcoinsTip, 2, checked, reason));
    BOOST_CHECK_EQUAL(splitRef->GetValueOut() - tx->GetValueOut(), fee);
    BOOST_CHECK(fee > 0 && fee <= maxTxFee);
    BOOST_CHECK(wallet.GetPQAddresses() == addresses);
    selection.SetNull();
    selection.Select(COutPoint(splitRef->GetHash(), 0));
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(internal, &keys[1], tx, fee, reason, &selection));
    BOOST_CHECK(!tx); // One selected half cannot silently spend an unselected half.
    selection.fAllowOtherInputs = true;
    BOOST_REQUIRE_MESSAGE(wallet.CreatePQMasternodeTransaction(internal, &keys[1], tx, fee, reason, &selection), reason);
    BOOST_REQUIRE_EQUAL(tx->vin.size(), 2U);
    BOOST_CHECK(tx->vin[0].prevout == COutPoint(splitRef->GetHash(), 0));
    wallet.LockCoin(COutPoint(splitRef->GetHash(), 1));
    BOOST_CHECK(!wallet.CreatePQMasternodeTransaction(internal, &keys[1], tx, fee, reason));
    BOOST_CHECK(!tx); BOOST_CHECK_EQUAL(fee, 0);
}

BOOST_AUTO_TEST_CASE(wallet_collateral_selection_staking_and_undo)
{
    struct DiskWallet : CWallet {
        using CWallet::CWallet;
        ~DiskWallet() { GetDBHandle().Flush(true); }
    } wallet("pqmn-wallet", WalletDatabase::Create(GetDataDir() / "pqmn-wallet"));
    bool firstRun;
    BOOST_REQUIRE_EQUAL(wallet.LoadWallet(firstRun), DB_LOAD_OK);
    const SecureString passphrase = "pqmn-test-only";
    BOOST_REQUIRE(wallet.EncryptWallet(passphrase));
    BOOST_REQUIRE(wallet.Unlock(passphrase));
    std::string address;
    BOOST_REQUIRE(wallet.GeneratePQAddress(address));
    BOOST_REQUIRE(wallet.GetPQKey(address, keys[2]));
    // Mature synthetic wallet UTXOs on a real isolated chain. Registry mutations
    // below use real role/fee signatures and the real transactional index.
    for (int height = 1; height <= Params().GetConsensus().nStakeMinDepth; ++height) {
        auto candidate = BlockAssembler(Params(), false).CreateNewBlock(pq::GetScript(ID(3)), nullptr, false, nullptr, true);
        BOOST_REQUIRE(candidate);
        auto block = std::make_shared<CBlock>(candidate->block);
        BOOST_REQUIRE(SolveBlock(block, height));
        BOOST_REQUIRE(ProcessNewBlock(block, nullptr));
    }
    LOCK2(cs_main, wallet.cs_wallet);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
    const CAmount amount = Params().GetConsensus().nMNCollateralAmt;
    CMutableTransaction source;
    source.vin.emplace_back(uint256S("9876"), 0);
    source.vout.emplace_back(amount, pq::GetScript(ID(2)));
    source.vout.emplace_back(10 * COIN, pq::GetScript(ID(2)));
    const auto sourceRef = MakeTransactionRef(source);
    const COutPoint held(sourceRef->GetHash(), 0), free(sourceRef->GetHash(), 1);
    wallet.SetLastBlockProcessed(chainActive.Tip());
    BOOST_REQUIRE(wallet.AddToWalletIfInvolvingMe(sourceRef,
        {CWalletTx::Status::CONFIRMED, 1, chainActive[1]->GetBlockHash(), 0}, true));
    for (uint32_t i = 0; i < 2; ++i) {
        Coin coin(source.vout[i], 1, false, false);
        pcoinsTip->AddCoin(COutPoint(sourceRef->GetHash(), i), Coin(coin), false);
        view.AddCoin(COutPoint(sourceRef->GetHash(), i), std::move(coin), false);
    }
    const auto stake = [&] {
        CMutableTransaction tx;
        tx.vin.emplace_back(held);
        tx.vout.emplace_back(0, CScript());
        tx.vout.push_back(source.vout[0]);
        return tx;
    };
    auto before = stake();
    BOOST_REQUIRE(wallet.SignCoinStake(before));
    BOOST_REQUIRE_EQUAL(wallet.GetPQUnspent().size(), 2U);
    BOOST_CHECK_EQUAL(wallet.GetStakingBalance(false), amount + 10 * COIN);
    auto operation = Registration(); operation.collateral = held;
    const CTransaction registration(Transaction(operation));
    pqmn::Index registry(*evoDb, Params()); std::string reason;
    auto transaction = evoDb->BeginTransaction();
    BOOST_REQUIRE_MESSAGE(registry.Apply(registration, view, chainActive.Height(), reason), reason);
    const auto checkProtected = [&] {
        const auto coins = wallet.GetPQUnspent();
        BOOST_CHECK_EQUAL(coins.size(), 1U);
        for (const auto& coin : coins) BOOST_CHECK(COutPoint(coin.tx->GetHash(), coin.i) == free);
        BOOST_CHECK_EQUAL(wallet.GetPQUnspent(true).size(), 2U);
        BOOST_CHECK_EQUAL(interfaces::Wallet(wallet).getBalances().balance, amount + 10 * COIN);
        BOOST_CHECK_EQUAL(wallet.GetStakingBalance(false), 10 * COIN);
        std::vector<CStakeableOutput> stakeable;
        BOOST_REQUIRE(wallet.StakeableCoins(&stakeable));
        BOOST_CHECK_EQUAL(stakeable.size(), 1U);
        for (const auto& coin : stakeable) BOOST_CHECK(COutPoint(coin.tx->GetHash(), coin.i) == free);
        auto blocked = stake();
        BOOST_CHECK(!wallet.SignCoinStake(blocked));
    };
    checkProtected();
    CTransactionRef payment; CAmount fee;
    BOOST_REQUIRE(wallet.CreatePQTransaction(address, COIN, payment, fee, reason));
    BOOST_REQUIRE_EQUAL(payment->vin.size(), 1U);
    BOOST_CHECK(payment->vin[0].prevout == free);
    BOOST_CHECK(!wallet.CreatePQTransaction(address, 11 * COIN, payment, fee, reason));
    CCoinControl control; control.Select(held);
    BOOST_REQUIRE(wallet.CreatePQTransaction(address, COIN, payment, fee, reason, &control));
    BOOST_REQUIRE_EQUAL(payment->vin.size(), 1U);
    BOOST_CHECK(payment->vin[0].prevout == held); // Deliberate collateral spend.
    control.fAllowOtherInputs = true;
    BOOST_REQUIRE(wallet.CreatePQTransaction(address, amount + COIN, payment, fee, reason, &control));
    BOOST_REQUIRE_EQUAL(payment->vin.size(), 2U);
    BOOST_CHECK(payment->vin[0].prevout == held);
    BOOST_CHECK(payment->vin[1].prevout == free);
    BOOST_CHECK(!wallet.CreatePQTransaction({CTxOut(COIN, pq::GetScript(ID(2)))}, pq::GOVERNANCE_PROPOSAL,
        {1}, payment, fee, reason, &control)); // Payment override cannot lock collateral into governance.
    wallet.LockCoin(held);
    BOOST_CHECK(!wallet.CreatePQTransaction(address, COIN, payment, fee, reason, &control));
    wallet.UnlockCoin(held);
    // Staking may hold a candidate list made before registration.
    std::vector<CStakeableOutput> stale;
    const CBlockIndex* sourceBlock = chainActive[1];
    stale.emplace_back(wallet.GetWalletTx(sourceRef->GetHash()), 0, chainActive.Height(), sourceBlock);
    auto blocked = stake(); int64_t time = chainActive.Tip()->GetBlockTime() + 60;
    BOOST_CHECK(!wallet.CreateCoinStake(chainActive.Tip(), chainActive.Tip()->nBits, blocked, time, &stale, false));
    BOOST_CHECK(stale.empty());
    pqmn::Payload revoke; revoke.action = pqmn::Action::REVOKE;
    revoke.registration = registration.GetHash(); revoke.sequence = 1;
    const CTransaction revoked(Transaction(revoke));
    BOOST_REQUIRE(registry.Apply(revoked, view, chainActive.Height(), reason));
    checkProtected(); // Revocation does not release the bond.
    BOOST_REQUIRE(registry.Undo(revoked.GetHash(), reason));
    BOOST_REQUIRE(registry.Undo(registration.GetHash(), reason));
    BOOST_CHECK_EQUAL(wallet.GetPQUnspent().size(), 2U);
    BOOST_CHECK_EQUAL(wallet.GetStakingBalance(false), amount + 10 * COIN);
    auto restored = stake();
    BOOST_CHECK(wallet.SignCoinStake(restored));
    BOOST_CHECK(wallet.ListLockedCoins().empty());
    wallet.LockCoin(held);
    auto manuallyLocked = stake();
    BOOST_CHECK(!wallet.SignCoinStake(manuallyLocked));
    BOOST_REQUIRE(registry.Apply(registration, view, chainActive.Height(), reason));
    BOOST_REQUIRE(registry.Undo(registration.GetHash(), reason));
    BOOST_CHECK(wallet.IsLockedCoin(held.hash, held.n)); // Undo must not undo a user's lock.
    BOOST_CHECK_EQUAL(interfaces::Wallet(wallet).getBalances().balance, amount + 10 * COIN);
    wallet.UnlockCoin(held);
    BOOST_REQUIRE(registry.Apply(registration, view, chainActive.Height(), reason));
    const auto collateralKey = std::make_pair(std::make_pair(std::string("pqmn1c"),
        Params().GetConsensus().hashGenesisBlock), held);
    evoDb->Write(collateralKey, uint256S("dead")); // Reverse entry points at a missing record.
    BOOST_CHECK_THROW(wallet.GetPQUnspent(), std::runtime_error);
    auto corruptStake = stake();
    BOOST_CHECK_THROW(wallet.SignCoinStake(corruptStake), std::runtime_error);
    BOOST_CHECK_THROW(wallet.CreatePQTransaction(address, COIN, payment, fee, reason, &control), std::runtime_error);
    evoDb->Write(collateralKey, registration.GetHash());
    auto missingRegistry = std::move(evoDb);
    BOOST_CHECK_THROW(wallet.GetPQUnspent(), std::runtime_error);
    evoDb = std::move(missingRegistry);
}
#endif

BOOST_AUTO_TEST_CASE(real_block_registration_validation_rollback_and_reorg)
{
    const auto candidate = [&](const std::vector<CTransactionRef>& transactions, CAmount fees = 0) {
        auto blockTemplate = BlockAssembler(Params(), false).CreateNewBlock(pq::GetScript(ID(3)), nullptr, false, nullptr, true);
        BOOST_REQUIRE(blockTemplate);
        auto block = std::make_shared<CBlock>(blockTemplate->block);
        auto reward = CMutableTransaction(*block->vtx[0]); reward.vout[0].nValue += fees;
        block->vtx[0] = MakeTransactionRef(reward);
        block->vtx.insert(block->vtx.end(), transactions.begin(), transactions.end());
        BOOST_REQUIRE(SolveBlock(block, WITH_LOCK(cs_main, return chainActive.Height() + 1)));
        return block;
    };
    COutPoint funding, independentFunding;
    for (int height = 1; height <= 101; ++height) {
        auto block = candidate({}); BOOST_REQUIRE(ProcessNewBlock(block, nullptr));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainActive.Height()), height);
        if (height == 1) funding = COutPoint(block->vtx[0]->GetHash(), 0);
        if (height == 2) independentFunding = COutPoint(block->vtx[0]->GetHash(), 0);
    }
    WAIT_LOCK(cs_main, chainLock);
    const auto parent = chainActive.Tip()->GetBlockHash();
    view.AddCoin(funding, Coin(pcoinsTip->AccessCoin(funding)), false);
    const auto reg = MakeTransactionRef(Transaction(Registration(), 1, true, 0, 2, funding));
    const COutPoint change(reg->GetHash(), 1);
    view.AddCoin(change, Coin(reg->vout[1], 102, false, false), false);
    pqmn::Payload update; update.action = pqmn::Action::UPDATE; update.registration = reg->GetHash();
    update.sequence = 1; update.operatorKey = keys[1].GetPublicKey(); update.payout = ID(5);
    const auto updated = MakeTransactionRef(Transaction(update, 1, false, 0, 2, change));
    auto block = candidate({reg, updated}, 2 * COIN);
    CValidationState inactive;
    BOOST_CHECK(!TestBlockValidity(inactive, *block, chainActive.Tip()));
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 102);
    pqmn::Index registry(*evoDb, Params()); pqmn::Record record;
    auto invalidReward = std::make_shared<CBlock>(*block);
    auto reward = CMutableTransaction(*invalidReward->vtx[0]); ++reward.vout[0].nValue;
    invalidReward->vtx[0] = MakeTransactionRef(reward);
    BOOST_REQUIRE(SolveBlock(invalidReward, 102));
    CValidationState badReward;
    BOOST_CHECK(!TestBlockValidity(badReward, *invalidReward, chainActive.Tip()));
    BOOST_CHECK_EQUAL(badReward.GetRejectReason(), "bad-blk-amount");
    BOOST_CHECK(!registry.Get(reg->GetHash(), record)); BOOST_CHECK(evoDb->VerifyBestBlock(parent));
    auto invalidRole = CMutableTransaction(*reg); pq::Payload envelope; pqmn::Payload operation;
    BOOST_REQUIRE(pq::DecodePayload(invalidRole, envelope)); BOOST_REQUIRE(pqmn::Decode(envelope.data, operation));
    operation.ownerSignature[0] ^= 1; envelope.data = pqmn::Encode(operation); FeeSign(invalidRole, envelope);
    auto invalidBlock = candidate({MakeTransactionRef(invalidRole)}, COIN);
    CValidationState badRole;
    BOOST_CHECK(!TestBlockValidity(badRole, *invalidBlock, chainActive.Tip()));
    BOOST_CHECK_EQUAL(badRole.GetRejectReason(), "bad-pqmn-registration-signature");
    BOOST_CHECK(!registry.Get(invalidRole.GetHash(), record));
    CValidationState state;
    BOOST_REQUIRE_MESSAGE(TestBlockValidity(state, *block, chainActive.Tip()), state.GetRejectReason());
    BOOST_CHECK(!registry.Get(reg->GetHash(), record)); // Validation-only rolls back.
    BOOST_CHECK(evoDb->VerifyBestBlock(parent)); BOOST_CHECK(pcoinsTip->HaveCoin(funding));
    CValidationState poolState;
    BOOST_CHECK_MESSAGE(AcceptToMemoryPool(mempool, poolState, reg, false, nullptr), poolState.GetRejectReason());
    BOOST_CHECK(mempool.exists(reg->GetHash()));
    BOOST_CHECK(!registry.Get(reg->GetHash(), record)); // Admission cannot commit registry state.
    auto assembled = BlockAssembler(Params(), false).CreateNewBlock(pq::GetScript(ID(3)), nullptr, false, nullptr, false);
    BOOST_REQUIRE(assembled);
    BOOST_CHECK(std::any_of(assembled->block.vtx.begin(), assembled->block.vtx.end(),
        [&](const CTransactionRef& tx) { return tx->GetHash() == reg->GetHash(); }));
    BOOST_CHECK(!registry.Get(reg->GetHash(), record)); // Template validation also rolls back.
    CValidationState chainedState;
    BOOST_CHECK(!AcceptToMemoryPool(mempool, chainedState, updated, false, nullptr));
    mempool.clear();
    { REVERSE_LOCK(chainLock); BOOST_REQUIRE(ProcessNewBlock(block, nullptr)); }
    BOOST_REQUIRE_EQUAL(chainActive.Tip()->GetBlockHash(), block->GetHash());
    BOOST_REQUIRE(registry.Get(reg->GetHash(), record));
    BOOST_CHECK(record.collateral == COutPoint(reg->GetHash(), 0));
    BOOST_CHECK_EQUAL(record.sequence, 1U); BOOST_CHECK(record.payout == ID(5));
    BOOST_CHECK(!pcoinsTip->HaveCoin(funding));
    FlushStateToDisk();
    BOOST_REQUIRE(registry.Get(reg->GetHash(), record));
    BOOST_CHECK(CVerifyDB().VerifyDB(pcoinsTip.get(), 4, 2));
    BOOST_REQUIRE(registry.Get(reg->GetHash(), record)); // VerifyDB rolls back too.
    CValidationState undo;
    BOOST_REQUIRE(InvalidateBlock(undo, Params(), chainActive.Tip()));
    BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash(), parent);
    BOOST_CHECK(!registry.Get(reg->GetHash(), record)); BOOST_CHECK(pcoinsTip->HaveCoin(funding));
    CValidationState reconnect;
    BOOST_REQUIRE(ReconsiderBlock(reconnect, LookupBlockIndex(block->GetHash())));
    { REVERSE_LOCK(chainLock); BOOST_REQUIRE(ActivateBestChain(reconnect)); }
    BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash(), block->GetHash());
    BOOST_REQUIRE(registry.Get(reg->GetHash(), record));
    // A deliberately mined collateral spend removes the registration; undo restores it.
    CMutableTransaction spend; spend.nVersion = 3; spend.nType = CTransaction::PQ; spend.sapData = nullopt;
    spend.vin.emplace_back(record.collateral);
    spend.vout.emplace_back(reg->vout[0].nValue - COIN, pq::GetScript(ID(3)));
    view.AddCoin(record.collateral, Coin(reg->vout[0], 102, false, false), false);
    pq::Payload transfer; transfer.authorizations.resize(1); transfer.authorizations[0].public_key = keys[2].GetPublicKey();
    FeeSign(spend, transfer, 2);
    auto spentBlock = candidate({MakeTransactionRef(spend)}, COIN);
    { REVERSE_LOCK(chainLock); BOOST_REQUIRE(ProcessNewBlock(spentBlock, nullptr)); }
    BOOST_CHECK_EQUAL(chainActive.Tip()->GetBlockHash(), spentBlock->GetHash());
    BOOST_CHECK(!registry.Get(reg->GetHash(), record));
    CValidationState restore;
    BOOST_REQUIRE(InvalidateBlock(restore, Params(), chainActive.Tip()));
    BOOST_REQUIRE(registry.Get(reg->GetHash(), record)); BOOST_CHECK_EQUAL(record.sequence, 1U);
    // A competing mined owner update invalidates a pending update and its ordinary child.
    // Remove the resurrected collateral spend first, so it does not conflict with the update.
    mempool.clear();
    view.AddCoin(independentFunding, Coin(pcoinsTip->AccessCoin(independentFunding)), false);
    update.sequence = 2; update.payout = ID(6);
    const auto pendingUpdate = MakeTransactionRef(Transaction(update, 1, false, 0, 2, independentFunding));
    CValidationState pendingState;
    BOOST_REQUIRE_MESSAGE(AcceptToMemoryPool(mempool, pendingState, pendingUpdate, false, nullptr), pendingState.GetRejectReason());
    const COutPoint pendingChange(pendingUpdate->GetHash(), 0);
    view.AddCoin(pendingChange, Coin(pendingUpdate->vout[0], MEMPOOL_HEIGHT, false, false), false);
    CMutableTransaction child = spend;
    child.vin[0].prevout = pendingChange;
    child.vout[0].nValue = pendingUpdate->vout[0].nValue - COIN;
    transfer.authorizations[0].public_key = keys[3].GetPublicKey();
    FeeSign(child, transfer, 3);
    const auto childRef = MakeTransactionRef(child);
    CValidationState childState;
    BOOST_REQUIRE_MESSAGE(AcceptToMemoryPool(mempool, childState, childRef, false, nullptr), childState.GetRejectReason());
    const COutPoint confirmedChange(updated->GetHash(), 0);
    view.AddCoin(confirmedChange, Coin(pcoinsTip->AccessCoin(confirmedChange)), false);
    update.payout = ID(7);
    const auto competing = MakeTransactionRef(Transaction(update, 1, false, 0, 2, confirmedChange));
    auto competingBlock = candidate({competing}, COIN);
    { REVERSE_LOCK(chainLock); BOOST_REQUIRE(ProcessNewBlock(competingBlock, nullptr)); }
    BOOST_CHECK(!mempool.exists(pendingUpdate->GetHash()));
    BOOST_CHECK(!mempool.exists(childRef->GetHash()));
    BOOST_REQUIRE(registry.Get(reg->GetHash(), record)); BOOST_CHECK_EQUAL(record.sequence, 2U);
    CValidationState back;
    BOOST_REQUIRE(InvalidateBlock(back, Params(), chainActive.Tip()));
    BOOST_REQUIRE(registry.Get(reg->GetHash(), record)); BOOST_CHECK_EQUAL(record.sequence, 1U);
    BOOST_CHECK(mempool.exists(competing->GetHash()));
    mempool.clear();
    CValidationState reaccepted;
    BOOST_REQUIRE_MESSAGE(AcceptToMemoryPool(mempool, reaccepted, pendingUpdate, false, nullptr), reaccepted.GetRejectReason());
    CValidationState belowRegistration;
    BOOST_REQUIRE(InvalidateBlock(belowRegistration, Params(), chainActive.Tip()));
    BOOST_CHECK_EQUAL(chainActive.Height(), 101);
    BOOST_CHECK(!mempool.exists(pendingUpdate->GetHash()));
    BOOST_CHECK(!registry.Get(reg->GetHash(), record));
    BOOST_CHECK(mempool.exists(reg->GetHash())); // The registration itself can be relayed again.
    BOOST_CHECK(mempool.IsPQMNCollateral(COutPoint(reg->GetHash(), 0)));
}

BOOST_AUTO_TEST_CASE(pq_reward_queue_selects_oldest_configured_mature_record)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    const auto make_record = [&](unsigned int n, uint32_t lastPaid) {
        pqmn::Record record;
        record.collateral = COutPoint(uint256S("aa"), n);
        record.owner = keys[0].GetPublicKey();
        record.operatorKey = keys[1].GetPublicKey();
        record.collateralKey = ID(2);
        record.payout = ID(n);
        BOOST_REQUIRE(Lookup((std::string("127.0.0.1:") + std::to_string(52000 + n)).c_str(), record.service, 0, false));
        record.registeredHeight = 1;
        record.collateralHeight = 1;
        record.lastPaidHeight = lastPaid;
        return record;
    };
    const std::array<uint256, 4> registrations{uint256S("01"), uint256S("02"), uint256S("03"), uint256S("04")};
    for (size_t n = 0; n < registrations.size(); ++n)
        evoDb->Write(std::make_pair(std::make_pair(std::string("pqmn1r"), Params().GetConsensus().hashGenesisBlock), registrations[n]),
                     make_record(n, n == 0 ? 0 : 10));

    uint256 registration;
    pqmn::Record winner;
    BOOST_REQUIRE(index.GetPayee(20, registration, winner));
    BOOST_CHECK(registration == registrations[0]);
    BOOST_CHECK(winner.payout == ID(0));

    winner.revivedHeight = 30;
    evoDb->Write(std::make_pair(std::make_pair(std::string("pqmn1r"), Params().GetConsensus().hashGenesisBlock), registrations[0]), winner);
    BOOST_REQUIRE(index.GetPayee(20, registration, winner));
    BOOST_CHECK(registration == registrations[1]);

    winner.service = {};
    evoDb->Write(std::make_pair(std::make_pair(std::string("pqmn1r"), Params().GetConsensus().hashGenesisBlock), registrations[0]), winner);
    BOOST_REQUIRE(index.GetPayee(20, registration, winner));
    BOOST_CHECK(registration == registrations[1]);
    winner = make_record(2, 10);
    winner.revoked = true;
    evoDb->Write(std::make_pair(std::make_pair(std::string("pqmn1r"), Params().GetConsensus().hashGenesisBlock), registrations[2]), winner);
    winner = make_record(3, 10);
    winner.registeredHeight = 21;
    evoDb->Write(std::make_pair(std::make_pair(std::string("pqmn1r"), Params().GetConsensus().hashGenesisBlock), registrations[3]), winner);
    BOOST_REQUIRE(index.GetPayee(20, registration, winner));
    BOOST_CHECK(registration == registrations[1]);
}

BOOST_AUTO_TEST_CASE(pq_reward_connect_disconnect_restores_queue_state_and_requires_undo)
{
    LOCK(cs_main);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto setup = evoDb->BeginTransaction(); setup->Commit();
    const auto reg = MakeTransactionRef(Transaction(Registration()));
    RegistryBlock first(parent, {reg});
    pqmn::Index index(*evoDb, Params()); std::string reason;
    {
        auto tx = evoDb->BeginTransaction();
        BOOST_REQUIRE(index.ConnectBlock(first.block, first.index, view, 20, reason));
        evoDb->WriteBestBlock(first.hash); tx->Commit();
    }
    view.SetBestBlock(first.hash);
    RegistryBlock paid(first.index, {});
    {
        auto tx = evoDb->BeginTransaction();
        BOOST_REQUIRE(index.ConnectBlock(paid.block, paid.index, view, 20, reason, COIN));
        pqmn::Record record; BOOST_REQUIRE(index.Get(reg->GetHash(), record));
        BOOST_CHECK_EQUAL(record.lastPaidHeight, 21U);
        evoDb->WriteBestBlock(paid.hash); tx->Commit();
    }
    view.SetBestBlock(paid.hash);
    {
        auto tx = evoDb->BeginTransaction();
        BOOST_REQUIRE(index.DisconnectBlock(paid.block, paid.index, view, 20, reason));
        pqmn::Record record; BOOST_REQUIRE(index.Get(reg->GetHash(), record));
        BOOST_CHECK_EQUAL(record.lastPaidHeight, 0U);
        tx->Rollback();
    }
    evoDb->WriteBestBlock(paid.hash);
    view.SetBestBlock(paid.hash);
    auto missing = evoDb->BeginTransaction();
    evoDb->Erase(std::make_pair(std::make_pair(std::string("pqmn1w"), Params().GetConsensus().hashGenesisBlock), paid.hash));
    BOOST_CHECK(!index.DisconnectBlock(paid.block, paid.index, view, 20, reason));
    missing->Rollback();
}

BOOST_AUTO_TEST_CASE(pq_reward_queue_rejects_corrupt_payout_fields)
{
    LOCK(cs_main);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    const uint256 registration = uint256S("99");
    const auto key = std::make_pair(std::make_pair(std::string("pqmn1r"), Params().GetConsensus().hashGenesisBlock), registration);
    const auto valid = [&] {
        pqmn::Record record;
        record.collateral = COutPoint(uint256S("bb"), 0);
        record.owner = keys[0].GetPublicKey(); record.operatorKey = keys[1].GetPublicKey();
        record.collateralKey = ID(2); record.payout = ID(0); record.operatorReward = 0;
        record.registeredHeight = 1; record.collateralHeight = 1;
        BOOST_REQUIRE(Lookup("127.0.0.1:53000", record.service, 0, false));
        return record;
    };
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto record = valid();
        if (mutation == 0) record.payout = {};
        if (mutation == 1) record.operatorReward = 10001;
        if (mutation == 2) { record.operatorReward = 1; record.operatorPayout = {}; }
        if (mutation == 3) { record.operatorReward = 0; record.operatorPayout = ID(1); }
        evoDb->Write(key, record);
        uint256 selected; pqmn::Record selectedRecord;
        BOOST_CHECK_THROW(index.GetPayee(20, selected, selectedRecord), std::runtime_error);
        evoDb->Erase(key);
    }
    auto inactive = valid();
    inactive.payout = ID(1); inactive.operatorReward = 1; inactive.operatorPayout = {};
    inactive.service = {}; evoDb->Write(key, inactive);
    uint256 selected; pqmn::Record selectedRecord;
    BOOST_CHECK(!index.GetPayee(20, selected, selectedRecord));
    inactive.revoked = true; evoDb->Write(key, inactive);
    BOOST_CHECK(!index.GetPayee(20, selected, selectedRecord));
}

BOOST_AUTO_TEST_CASE(service_heartbeat_carrier_updates_and_undo_restores_activity)
{
    LOCK(cs_main);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 20);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_SERVICE, 21);
    const uint256 parentHash = uint256S("1234"); CBlockIndex parent;
    parent.phashBlock = &parentHash; parent.nHeight = 19;
    view.SetBestBlock(parentHash); evoDb->WriteBestBlock(parentHash);
    auto setup = evoDb->BeginTransaction(); setup->Commit();
    const auto reg = MakeTransactionRef(Transaction(Registration()));
    RegistryBlock first(parent, {reg});
    pqmn::Index index(*evoDb, Params()); std::string reason;
    {
        auto tx = evoDb->BeginTransaction();
        BOOST_REQUIRE(index.ConnectBlock(first.block, first.index, view, 20, reason));
        evoDb->WriteBestBlock(first.hash); tx->Commit();
    }
    view.SetBestBlock(first.hash);
    pqservice::Heartbeat heartbeat;
    BOOST_REQUIRE(index.MatchesChainTip(&first.index));
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_SERVICE, 22);
    BOOST_CHECK(!index.MatchesChainTip(&first.index));
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_SERVICE, 21);
    heartbeat.registration = reg->GetHash(); heartbeat.height = 20; heartbeat.blockHash = first.hash;
    std::vector<unsigned char> sig;
    BOOST_REQUIRE(keys[1].Sign(pqservice::Message(heartbeat, Params().GetConsensus().hashGenesisBlock,
                              keys[1].GetPublicKey()), pqservice::Context(), sig));
    std::copy(sig.begin(), sig.end(), heartbeat.signature.begin());
    BOOST_REQUIRE_MESSAGE(pqservice::Verify(heartbeat, index, &first.index, Params(), reason), reason);
    for (int mutation = 0; mutation < 6; ++mutation) {
        auto bad = heartbeat;
        if (mutation == 0) bad.signature[0] ^= 1;
        if (mutation == 1) bad.sequence++;
        if (mutation == 2) bad.height++;
        if (mutation == 3) bad.blockHash = parentHash;
        if (mutation == 4) bad.registration = parentHash;
        if (mutation == 5) {
            BOOST_REQUIRE(keys[1].Sign(pqservice::Message(bad, parentHash, keys[1].GetPublicKey()),
                                      pqservice::Context(), sig));
            std::copy(sig.begin(), sig.end(), bad.signature.begin());
        }
        BOOST_CHECK(!pqservice::Verify(bad, index, &first.index, Params(), reason));
    }
    RegistryBlock carrier(first.index, {});
    CMutableTransaction coinbase(*carrier.block.vtx[0]);
    pq::Payload payload; payload.mode = pq::SERVICE;
    payload.data = pqservice::Encode({{}, {heartbeat}});
    coinbase.nVersion = 3; coinbase.nType = CTransaction::PQ; coinbase.sapData = nullopt;
    coinbase.extraPayload = pq::EncodePayload(payload);
    carrier.block.vtx[0] = MakeTransactionRef(coinbase);
    carrier.block.hashMerkleRoot = BlockMerkleRoot(carrier.block);
    carrier.hash = carrier.block.GetHash();
    {
        auto tx = evoDb->BeginTransaction();
        BOOST_REQUIRE_MESSAGE(index.ConnectBlock(carrier.block, carrier.index, view, 20, reason, COIN), reason);
        pqmn::Record record; BOOST_REQUIRE(index.Get(reg->GetHash(), record));
        BOOST_CHECK_EQUAL(record.lastHeartbeatHeight, 20U);
        BOOST_CHECK(!pqservice::Verify(heartbeat, index, &carrier.index, Params(), reason));
        evoDb->WriteBestBlock(carrier.hash); tx->Commit();
    }
    view.SetBestBlock(carrier.hash);
    auto tx = evoDb->BeginTransaction();
    BOOST_REQUIRE_MESSAGE(index.DisconnectBlock(carrier.block, carrier.index, view, 20, reason), reason);
    pqmn::Record record; BOOST_REQUIRE(index.Get(reg->GetHash(), record));
    BOOST_CHECK_EQUAL(record.lastHeartbeatHeight, 0U);
    BOOST_CHECK_EQUAL(record.lastPaidHeight, 0U);
}

BOOST_AUTO_TEST_CASE(service_heartbeat_reward_expiry_grace_and_network_absence)
{
    LOCK(cs_main);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_SERVICE, 21);
    auto transaction = evoDb->BeginTransaction();
    pqmn::Index index(*evoDb, Params());
    const auto key = [&](const uint256& id) {
        return std::make_pair(std::make_pair(std::string("pqmn1r"), Params().GetConsensus().hashGenesisBlock), id);
    };
    pqmn::Record first;
    first.owner = keys[0].GetPublicKey(); first.operatorKey = keys[1].GetPublicKey();
    first.payout = ID(0); first.collateralKey = ID(2);
    first.collateral = collateral; first.service = Registration().service;
    first.registeredHeight = first.collateralHeight = 1;
    auto second = first;
    second.lastHeartbeatHeight = 60;
    const auto older = uint256S("01"), active = uint256S("02");
    evoDb->Write(key(older), first); evoDb->Write(key(active), second);
    uint256 selected; pqmn::Record record;
    BOOST_REQUIRE(index.GetPayee(70, selected, record));
    BOOST_CHECK(selected == active); // No bootstrap or finality certificate needed.
    const uint32_t expiry = 60 + Params().GetConsensus().nPQServiceWindow;
    BOOST_REQUIRE(index.GetPayee(expiry, selected, record));
    BOOST_CHECK(selected == active);
    BOOST_REQUIRE(index.GetPayee(expiry + 1, selected, record));
    BOOST_CHECK(selected == older); // Network-wide evidence absence cannot halt rewards.
    BOOST_REQUIRE(index.GetPayee(21, selected, record));
    BOOST_CHECK(selected == older); // Activation grace, no retroactive punishment.
    first.registeredHeight = 65; evoDb->Write(key(older), first);
    second.lastPaidHeight = 69; evoDb->Write(key(active), second);
    BOOST_REQUIRE(index.GetPayee(70, selected, record));
    BOOST_CHECK(selected == older); // Fresh registration has time to publish.
}

BOOST_AUTO_TEST_SUITE_END()
