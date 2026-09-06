// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <test/test_organiclife.h>
#include <evo/pqmasternode.h>
#include <chainparams.h>
#include <coins.h>
#include <consensus/merkle.h>
#include <netbase.h>
#include <validation.h>
#include <txdb.h>
#include <blockassembler.h>
#include <boost/test/unit_test.hpp>

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

BOOST_AUTO_TEST_CASE(reserved_envelope_parses_but_is_disabled_by_default)
{
    pq::Payload payload; payload.mode = pq::MASTERNODE; payload.authorizations.resize(1); payload.data = {1};
    BOOST_CHECK(!pq::EncodePayload(payload).empty());
    auto tx = Transaction(Registration());
    std::string reason;
    BOOST_CHECK(pq::CheckStructure(tx, Params(), reason));
    for (const std::string network : {"main", "test", "regtest"}) {
        const auto params = CreateChainParams(network);
        for (int height : {0, 1, 100, 1000000})
            BOOST_CHECK(!pq::CheckContext(tx, *params, height, reason));
    }
    BOOST_CHECK_EQUAL(pq::GetSigOpCost(tx), 4 * pq::SIGOP_COST);
}

BOOST_AUTO_TEST_CASE(registry_context_requires_explicit_regtest_activation_after_payments)
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
    for (const std::string network : {"main", "test"}) {
        auto params = CreateChainParams(network);
        params->UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 20);
        BOOST_CHECK(!pq::MasternodesActive(*params, 20));
        BOOST_CHECK(!pq::CheckContext(tx, *params, 20, reason));
    }
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
    auto main = CreateChainParams(CBaseChainParams::MAIN); pqmn::Index mainIndex(*evoDb, *main);
    BOOST_CHECK(!mainIndex.ConnectBlock(block.block, block.index, view, 20, reason));
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
    evoDb->Write(activation, 2); evoDb->Write(marker, secondHash);
    BOOST_CHECK(ReplayBlocks(Params(), &database, requiresReindex)); BOOST_CHECK(!requiresReindex);
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
    COutPoint funding;
    for (int height = 1; height <= 101; ++height) {
        auto block = candidate({}); BOOST_REQUIRE(ProcessNewBlock(block, nullptr));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainActive.Height()), height);
        if (height == 1) funding = COutPoint(block->vtx[0]->GetHash(), 0);
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
    BOOST_CHECK(!AcceptToMemoryPool(mempool, poolState, reg, false, nullptr));
    BOOST_CHECK_EQUAL(poolState.GetRejectReason(), "pq-masternode-relay-disabled");
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
}

BOOST_AUTO_TEST_SUITE_END()
