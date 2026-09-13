// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <test/test_organiclife.h>
#include <pqtransaction.h>
#include <pqservice.h>
#include <chainparams.h>
#include <coins.h>
#include <blockassembler.h>
#include <validation.h>
#include <evo/specialtx_validation.h>
#include <llmq/quorums_blockprocessor.h>
#include <consensus/tx_verify.h>
#include <consensus/upgrades.h>
#include <crypto/sha256.h>
#include <policy/policy.h>
#include <script/interpreter.h>
#include <script/standard.h>
#include <test/data/mldsa44_vectors.h>
#include <utilstrencodings.h>
#include <boost/test/unit_test.hpp>
#include <chrono>

namespace {
struct PQSetup : BasicTestingSetup { PQSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {} };
// A trusted-view height independent of the live tip, without creating a chain.
struct PQViewHeight {
    CBlockIndex previous;
    const uint256 hash{uint256S("abcdef")};
    explicit PQViewHeight(CCoinsViewCache& view, int height) {
        previous.nHeight = height - 1;
        BOOST_REQUIRE(mapBlockIndex.emplace(hash, &previous).second);
        view.SetBestBlock(hash);
    }
    ~PQViewHeight() { mapBlockIndex.erase(hash); }
};
CScript LegacyScript() { return CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 7) << OP_EQUALVERIFY << OP_CHECKSIG; }
CMutableTransaction Transfer(const pq::KeyID& id, const pq::Payload& payload)
{
    CMutableTransaction tx;
    tx.nVersion = 3; tx.nType = CTransaction::PQ; tx.sapData = nullopt;
    tx.vin.emplace_back(COutPoint(uint256S("01"), 2), CScript(), 3);
    tx.vout.emplace_back(90, pq::GetScript(id));
    tx.nLockTime = 4;
    tx.extraPayload = pq::EncodePayload(payload);
    return tx;
}

void CheckNetworkBoundInputs(const std::string& other_network)
{
    mldsa44::Key key; BOOST_REQUIRE(key.Generate());
    const auto id = *pq::GetID(key.GetPublicKey(), Params().NetworkIDString());
    pq::Payload payload; payload.authorizations.resize(1);
    payload.authorizations[0].public_key = key.GetPublicKey();
    auto tx = Transfer(id, payload);
    const std::vector<CTxOut> prev{CTxOut(100, pq::GetScript(id))};
    const auto other_params = CreateChainParams(other_network);
    LOCK(cs_main);
    for (int mutation = 0; mutation < 4; ++mutation) {
        auto spent = prev;
        if (mutation == 3) spent[0].scriptPubKey = pq::GetScript(*pq::GetID(key.GetPublicKey(), other_network));
        CCoinsViewCache view(pcoinsTip.get()); PQViewHeight previous(view, 20);
        view.AddCoin(tx.vin[0].prevout, Coin(spent[0], 20, false, false), false);
        const auto& genesis = (mutation == 2 ? *other_params : Params()).GetConsensus().hashGenesisBlock;
        const auto context = pq::SignatureContext(mutation == 1 ? other_network : Params().NetworkIDString());
        BOOST_REQUIRE(context);
        const auto message = pq::SignatureMessage(tx, spent, payload, genesis, 0);
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(key.Sign(message, *context, signature));
        // The wrong commitment alone fails: this signature is valid for the
        // full trusted prevout under the current network's context and genesis.
        if (mutation == 3) BOOST_REQUIRE(mldsa44::Verify(key.GetPublicKey(), message, *context, signature));
        std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
        tx.extraPayload = pq::EncodePayload(payload);
        const CTransaction candidate(tx);
        for (bool scripts : {false, true}) {
            CValidationState state; PrecomputedTransactionData data(candidate);
            BOOST_CHECK_EQUAL(CheckInputs(candidate, state, view, scripts, MANDATORY_SCRIPT_VERIFY_FLAGS, true, data), mutation == 0);
            if (mutation) BOOST_CHECK_EQUAL(state.GetRejectReason(), mutation == 3 ? "bad-pq-key" : "bad-pq-signature");
        }
    }
}
}

BOOST_FIXTURE_TEST_SUITE(pqtransaction_tests, PQSetup)

BOOST_AUTO_TEST_CASE(mainnet_signature_contexts_are_separate)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    const std::vector<unsigned char> message{1, 2, 3};
    for (const auto context : {pq::SignatureContext, pq::BlockSignatureContext, pq::GovernanceSignatureContext}) {
        const auto main = context("main");
        const auto test = context("test");
        const auto regtest = context("regtest");
        BOOST_REQUIRE(main);
        BOOST_REQUIRE(test);
        BOOST_REQUIRE(regtest);
        const std::string value(main->begin(), main->end());
        BOOST_CHECK(value != std::string(test->begin(), test->end()));
        BOOST_CHECK(value != std::string(regtest->begin(), regtest->end()));
        BOOST_CHECK(!context("unknown"));
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(key.Sign(message, *main, signature));
        BOOST_CHECK(mldsa44::Verify(key.GetPublicKey(), message, *main, signature));
        BOOST_CHECK(!mldsa44::Verify(key.GetPublicKey(), message, *test, signature));
        BOOST_CHECK(!mldsa44::Verify(key.GetPublicKey(), message, *regtest, signature));
    }
    const auto tx = *pq::SignatureContext("main");
    const auto block = *pq::BlockSignatureContext("main");
    const auto governance = *pq::GovernanceSignatureContext("main");
    BOOST_CHECK(std::string(tx.begin(), tx.end()) != std::string(block.begin(), block.end()));
    BOOST_CHECK(std::string(tx.begin(), tx.end()) != std::string(governance.begin(), governance.end()));
    BOOST_CHECK(std::string(block.begin(), block.end()) != std::string(governance.begin(), governance.end()));
}

