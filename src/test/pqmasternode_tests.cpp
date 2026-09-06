// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <test/test_organiclife.h>
#include <evo/pqmasternode.h>
#include <chainparams.h>
#include <coins.h>
#include <netbase.h>
#include <validation.h>
#include <boost/test/unit_test.hpp>

namespace {
struct MNSetup : BasicTestingSetup {
    std::array<mldsa44::Key, 9> keys;
    CCoinsView base;
    CCoinsViewCache view{&base};
    uint32_t nextInput{1};
    const COutPoint collateral{uint256S("aa"), 0};
    MNSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {
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
                                    size_t ownerKey = 0, size_t collateralKey = 2) {
        CMutableTransaction tx;
        tx.nVersion = 3; tx.nType = CTransaction::PQ; tx.sapData = nullopt;
        tx.vin.emplace_back(COutPoint(uint256S("bb"), nextInput++));
        const auto amount = Params().GetConsensus().nMNCollateralAmt;
        view.AddCoin(tx.vin[0].prevout, Coin(CTxOut(amount + 10 * COIN, pq::GetScript(ID(3))), 1, false, false), false);
        if (internal) {
            op.collateral = COutPoint(uint256(), 0);
            tx.vout.emplace_back(amount, pq::GetScript(ID(2)));
            tx.vout.emplace_back(9 * COIN, pq::GetScript(ID(3)));
        } else tx.vout.emplace_back(amount + 9 * COIN, pq::GetScript(ID(3)));
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
}

BOOST_FIXTURE_TEST_SUITE(pqmasternode_tests, MNSetup)

BOOST_AUTO_TEST_CASE(reserved_envelope_parses_but_never_activates)
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

BOOST_AUTO_TEST_SUITE_END()