BOOST_AUTO_TEST_CASE(output_and_payload_formats)
{
    pq::KeyID id{}; id.fill(0x42);
    const CScript script = pq::GetScript(id);
    BOOST_CHECK_EQUAL(HexStr(script), "ff51204242424242424242424242424242424242424242424242424242424242424242");
    BOOST_CHECK(pq::HasMarker(script));
    BOOST_CHECK(!script.IsUnspendable());
    pq::KeyID decoded{};
    BOOST_REQUIRE(pq::ExtractID(script, decoded));
    BOOST_CHECK(id == decoded);
    for (const CScript& bad : {CScript(), CScript(script.begin(), script.end()-1), CScript(script) << OP_0, CScript() << OP_INVALIDOPCODE << OP_2 << std::vector<unsigned char>(32)}) {
        decoded.fill(42);
        BOOST_CHECK(!pq::ExtractID(bad, decoded));
        BOOST_CHECK(decoded == pq::KeyID{});
    }
    BOOST_CHECK(!VerifyScript({}, script, SCRIPT_VERIFY_NONE, BaseSignatureChecker(), SIGVERSION_BASE));
    for (size_t count : {size_t(1), size_t(2)}) {
        pq::Payload payload;
        payload.authorizations.resize(count);
        const auto bytes = pq::EncodePayload(payload);
        BOOST_REQUIRE_EQUAL(bytes.size(), 3 + pq::AUTH_SIZE * count);
        BOOST_CHECK_EQUAL(bytes[0], 1); BOOST_CHECK_EQUAL(bytes[1], payload.mode); BOOST_CHECK_EQUAL(bytes[2], count);
        auto tx = Transfer(id, payload);
        pq::Payload read;
        BOOST_REQUIRE(pq::DecodePayload(tx, read));
        BOOST_CHECK(pq::EncodePayload(read) == bytes);
        tx.extraPayload=std::vector<unsigned char>{};
        BOOST_CHECK(!pq::DecodePayload(tx,read));
        BOOST_CHECK(read.authorizations.empty());
        for (int mutation=0; mutation<5; ++mutation) {
            auto bad = bytes;
            if (mutation==0) bad[0]=2;
            if (mutation==1) bad[1]=3;
            if (mutation==2) bad[2]=3;
            if (mutation==3) bad.pop_back();
            if (mutation==4) bad.push_back(0);
            tx.extraPayload=bad;
            BOOST_CHECK(!pq::DecodePayload(tx, read));
            BOOST_CHECK(read.authorizations.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(unsupported_modes_are_not_encodable)
{
    pq::Payload payload;
    payload.mode = 0;
    BOOST_CHECK(pq::EncodePayload(payload).empty());

    payload.mode = 3;
    payload.authorizations.resize(1);
    BOOST_CHECK(pq::EncodePayload(payload).empty());
}

BOOST_AUTO_TEST_CASE(governance_payload_is_bounded_and_signed)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    const auto id = *pq::GetID(key.GetPublicKey(), "regtest");
    pq::Payload payload;
    payload.mode = pq::GOVERNANCE_LOCK;
    payload.authorizations.resize(1);
    payload.authorizations[0].public_key = key.GetPublicKey();
    payload.data = {0x01, 0x02, 0x03};
    auto tx = Transfer(id, payload);
    const std::vector<CTxOut> prevouts{CTxOut(100, pq::GetScript(id))};
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(pq::SignatureMessage(tx, prevouts, payload,
                           Params().GetConsensus().hashGenesisBlock, 0),
                           *pq::SignatureContext("regtest"), signature));
    std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
    tx.extraPayload = pq::EncodePayload(payload);
    std::string reason;
    BOOST_REQUIRE(pq::VerifyInputs(tx, prevouts, Params(), reason));

    pq::Payload decoded;
    BOOST_REQUIRE(pq::DecodePayload(tx, decoded));
    BOOST_CHECK(decoded.data == payload.data);
    decoded.data[0] ^= 1;
    tx.extraPayload = pq::EncodePayload(decoded);
    BOOST_CHECK(!pq::VerifyInputs(tx, prevouts, Params(), reason));
    BOOST_CHECK_EQUAL(reason, "bad-pq-signature");

    payload.data.assign(pq::MAX_GOVERNANCE_DATA_SIZE + 1, 0x42);
    BOOST_CHECK(pq::EncodePayload(payload).empty());
}

BOOST_AUTO_TEST_CASE(pq_stake_has_one_owner_and_no_script_signature)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.Generate());
    const auto id = *pq::GetID(key.GetPublicKey(), "regtest");
    pq::Payload payload;
    payload.mode = pq::STAKE;
    payload.authorizations.resize(1);
    payload.authorizations[0].public_key = key.GetPublicKey();
    auto stake = Transfer(id, payload);
    stake.vout.insert(stake.vout.begin(), CTxOut(0, CScript()));
    BOOST_REQUIRE(CTransaction(stake).IsCoinStake());
    const std::vector<CTxOut> prevouts{CTxOut(100, pq::GetScript(id))};
    const auto message = pq::SignatureMessage(stake, prevouts, payload,
        Params().GetConsensus().hashGenesisBlock, 0);
    BOOST_REQUIRE(!message.empty());
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(message, *pq::SignatureContext("regtest"), signature));
    std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
    stake.extraPayload = pq::EncodePayload(payload);
    std::string reason;
    BOOST_CHECK(pq::CheckStructure(stake, Params(), reason));
    BOOST_CHECK(pq::VerifyInputs(stake, prevouts, Params(), reason));

    stake.vout.emplace_back(10, pq::GetScript(id));
    const CTransaction stake_with_payment(stake);
    signature.clear();
    BOOST_REQUIRE(key.Sign(pq::SignatureMessage(stake_with_payment, prevouts, payload,
                           Params().GetConsensus().hashGenesisBlock, 0),
                           *pq::SignatureContext("regtest"), signature));
    std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
    stake.extraPayload = pq::EncodePayload(payload);
    BOOST_CHECK(pq::VerifyInputs(stake, prevouts, Params(), reason));

    auto invalid = stake;
    invalid.vin[0].scriptSig << OP_1;
    BOOST_CHECK(!pq::CheckStructure(invalid, Params(), reason));
    invalid = stake;
    invalid.vout[1].scriptPubKey = LegacyScript();
    BOOST_CHECK(!pq::CheckStructure(invalid, Params(), reason));
}

BOOST_AUTO_TEST_CASE(structure_network_and_legacy_boundaries)
{
    pq::KeyID id{};
    pq::Payload p; p.mode=pq::TRANSFER; p.authorizations.resize(1);
    auto tx=Transfer(id,p);
    std::string why;
    BOOST_REQUIRE(pq::CheckStructure(tx,Params(),why));
    BOOST_CHECK(pq::CheckStructure(tx,*CreateChainParams(CBaseChainParams::MAIN),why));
    BOOST_CHECK(pq::CheckStructure(tx,*CreateChainParams(CBaseChainParams::TESTNET),why));
    for (int mutation=0; mutation<8; ++mutation) {
        auto bad=tx;
        if(mutation==0) bad.nVersion=1;
        if(mutation==1) bad.sapData=SaplingTxData();
        if(mutation==2) bad.vin[0].scriptSig << OP_0;
        if(mutation==3) bad.vin.clear();
        if(mutation==4) bad.vout.clear();
        if(mutation==5) bad.vout.resize(3,bad.vout[0]);
        if(mutation==6) bad.vout[0].scriptPubKey=LegacyScript();
        if(mutation==7) bad.vout[0].scriptPubKey << OP_0;
        BOOST_CHECK(!pq::CheckStructure(bad,Params(),why));
    }
    auto ordinary = tx;
    ordinary.nType = CTransaction::NORMAL;
    ordinary.extraPayload = nullopt;
    BOOST_CHECK(pq::CheckStructure(ordinary, Params(), why));
    BOOST_CHECK(!pq::CheckContext(ordinary, Params(), 1, why));
    BOOST_CHECK_EQUAL(why, "bad-pq-only-transaction");
}

BOOST_AUTO_TEST_CASE(signature_binding_and_rejection)
{
    mldsa44::Key key;
    auto seed=ParseHex(mldsa44_vectors::KEYGEN_SEED);
    BOOST_REQUIRE(key.SetSeed(seed));
    const auto id=*pq::GetID(key.GetPublicKey(),"regtest");
    pq::Payload p; p.mode=pq::TRANSFER; p.authorizations.resize(1); p.authorizations[0].public_key=key.GetPublicKey();
    auto tx=Transfer(id,p);
    const std::vector<CTxOut> prev{CTxOut(100,pq::GetScript(id))};
    const uint256 genesis=Params().GetConsensus().hashGenesisBlock;
    const auto message=pq::SignatureMessage(tx,prev,p,genesis,0);
    BOOST_REQUIRE(!message.empty());
    // Independent explicit little-endian concatenation and SHA-256 via Node crypto.
    std::array<unsigned char,32> message_hash{};
    CSHA256().Write(message.data(),message.size()).Finalize(message_hash.data());
    BOOST_CHECK_EQUAL(message.size(),1489U);
    BOOST_CHECK_EQUAL(HexStr(message_hash),"46dbf80f31e66ed2340ae3be093fffe107c5200608a8de4c3feb6adbda394d4d");
    const auto context = pq::SignatureContext("regtest");
    BOOST_REQUIRE(context);
    BOOST_CHECK_EQUAL(std::string(context->begin(),context->end()), "OLC/PQ/ML-DSA-44/regtest/tx/v1");
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(message,*context,signature));
    std::copy(signature.begin(),signature.end(),p.authorizations[0].signature.begin());
    tx.extraPayload=pq::EncodePayload(p);
    std::string why;
    BOOST_REQUIRE(pq::VerifyInputs(tx,prev,Params(),why));
    BOOST_CHECK_EQUAL(pq::GetSigOpCost(tx),400U);
    for(int mutation=0;mutation<8;++mutation) {
        auto bad=tx; auto spent=prev;
        if(mutation==0) bad.vout[0].nValue--;
        if(mutation==1) bad.vin[0].prevout.n++;
        if(mutation==2) bad.vin[0].nSequence++;
        if(mutation==3) bad.nLockTime++;
        if(mutation==4) spent[0].nValue++;
        if(mutation==5) spent[0].scriptPubKey=LegacyScript();
        if(mutation==6) (*bad.extraPayload)[3]^=1;
        if(mutation==7) bad.extraPayload->back()^=1;
        BOOST_CHECK(!pq::VerifyInputs(bad,spent,Params(),why));
    }
    BOOST_CHECK(!mldsa44::Verify(key.GetPublicKey(),pq::SignatureMessage(tx,prev,p,uint256S("02"),0),*context,signature));
    BOOST_CHECK(!mldsa44::Verify(key.GetPublicKey(),message,{},signature));
    BOOST_CHECK(pq::SignatureMessage(tx,{},p,genesis,0).empty());
    BOOST_CHECK(pq::SignatureMessage(tx,prev,p,genesis,1).empty());
}

BOOST_AUTO_TEST_CASE(reset_testnet_has_fresh_identity_and_active_payments)
{
    const auto test = CreateChainParams(CBaseChainParams::TESTNET);
    const auto main = CreateChainParams(CBaseChainParams::MAIN);
    BOOST_CHECK_EQUAL(test->GetConsensus().hashGenesisBlock.GetHex(),
        "0000074a425b707b97fd4404f6e97f69e2fb627ee0c9e62a6800152f483a1886");
    BOOST_CHECK(test->Checkpoints().mapCheckpoints->at(0) == test->GetConsensus().hashGenesisBlock);
    BOOST_CHECK_EQUAL(test->Checkpoints().nTimeLastCheckpoint, test->GenesisBlock().nTime);
    const unsigned char magic[]{0x6b, 0xc4, 0x02, 0x11};
    BOOST_CHECK_EQUAL_COLLECTIONS(test->MessageStart(), test->MessageStart() + 4, magic, magic + 4);
    BOOST_CHECK(!pq::PaymentsActive(*test, 0));
    BOOST_CHECK(pq::PaymentsActive(*test, 1));
    BOOST_CHECK(!test->GetConsensus().NetworkUpgradeActive(39, Consensus::UPGRADE_POS));
    BOOST_CHECK(test->GetConsensus().NetworkUpgradeActive(40, Consensus::UPGRADE_POS));
    BOOST_CHECK_EQUAL(main->GetConsensus().hashGenesisBlock.GetHex(),
        "0000091cf3aeeed50f65d6e640a35029d7b541b4e42950232385b13e88a12fdb");
    BOOST_CHECK_EQUAL(main->GetConsensus().vUpgrades[Consensus::UPGRADE_POS].nActivationHeight, 10081);
    BOOST_CHECK(!pq::PaymentsActive(*main, 0));
    BOOST_CHECK(pq::PaymentsActive(*main, 1));
    BOOST_CHECK(pq::PaymentsActive(*main, 1000000));
    BOOST_CHECK(!pq::MasternodesActive(*main, 0));
    BOOST_CHECK(pq::MasternodesActive(*main, 1));
    BOOST_CHECK_EQUAL(main->GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_SERVICE].nActivationHeight, 1);
}

BOOST_AUTO_TEST_CASE(pq_activation_requires_pq_rewards_and_transfers)
{
    pq::KeyID id{};
    id.fill(0x42);
    std::string reason;
    BOOST_CHECK(pq::CheckContext(*Params().GenesisBlock().vtx[0], Params(), 0, reason));

    CMutableTransaction coinbase;
    coinbase.vin.emplace_back();
    coinbase.vin[0].scriptSig = CScript() << OP_1 << OP_1;
    coinbase.vout.emplace_back(COIN, LegacyScript());
    BOOST_CHECK(!pq::CheckContext(CTransaction(coinbase), Params(), 1, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pq-only-output");

    coinbase.vout[0].scriptPubKey = pq::GetScript(id);
    BOOST_CHECK(pq::CheckContext(CTransaction(coinbase), Params(), 1, reason));

    CMutableTransaction ordinary;
    ordinary.vin.emplace_back(COutPoint(uint256S("01"), 0));
    ordinary.vout.emplace_back(COIN, LegacyScript());
    BOOST_CHECK(!pq::CheckContext(CTransaction(ordinary), Params(), 1, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pq-only-transaction");

    pq::Payload payload;
    payload.mode = pq::TRANSFER;
    payload.authorizations.resize(1);
    BOOST_CHECK(pq::CheckContext(CTransaction(Transfer(id, payload)), Params(), 1, reason));

    const auto testnet = CreateChainParams(CBaseChainParams::TESTNET);
    BOOST_CHECK(pq::CheckContext(CTransaction(ordinary), *testnet, 0, reason));
    BOOST_CHECK(!pq::CheckContext(CTransaction(ordinary), *testnet, 1, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pq-only-transaction");

    const auto mainnet = CreateChainParams(CBaseChainParams::MAIN);
    BOOST_CHECK(!pq::CheckContext(CTransaction(ordinary), *mainnet, 1, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pq-only-transaction");
}

BOOST_AUTO_TEST_CASE(testnet_signature_has_independent_context_and_genesis)
{
    const auto params = CreateChainParams(CBaseChainParams::TESTNET);
    mldsa44::Key key;
    BOOST_REQUIRE(key.SetSeed(ParseHex(mldsa44_vectors::KEYGEN_SEED)));
    const auto id = *pq::GetID(key.GetPublicKey(), "test");
    pq::Payload payload; payload.mode = pq::TRANSFER; payload.authorizations.resize(1);
    payload.authorizations[0].public_key = key.GetPublicKey();
    auto tx = Transfer(id, payload);
    const std::vector<CTxOut> prev{CTxOut(100, pq::GetScript(id))};
    const unsigned char context[] = "OLC/PQ/ML-DSA-44/testnet/tx/v1";
    const Span<const unsigned char> test_context{context, sizeof(context) - 1};
    const auto actual_context = pq::SignatureContext("test");
    BOOST_REQUIRE(actual_context);
    BOOST_CHECK(std::equal(actual_context->begin(), actual_context->end(), test_context.begin(), test_context.end()));
    for (const std::string network : {"", "testnet", "unknown"}) BOOST_CHECK(!pq::SignatureContext(network));
    const auto genesis = params->GetConsensus().hashGenesisBlock;
    BOOST_CHECK_EQUAL(genesis.GetHex(), "0000074a425b707b97fd4404f6e97f69e2fb627ee0c9e62a6800152f483a1886");
    for (int mutation = 0; mutation < 3; ++mutation) {
        const auto message = pq::SignatureMessage(tx, prev, payload,
            mutation == 2 ? Params().GetConsensus().hashGenesisBlock : genesis, 0);
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(key.Sign(message, mutation == 1 ? *pq::SignatureContext("regtest") : test_context, signature));
        std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
        tx.extraPayload = pq::EncodePayload(payload);
        std::string reason;
        BOOST_CHECK_EQUAL(pq::VerifyInputs(tx, prev, *params, reason), mutation == 0);
        if (mutation) BOOST_CHECK_EQUAL(reason, "bad-pq-signature");
    }
}

BOOST_AUTO_TEST_CASE(two_input_signatures_commit_to_order_and_position)
{
    mldsa44::Key a,b; BOOST_REQUIRE(a.Generate()); BOOST_REQUIRE(b.Generate());
    pq::Payload p; p.mode=pq::TRANSFER; p.authorizations.resize(2);
    p.authorizations[0].public_key=a.GetPublicKey(); p.authorizations[1].public_key=b.GetPublicKey();
    const auto id=*pq::GetID(a.GetPublicKey(),"regtest");
    auto tx=Transfer(id,p); tx.vin.emplace_back(COutPoint(uint256S("02"),0));
    tx.vout.emplace_back(5,pq::GetScript(*pq::GetID(b.GetPublicKey(),"regtest")));
    const std::vector<CTxOut> prev{CTxOut(50,pq::GetScript(id)),CTxOut(50,pq::GetScript(*pq::GetID(b.GetPublicKey(),"regtest")))};
    std::vector<unsigned char> sig;
    for(uint32_t i=0;i<2;++i) {
        const auto message=pq::SignatureMessage(tx,prev,p,Params().GetConsensus().hashGenesisBlock,i);
        BOOST_REQUIRE((i?b:a).Sign(message,*pq::SignatureContext("regtest"),sig));
        std::copy(sig.begin(),sig.end(),p.authorizations[i].signature.begin());
    }
    tx.extraPayload=pq::EncodePayload(p); std::string why;
    BOOST_REQUIRE(pq::VerifyInputs(tx,prev,Params(),why));
    BOOST_CHECK_EQUAL(pq::GetSigOpCost(tx),800U);
    for(int mutation=0;mutation<6;++mutation) {
        auto bad=tx; auto spent=prev; auto payload=p;
        if(mutation==0) std::swap(bad.vin[0],bad.vin[1]);
        if(mutation==1) std::swap(bad.vout[0],bad.vout[1]);
        if(mutation==2) std::swap(payload.authorizations[0].signature,payload.authorizations[1].signature);
        if(mutation==3) {std::swap(bad.vin[0],bad.vin[1]);std::swap(spent[0],spent[1]);std::swap(payload.authorizations[0],payload.authorizations[1]);}
        if(mutation==4) spent[1].nValue++;
        if(mutation==5) payload.authorizations[1].signature.back()^=1;
        bad.extraPayload=pq::EncodePayload(payload);
        BOOST_CHECK(!pq::VerifyInputs(bad,spent,Params(),why));
    }
    p.authorizations.resize(3);
    BOOST_CHECK(pq::EncodePayload(p).empty());
}

BOOST_AUTO_TEST_CASE(payload_memory_is_charged_to_mempool)
{
    pq::Payload payload;
    auto tx=Transfer(pq::KeyID{},payload);
    tx.extraPayload=nullopt;
    const auto base=CTransaction(tx).DynamicMemoryUsage();
    for(size_t count : {size_t(1),size_t(2)}) {
        payload.authorizations.resize(count);
        tx.extraPayload=pq::EncodePayload(payload);
        const auto ref=MakeTransactionRef(tx);
        const auto allocation=memusage::DynamicUsage(*ref->extraPayload);
        BOOST_CHECK_EQUAL(ref->DynamicMemoryUsage(),base+allocation);
        CTxMemPoolEntry entry(ref,10000,0,0,false,pq::GetSigOpCost(*ref));
        BOOST_CHECK(entry.DynamicMemoryUsage() >= base+allocation);
    }
}

BOOST_AUTO_TEST_CASE(bounded_verification_workload)
{
    mldsa44::Key key; BOOST_REQUIRE(key.Generate());
    const auto id=*pq::GetID(key.GetPublicKey(),"regtest");
    pq::Payload payload; payload.mode=pq::TRANSFER; payload.authorizations.resize(1);
    payload.authorizations[0].public_key=key.GetPublicKey();
    auto tx=Transfer(id,payload);
    const std::vector<CTxOut> prev{CTxOut(100,pq::GetScript(id))};
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(pq::SignatureMessage(tx,prev,payload,Params().GetConsensus().hashGenesisBlock,0),
                          *pq::SignatureContext("regtest"),signature));
    std::copy(signature.begin(),signature.end(),payload.authorizations[0].signature.begin());
    tx.extraPayload=pq::EncodePayload(payload);
    const CTransaction valid(tx);
    // Change the challenge, retaining the signature's complete canonical packing.
    payload.authorizations[0].signature[0]^=1;
    tx.extraPayload=pq::EncodePayload(payload);
    const CTransaction invalid(tx);
    std::string reason;
    for(bool good : {true,false}) {
        const auto start=std::chrono::steady_clock::now();
        for(size_t i=0;i<100;++i) BOOST_CHECK_EQUAL(pq::VerifyInputs(good?valid:invalid,prev,Params(),reason),good);
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        BOOST_TEST_MESSAGE("100 " << (good?"valid":"invalid-challenge") << " PQ authorizations: " << elapsed << " us");
    }
}

BOOST_AUTO_TEST_CASE(bounded_payload_mutations)
{
    mldsa44::Key key;
    BOOST_REQUIRE(key.SetSeed(ParseHex(mldsa44_vectors::KEYGEN_SEED)));
    const auto id = *pq::GetID(key.GetPublicKey(), "regtest");
    for (size_t count : {size_t{1}, size_t{2}}) {
        pq::Payload payload; payload.mode = pq::TRANSFER; payload.authorizations.resize(count);
        for (auto& auth : payload.authorizations) auth.public_key = key.GetPublicKey();
        auto tx = Transfer(id, payload); tx.nLockTime = 0;
        if (count == 2) tx.vin.emplace_back(COutPoint(uint256S("02"), 0));
        const std::vector<CTxOut> prevouts(count, CTxOut(100, pq::GetScript(id)));
        for (size_t i = 0; i < count; ++i) {
            std::vector<unsigned char> signature;
            BOOST_REQUIRE(key.Sign(pq::SignatureMessage(tx, prevouts, payload,
                Params().GetConsensus().hashGenesisBlock, i), *pq::SignatureContext("regtest"), signature));
            std::copy(signature.begin(), signature.end(), payload.authorizations[i].signature.begin());
        }
        const auto bytes = pq::EncodePayload(payload);
        tx.extraPayload = bytes;
        std::string reason;
        BOOST_REQUIRE(pq::VerifyInputs(tx, prevouts, Params(), reason));
        pq::Payload decoded = payload;
        for (size_t length = 0; length < bytes.size(); ++length) {
            tx.extraPayload = std::vector<unsigned char>(bytes.begin(), bytes.begin() + length);
            BOOST_CHECK(!pq::DecodePayload(tx, decoded));
            BOOST_CHECK(decoded.authorizations.empty());
        }
        // Every single-byte mutation either breaks the fixed envelope or fails
        // authorization; the decoder deliberately does not authenticate content.
        for (size_t i = 0; i < bytes.size(); ++i) {
            tx.extraPayload = bytes;
            (*tx.extraPayload)[i] ^= 1;
            BOOST_CHECK_EQUAL(pq::DecodePayload(tx, decoded), i >= 3);
            BOOST_CHECK(!pq::VerifyInputs(tx, prevouts, Params(), reason));
        }
        for (size_t length : {bytes.size() + 1, size_t{10000}, size_t{10001}, size_t{1024 * 1024}}) {
            tx.extraPayload = bytes;
            tx.extraPayload->resize(length);
            decoded = payload;
            BOOST_CHECK(!pq::DecodePayload(tx, decoded));
            BOOST_CHECK(decoded.authorizations.empty());
        }
    }
}

BOOST_AUTO_TEST_CASE(service_coinbase_envelope_is_bounded_and_coinbase_only)
{
    // Versioned service carrier: optional certificate, then fixed-size proofs.
    CDataStream data(SER_NETWORK, 0);
    data << uint8_t{1} << std::vector<unsigned char>{} << uint8_t{1}
         << uint256S("01") << uint64_t{0} << uint32_t{10} << uint256S("10")
         << std::array<unsigned char, mldsa44::SIGNATURE_SIZE>{};
    pq::Payload payload; payload.mode = 8;
    payload.data = {data.begin(), data.end()};
    const auto encoded = pq::EncodePayload(payload);
    BOOST_REQUIRE(!encoded.empty());
    auto tx = Transfer(pq::KeyID{}, payload);
    std::string reason;
    BOOST_CHECK(!pq::CheckStructure(CTransaction(tx), Params(), reason));
    tx.vin[0].prevout.SetNull();
    BOOST_REQUIRE(pq::CheckStructure(CTransaction(tx), Params(), reason));
    BOOST_CHECK_EQUAL(pq::GetSigOpCost(CTransaction(tx)), pq::SIGOP_COST);
    BOOST_CHECK(!pq::CheckContext(CTransaction(tx), Params(), 11, reason));
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_MASTERNODES, 1);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ_SERVICE, 11);
    BOOST_CHECK(!pq::CheckContext(CTransaction(tx), Params(), 10, reason));
    BOOST_CHECK(pq::CheckContext(CTransaction(tx), Params(), 11, reason));
    pqservice::Carrier carrier, decoded;
    BOOST_REQUIRE(pqservice::Decode(payload.data, carrier));
    carrier.heartbeats.push_back(carrier.heartbeats[0]);
    BOOST_CHECK(!pqservice::Decode(pqservice::Encode(carrier), decoded));
    BOOST_CHECK(decoded.heartbeats.empty());
    carrier.heartbeats.resize(pqservice::MAX_HEARTBEATS + 1);
    BOOST_CHECK(pqservice::Encode(carrier).empty());
    payload.data.push_back(0);
    tx.extraPayload = pq::EncodePayload(payload);
    BOOST_CHECK(!pq::CheckStructure(CTransaction(tx), Params(), reason));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(pqtestnet_validation_tests, TestnetSetup)

BOOST_AUTO_TEST_CASE(shared_inputs_bind_testnet_context_and_genesis)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    CheckNetworkBoundInputs("regtest");
}

BOOST_AUTO_TEST_CASE(activation_boundaries_and_prerequisites)
{
    pq::Payload payload;
    payload.authorizations.resize(1);
    auto tx = Transfer(pq::KeyID{}, payload);
    for (const auto& network : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        auto params = CreateChainParams(network);
        params->UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
        for (int height : {19, 20, 21}) {
            const bool active = height >= 20;
            BOOST_CHECK_EQUAL(pq::PaymentsActive(*params, height), active);
            CValidationState state;
            BOOST_CHECK_EQUAL(ContextualCheckTransaction(MakeTransactionRef(tx), state, *params, height, true, false), active);
            if (!active) BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pq-not-active");
        }
        BOOST_CHECK(!pq::PaymentsActive(*params, -1));
        params->UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT);
        BOOST_CHECK(!pq::PaymentsActive(*params, 21));
    }
    struct UnknownNetwork : CChainParams {
        UnknownNetwork() : CChainParams(*CreateChainParams(CBaseChainParams::TESTNET)) { strNetworkID = "unknown"; }
        const CCheckpointData& Checkpoints() const override { return Params().Checkpoints(); }
    } unknown;
    unknown.UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 0);
    std::string reason;
    BOOST_CHECK(!pq::PaymentsActive(unknown, 20));
    BOOST_CHECK(!pq::CheckStructure(tx, unknown, reason));
    BOOST_CHECK_EQUAL(reason, "bad-pq-network");
    CValidationState state;
    BOOST_CHECK(!ContextualCheckTransaction(MakeTransactionRef(tx), state, unknown, 20, true, false));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pq-not-active");
}

BOOST_AUTO_TEST_CASE(pq_rewards_become_spendable_at_activation)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    mldsa44::Key key; BOOST_REQUIRE(key.Generate());
    const auto id = *pq::GetID(key.GetPublicKey(), "test");
    pq::Payload payload; payload.mode = pq::TRANSFER; payload.authorizations.resize(1);
    payload.authorizations[0].public_key = key.GetPublicKey();
    auto tx = Transfer(id, payload);
    const std::vector<CTxOut> prev{CTxOut(100, pq::GetScript(id))};
    std::vector<unsigned char> signature;
    BOOST_REQUIRE(key.Sign(pq::SignatureMessage(tx, prev, payload, Params().GetConsensus().hashGenesisBlock, 0),
                          *pq::SignatureContext("test"), signature));
    std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
    tx.extraPayload = pq::EncodePayload(payload);
    const CTransaction transfer(tx);
    std::string reason;
    BOOST_REQUIRE(pq::VerifyInputs(transfer, prev, Params(), reason));
    LOCK(cs_main);
    for (int height : {19, 20, 21}) {
        for (uint32_t coin_height : {19U, 20U, MEMPOOL_HEIGHT}) {
            CCoinsViewCache view(pcoinsTip.get());
            PQViewHeight previous(view, height);
            view.AddCoin(tx.vin[0].prevout, Coin(prev[0], coin_height, false, false), false);
            for (bool scripts : {false, true}) {
                CValidationState state; PrecomputedTransactionData data(transfer);
                BOOST_CHECK_EQUAL(CheckInputs(transfer, state, view, scripts, MANDATORY_SCRIPT_VERIFY_FLAGS, true, data),
                                  height >= 20);
                if (height < 20) BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pq-not-active");
            }
        }
    }
    // Even when Script checks are skipped, neither a normal spend nor staking
    // may interpret an old marker as a legacy spendable output.
    for (bool stake : {false, true}) {
        auto ordinary = tx;
        ordinary.nType = CTransaction::NORMAL; ordinary.extraPayload = nullopt;
        ordinary.vout = {CTxOut(90, LegacyScript())};
        if (stake) ordinary.vout.insert(ordinary.vout.begin(), CTxOut(0, CScript()));
        const CTransaction normal(ordinary);
        for (int height : {19, 20}) {
            CCoinsViewCache view(pcoinsTip.get()); PQViewHeight previous(view, height);
            view.AddCoin(tx.vin[0].prevout, Coin(prev[0], 19, false, false), false);
            for (bool scripts : {false, true}) {
                CValidationState state; PrecomputedTransactionData data(normal);
                BOOST_CHECK(!CheckInputs(normal, state, view, scripts, MANDATORY_SCRIPT_VERIFY_FLAGS, true, data));
                BOOST_CHECK_EQUAL(state.GetRejectReason(), height < 20 ? "bad-pq-spend-type" :
                    (stake ? "bad-pq-only-stake" : "bad-pq-only-transaction"));
            }
        }
    }
}

BOOST_AUTO_TEST_CASE(reorg_below_activation_evicts_existing_pending_entries)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    pq::Payload payload;
    payload.authorizations.resize(1);
    auto fund = Transfer(pq::KeyID{}, payload); fund.nLockTime = 0;
    auto child = fund; child.vin[0].prevout = COutPoint(fund.GetHash(), 0);
    auto ordinary = fund; ordinary.nType = CTransaction::NORMAL; ordinary.extraPayload = nullopt;
    ordinary.vout[0].scriptPubKey = LegacyScript();
    LOCK(cs_main);
    for (unsigned int next_height : {21U, 20U, 19U}) {
        CTxMemPool pool(CFeeRate(0)); TestMemPoolEntryHelper entry;
        pool.addUnchecked(fund.GetHash(), entry.FromTx(fund));
        pool.addUnchecked(child.GetHash(), entry.FromTx(child));
        pool.addUnchecked(ordinary.GetHash(), entry.FromTx(ordinary));
        pool.removeForReorg(pcoinsTip.get(), next_height, 0);
        BOOST_CHECK_EQUAL(pool.exists(fund.GetHash()), next_height >= 20);
        BOOST_CHECK_EQUAL(pool.exists(child.GetHash()), next_height >= 20);
        BOOST_CHECK_EQUAL(pool.exists(ordinary.GetHash()), next_height < 20);
    }
}

static void CheckForwardActivationCleanup()
{
    struct RestorePolicy {
        const bool standard{fRequireStandard};
        ~RestorePolicy() { fRequireStandard = standard; mempool.setSanityCheck(0); }
    } restore_policy;
    fRequireStandard = false; // The existing testnet default permits nonstandard scripts.
    mempool.setSanityCheck(1);
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 3);
    const CScript spendable = CScript() << OP_TRUE;
    auto mine_empty_block = [&](int height) {
        auto block_template = BlockAssembler(Params(), false).CreateNewBlock(spendable, nullptr, false, nullptr, true);
        BOOST_REQUIRE(block_template);
        auto block = std::make_shared<CBlock>(block_template->block);
        BOOST_REQUIRE(SolveBlock(block, height));
        BOOST_REQUIRE(ProcessNewBlock(block, nullptr));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainActive.Height()), height);
    };
    mine_empty_block(1);
    CMutableTransaction parent;
    parent.vin.emplace_back(COutPoint(uint256S("abc1"), 0));
    parent.vout = {CTxOut(COIN, pq::GetScript(pq::KeyID{})), CTxOut(2 * COIN, spendable)};
    CMutableTransaction child;
    child.vin.emplace_back(COutPoint(parent.GetHash(), 1));
    child.vout.emplace_back(COIN, spendable);
    CMutableTransaction ordinary;
    ordinary.vin.emplace_back(COutPoint(uint256S("abc2"), 0));
    ordinary.vout.emplace_back(3 * COIN, spendable);
    {
        LOCK(cs_main);
        pcoinsTip->AddCoin(parent.vin[0].prevout, Coin(CTxOut(4 * COIN, spendable), 0, false, false), false);
        pcoinsTip->AddCoin(ordinary.vin[0].prevout, Coin(CTxOut(4 * COIN, spendable), 0, false, false), false);
        for (const auto& tx : {parent, child, ordinary}) {
            CValidationState state;
            BOOST_REQUIRE_MESSAGE(AcceptToMemoryPool(mempool, state, MakeTransactionRef(tx), false, nullptr, true),
                                  state.GetRejectReason());
        }
    }
    BOOST_REQUIRE_EQUAL(mempool.size(), 3U);
    // All three transactions are valid for candidate height H-1.
    WITH_LOCK(cs_main, mempool.check(pcoinsTip.get()));
    BOOST_CHECK_NO_THROW(BlockAssembler(Params(), false).CreateNewBlock(spendable));
    mine_empty_block(2); // The next candidate height is now H.
    BOOST_CHECK(!mempool.exists(parent.GetHash()));
    BOOST_CHECK(!mempool.exists(child.GetHash()));
    BOOST_CHECK(!mempool.exists(ordinary.GetHash()));
    BOOST_CHECK_EQUAL(mempool.size(), 0U);
    std::unique_ptr<CBlockTemplate> next;
    BOOST_CHECK_NO_THROW(next = BlockAssembler(Params(), false).CreateNewBlock(pq::GetScript(pq::KeyID{})));
    BOOST_REQUIRE(next);
    BOOST_CHECK_EQUAL(next->block.vtx.size(), 1U);
}

BOOST_AUTO_TEST_CASE(forward_activation_cleans_all_pending_ordinary_transactions)
{
    CheckForwardActivationCleanup();
}

BOOST_AUTO_TEST_CASE(block_template_without_tier_two_processor)
{
    auto processor = std::move(llmq::quorumBlockProcessor);
    BOOST_REQUIRE(!llmq::quorumBlockProcessor);
    std::unique_ptr<CBlockTemplate> block_template;
    BOOST_CHECK_NO_THROW(block_template = BlockAssembler(Params(), false).CreateNewBlock(
        pq::GetScript(pq::KeyID{}), nullptr, false, nullptr, true));
    BOOST_CHECK(block_template);
    llmq::quorumBlockProcessor = std::move(processor);
}

BOOST_AUTO_TEST_CASE(mempool_sanity_preserves_unconfirmed_heights_above_one_million)
{
    constexpr int activation_height = 1000001;
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, activation_height);
    mldsa44::Key key; BOOST_REQUIRE(key.Generate());
    const auto id = *pq::GetID(key.GetPublicKey(), "test");
    pq::Payload payload; payload.mode = pq::TRANSFER; payload.authorizations.resize(1);
    payload.authorizations[0].public_key = key.GetPublicKey();
    LOCK(cs_main);
    CCoinsViewCache view(pcoinsTip.get()); PQViewHeight previous(view, activation_height);
    COutPoint outpoint(uint256S("01"), 2);
    CTxOut prevout(4 * COIN, pq::GetScript(id));
    view.AddCoin(outpoint, Coin(prevout, activation_height, false, false), false);
    CTxMemPool pool(CFeeRate(0)); pool.setSanityCheck(1);
    TestMemPoolEntryHelper entry;
    // A grandchild covers both synthetic UpdateCoins paths: independent entries
    // and entries deferred until their in-mempool parent has been checked.
    for (int i = 0; i < 3; ++i) {
        auto tx = Transfer(id, payload);
        tx.nLockTime = 0;
        tx.vin[0].prevout = outpoint;
        tx.vout[0].nValue = prevout.nValue - COIN;
        std::vector<unsigned char> signature;
        BOOST_REQUIRE(key.Sign(pq::SignatureMessage(tx, {prevout}, payload, Params().GetConsensus().hashGenesisBlock, 0),
                              *pq::SignatureContext("test"), signature));
        std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
        tx.extraPayload = pq::EncodePayload(payload);
        const CTransaction transaction(tx);
        CCoinsViewMemPool pool_view(&view, pool);
        CCoinsViewCache admission_view(&pool_view);
        if (i) BOOST_CHECK_EQUAL(admission_view.AccessCoin(outpoint).nHeight, MEMPOOL_HEIGHT);
        for (bool scripts : {false, true}) {
            CValidationState state; PrecomputedTransactionData data(transaction);
            BOOST_REQUIRE_MESSAGE(CheckInputs(transaction, state, admission_view, scripts, MANDATORY_SCRIPT_VERIFY_FLAGS, true, data),
                                  state.GetRejectReason());
        }
        pool.addUnchecked(transaction.GetHash(), entry.Fee(COIN).Height(activation_height - 1)
            .SigOps(pq::GetSigOpCost(transaction)).FromTx(transaction));
        outpoint = COutPoint(transaction.GetHash(), 0);
        prevout = transaction.vout[0];
    }
    BOOST_REQUIRE_EQUAL(pool.size(), 3U);
    pool.check(&view);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(pqvalidation_tests, RegTestingSetup)

BOOST_AUTO_TEST_CASE(shared_inputs_bind_regtest_context_and_genesis)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    CheckNetworkBoundInputs("test");
}

BOOST_AUTO_TEST_CASE(shared_input_checks_are_mandatory)
{
    mldsa44::Key key; BOOST_REQUIRE(key.Generate());
    const auto id=*pq::GetID(key.GetPublicKey(),"regtest");
    pq::Payload p; p.mode=pq::TRANSFER; p.authorizations.resize(1); p.authorizations[0].public_key=key.GetPublicKey();
    auto tx=Transfer(id,p);
    const std::vector<CTxOut> prev{CTxOut(100,pq::GetScript(id))};
    std::vector<unsigned char> sig;
    BOOST_REQUIRE(key.Sign(pq::SignatureMessage(tx,prev,p,Params().GetConsensus().hashGenesisBlock,0),*pq::SignatureContext("regtest"),sig));
    std::copy(sig.begin(),sig.end(),p.authorizations[0].signature.begin()); tx.extraPayload=pq::EncodePayload(p);
    LOCK(cs_main);
    CCoinsViewCache view(pcoinsTip.get());
    view.AddCoin(tx.vin[0].prevout,Coin(CTxOut(prev[0]),0,false,false),false);
    CTransaction valid(tx);
    BOOST_CHECK_EQUAL(GetLegacySigOpCount(valid),400U);
    BOOST_CHECK(GetDustThreshold(prev[0],CFeeRate(1000)) >= 3773);
    for (bool scripts : {false,true}) {
        CValidationState state; PrecomputedTransactionData data(valid);
        BOOST_CHECK(CheckInputs(valid,state,view,scripts,MANDATORY_SCRIPT_VERIFY_FLAGS,true,data));
        auto bad=tx; bad.extraPayload->back()^=1;
        CTransaction invalid(bad); CValidationState bad_state; PrecomputedTransactionData bad_data(invalid);
        BOOST_CHECK(!CheckInputs(invalid,bad_state,view,scripts,MANDATORY_SCRIPT_VERIFY_FLAGS,true,bad_data));
        BOOST_CHECK_EQUAL(bad_state.GetRejectReason(),"bad-pq-signature");
    }
    auto normal=tx; normal.nType=CTransaction::NORMAL; normal.extraPayload=nullopt; normal.vout[0].scriptPubKey=LegacyScript();
    CTransaction downgrade(normal); CValidationState state; PrecomputedTransactionData data(downgrade);
    BOOST_CHECK(!CheckInputs(downgrade,state,view,false,0,true,data));
    BOOST_CHECK_EQUAL(state.GetRejectReason(),"bad-pq-only-transaction");
    normal.vout={CTxOut(0,CScript()),CTxOut(90,LegacyScript())};
    const CTransaction coinstake(normal);
    BOOST_REQUIRE(coinstake.IsCoinStake());
    for(bool scripts : {false,true}) {
        CValidationState stake_state; PrecomputedTransactionData stake_data(coinstake);
        BOOST_CHECK(!CheckInputs(coinstake,stake_state,view,scripts,MANDATORY_SCRIPT_VERIFY_FLAGS,true,stake_data));
        BOOST_CHECK_EQUAL(stake_state.GetRejectReason(),"bad-pq-only-stake");
    }
}

BOOST_AUTO_TEST_CASE(shared_input_checks_enforce_activation_without_scripts)
{
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    auto tx = Transfer(pq::KeyID{}, pq::Payload{});
    LOCK(cs_main);
    CCoinsViewCache view(pcoinsTip.get());
    view.AddCoin(tx.vin[0].prevout, Coin(CTxOut(100, LegacyScript()), 0, false, false), false);
    CValidationState state;
    const CTransaction fund(tx);
    PrecomputedTransactionData data(fund);
    BOOST_CHECK(!CheckInputs(fund, state, view, false, 0, true, data));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-pq-not-active");
}

BOOST_AUTO_TEST_CASE(reorg_removes_pending_pq_and_descendants)
{
    auto fund = Transfer(pq::KeyID{}, pq::Payload{});
    fund.nLockTime = 0;
    auto child = fund;
    child.nType = CTransaction::NORMAL;
    child.extraPayload = nullopt;
    child.vin[0].prevout = COutPoint(fund.GetHash(), 0);
    child.vout[0].scriptPubKey = LegacyScript();
    auto ordinary = child;
    ordinary.vin[0].prevout = COutPoint(uint256S("02"), 0);
    CTxMemPool pool(CFeeRate(0));
    TestMemPoolEntryHelper entry;
    pool.addUnchecked(fund.GetHash(), entry.FromTx(fund));
    pool.addUnchecked(child.GetHash(), entry.FromTx(child));
    pool.addUnchecked(ordinary.GetHash(), entry.FromTx(ordinary));
    LOCK(cs_main);
    pool.removeForReorg(pcoinsTip.get(), 1, 0);
    BOOST_CHECK(!pool.exists(fund.GetHash()));
    BOOST_CHECK(!pool.exists(child.GetHash()));
    BOOST_CHECK(!pool.exists(ordinary.GetHash()));
    BOOST_CHECK_EQUAL(pool.size(), 0U);
}

BOOST_AUTO_TEST_CASE(output_creation_requires_the_pq_envelope)
{
    CMutableTransaction tx;
    tx.vin.emplace_back(COutPoint(uint256S("01"),2));
    tx.vout.emplace_back(100,pq::GetScript(pq::KeyID{}));
    CValidationState state;
    BOOST_CHECK(CheckTransaction(tx,state,true));
    CValidationState ordinary_context;
    BOOST_CHECK(!ContextualCheckTransaction(MakeTransactionRef(tx),ordinary_context,Params(),1,false,false));
    BOOST_CHECK_EQUAL(ordinary_context.GetRejectReason(),"bad-pq-only-transaction");
    pq::Payload payload;
    payload.authorizations.resize(1);
    tx.nVersion=3; tx.nType=CTransaction::PQ; tx.sapData=nullopt; tx.extraPayload=pq::EncodePayload(payload);
    CValidationState pq_state;
    BOOST_CHECK(CheckTransaction(tx,pq_state,true));
    LOCK(cs_main);
    CValidationState special_state;
    BOOST_CHECK(CheckSpecialTxNoContext(tx,special_state));
    UpdateNetworkUpgradeParameters(Consensus::UPGRADE_PQ, 20);
    CValidationState before_activation;
    BOOST_CHECK(!ContextualCheckTransaction(MakeTransactionRef(tx),before_activation,Params(),1,false,false));
}

BOOST_AUTO_TEST_CASE(block_authorization_budget_boundary)
{
    CMutableTransaction coinbase;
    coinbase.vin.emplace_back();
    coinbase.vin[0].scriptSig=CScript() << OP_1 << OP_1;
    coinbase.vout.emplace_back(COIN,CScript() << OP_TRUE);
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(coinbase));
    pq::Payload payload; payload.mode=pq::TRANSFER; payload.authorizations.resize(1);
    for(uint32_t i=0;i<100;++i) {
        auto tx=Transfer(pq::KeyID{},payload); tx.vin[0].prevout.n=i;
        block.vtx.push_back(MakeTransactionRef(tx));
    }
    LOCK(cs_main);
    CValidationState at_limit;
    BOOST_CHECK(CheckBlock(block,at_limit,false,false,false));
    CMutableTransaction ordinary;
    ordinary.vin.emplace_back(COutPoint(uint256S("03"),0));
    ordinary.vout.emplace_back(COIN,LegacyScript());
    block.vtx.push_back(MakeTransactionRef(ordinary));
    CValidationState mixed_over;
    BOOST_CHECK(!CheckBlock(block,mixed_over,false,false,false));
    BOOST_CHECK_EQUAL(mixed_over.GetRejectReason(),"bad-blk-sigops");
    block.vtx.pop_back();
    const auto last_pq=block.vtx.back();
    block.vtx.back()=MakeTransactionRef(ordinary);
    CValidationState mixed_below;
    BOOST_CHECK(CheckBlock(block,mixed_below,false,false,false));
    block.vtx.back()=last_pq;
    auto extra=Transfer(pq::KeyID{},payload); extra.vin[0].prevout.n=100;
    block.vtx.push_back(MakeTransactionRef(extra));
    CValidationState over_limit;
    BOOST_CHECK(!CheckBlock(block,over_limit,false,false,false));
    BOOST_CHECK_EQUAL(over_limit.GetRejectReason(),"bad-blk-sigops");
}

BOOST_AUTO_TEST_CASE(full_block_authorization_resource_samples)
{
    mldsa44::Key key; BOOST_REQUIRE(key.Generate());
    const auto id = *pq::GetID(key.GetPublicKey(), "regtest");
    const CTxOut pq_prevout(COIN, pq::GetScript(id));
    uint32_t next_outpoint = 0;
    for (int scenario = 0; scenario < 3; ++scenario) {
        const char* label = scenario == 0 ? "100-pq" :
                            scenario == 1 ? "100-pq-invalid-last" : "101-pq-over-budget";
        for (int sample = 0; sample < 3; ++sample) {
            auto block_template = BlockAssembler(Params(), false).CreateNewBlock(pq::GetScript(id));
            BOOST_REQUIRE(block_template);
            auto block = std::make_shared<CBlock>(block_template->block);
            const int height = WITH_LOCK(cs_main, return chainActive.Height() + 1);
            const int count = scenario == 2 ? 101 : 100;
            std::vector<COutPoint> injected;
            for (int i = 0; i < count; ++i) {
                pq::Payload payload; payload.mode = pq::TRANSFER; payload.authorizations.resize(1);
                payload.authorizations[0].public_key = key.GetPublicKey();
                auto tx = Transfer(id, payload);
                tx.nLockTime = 0;
                tx.vin[0].prevout = COutPoint(uint256S("f00d"), next_outpoint++);
                tx.vout[0].nValue = COIN - 1000;
                std::vector<unsigned char> signature;
                BOOST_REQUIRE(key.Sign(pq::SignatureMessage(tx, {pq_prevout}, payload,
                    Params().GetConsensus().hashGenesisBlock, 0), *pq::SignatureContext("regtest"), signature));
                std::copy(signature.begin(), signature.end(), payload.authorizations[0].signature.begin());
                tx.extraPayload = pq::EncodePayload(payload);
                injected.push_back(tx.vin[0].prevout);
                WITH_LOCK(cs_main, pcoinsTip->AddCoin(injected.back(), Coin(CTxOut(pq_prevout), 0, false, false), false));
                block->vtx.push_back(MakeTransactionRef(tx));
            }
            auto reward = CMutableTransaction(*block->vtx[0]);
            reward.vout[0].nValue += count * 1000;
            block->vtx[0] = MakeTransactionRef(std::move(reward));
            BOOST_REQUIRE(SolveBlock(block, height));
            if (scenario == 1) {
                CValidationState control;
                BOOST_REQUIRE(WITH_LOCK(cs_main, return TestBlockValidity(control, *block, chainActive.Tip())));
                auto last = CMutableTransaction(*block->vtx.back());
                (*last.extraPayload)[3 + mldsa44::PUBLIC_KEY_SIZE] ^= 1; // canonical packing, invalid challenge
                block->vtx.back() = MakeTransactionRef(last);
                BOOST_REQUIRE(SolveBlock(block, height));
            }
            unsigned int sigops = 0;
            for (const auto& tx : block->vtx) sigops += GetLegacySigOpCount(*tx);
            BOOST_REQUIRE_EQUAL(sigops, count * pq::SIGOP_COST);
            block->fChecked = false;
            const auto previous_hash = WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash());
            const auto cache_before = WITH_LOCK(cs_main, return pcoinsTip->DynamicMemoryUsage());
            CValidationState state;
            const auto start = std::chrono::steady_clock::now();
            const bool valid = WITH_LOCK(cs_main, return TestBlockValidity(state, *block, chainActive.Tip()));
            const auto validation_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
            BOOST_REQUIRE_MESSAGE(valid == (scenario == 0),
                "scenario=" << label << " reject=" << state.GetRejectReason()
                << " debug=" << state.GetDebugMessage());
            BOOST_CHECK_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), previous_hash);
            if (!valid) BOOST_CHECK_EQUAL(state.GetRejectReason(), scenario == 1 ? "bad-pq-signature" : "bad-blk-sigops");
            BOOST_TEST_MESSAGE("PQ block " << label << " sample=" << sample << " TestBlockValidity_us=" << validation_us
                << " serialized_bytes=" << GetSerializeSize(*block, PROTOCOL_VERSION)
                << " tip_cache_before_bytes=" << cache_before
                << " tip_cache_after_validation_bytes=" << WITH_LOCK(cs_main, return pcoinsTip->DynamicMemoryUsage()));
            if (valid) {
                block->fChecked = false;
                const auto connect_start = std::chrono::steady_clock::now();
                const bool accepted = ProcessNewBlock(block, nullptr);
                const auto connect_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - connect_start).count();
                BOOST_REQUIRE(accepted);
                BOOST_REQUIRE_EQUAL(WITH_LOCK(cs_main, return chainActive.Tip()->GetBlockHash()), block->GetHash());
                BOOST_TEST_MESSAGE("PQ block " << label << " sample=" << sample << " ProcessNewBlock_us=" << connect_us
                    << " tip_cache_after_connection_bytes=" << WITH_LOCK(cs_main, return pcoinsTip->DynamicMemoryUsage()));
            }
            {
                LOCK(cs_main);
                for (const auto& outpoint : injected) {
                    BOOST_CHECK_EQUAL(pcoinsTip->AccessCoin(outpoint).IsSpent(), valid);
                    if (!valid) pcoinsTip->SpendCoin(outpoint);
                }
            }
        }
    }
    // Inputs are synthetic trusted UTXOs. Signing and PoW solving are outside
    // timings. TestBlockValidity reaches ConnectBlock(fJustCheck=true), without
    // disk writes; ProcessNewBlock also connects/writes, but does not force fsync.
    // Cache bytes are not peak RSS; collect process peak memory externally.
}

BOOST_AUTO_TEST_SUITE_END()
