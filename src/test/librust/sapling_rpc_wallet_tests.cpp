// Copyright (c) 2016-2020 The ZCash developers
// Copyright (c) 2020-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "fs.h"

#include "wallet/test/wallet_test_fixture.h"
#include "test/librust/utiltest.h"

#include "rpc/server.h"
#include "rpc/protocol.h"
#include "rpc/register.h"

#include "core_io.h"
#include "key_io.h"
#include "consensus/merkle.h"
#include "wallet/wallet.h"
#include "wallet/walletutil.h"
#include "wallet/rpcwallet.h"

#include "sapling/key_io_sapling.h"
#include "sapling/address.h"
#include "sapling/sapling_operation.h"

#include <algorithm>
#include <initializer_list>

#include <boost/test/unit_test.hpp>

#include <univalue.h>


namespace {

    /** Set the working directory for the duration of the scope. */
    class PushCurrentDirectory {
    public:
        explicit PushCurrentDirectory(const std::string &new_cwd)
                : old_cwd(fs::current_path()) {
            fs::current_path(new_cwd);
        }

        ~PushCurrentDirectory() {
            fs::current_path(old_cwd);
        }
    private:
        fs::path old_cwd;
    };

    class WalletRegistration {
    public:
        explicit WalletRegistration(CWallet& wallet) : wallet(&wallet) {
            vpwallets.insert(vpwallets.begin(), this->wallet);
        }

        ~WalletRegistration() {
            const auto it = std::find(vpwallets.begin(), vpwallets.end(), wallet);
            if (it != vpwallets.end()) vpwallets.erase(it);
        }

    private:
        CWallet* wallet;
    };

    libzcash::SaplingExtendedSpendingKey ForeignSaplingSpendingKey() {
        return GetTestMasterSaplingSpendingKey().Derive(0x12345);
    }

    void CheckRemovedSaplingRPC(const char* method,
                                std::initializer_list<std::string> arguments) {
        const std::string originalNetwork = Params().NetworkIDString();
        for (const std::string& network : {CBaseChainParams::MAIN,
                                           CBaseChainParams::TESTNET,
                                           CBaseChainParams::REGTEST}) {
            SelectParams(network);
            CRPCTable commands;
            RegisterAllCoreRPCCommands(commands);
            RegisterWalletRPCCommands(commands);

            BOOST_TEST_CONTEXT(network << ": " << method) {
                BOOST_CHECK(commands[method] == nullptr);
                BOOST_CHECK(tableRPC[method] == nullptr);
                for (const std::string& argument : arguments) {
                    JSONRPCRequest request;
                    request.strMethod = method;
                    BOOST_REQUIRE(request.params.read(argument));
                    BOOST_CHECK_EXCEPTION(commands.execute(request), UniValue,
                        [&](const UniValue& error) {
                            return find_value(error, "code").get_int() == RPC_METHOD_NOT_FOUND &&
                                   find_value(error, "message").get_str() ==
                                       std::string("Method not found: ") + method;
                        });
                }
            }
        }
        SelectParams(originalNetwork);
    }

}

BOOST_FIXTURE_TEST_SUITE(sapling_rpc_wallet_tests, WalletTestingSetup)

/**
 * This test covers RPC command validateaddress
 */

BOOST_AUTO_TEST_CASE(rpc_wallet_sapling_validateaddress)
{
    SelectParams(CBaseChainParams::MAIN);
    const auto foreignAddress = ForeignSaplingSpendingKey().DefaultAddress();
    const std::string encodedAddress = KeyIO::EncodePaymentAddress(foreignAddress);
    SelectParams(CBaseChainParams::TESTNET);
    const std::string wrongNetworkAddress = KeyIO::EncodePaymentAddress(foreignAddress);
    SelectParams(CBaseChainParams::MAIN);

    // Keep Sapling decoding coverage while asserting the RPC is not exposed in PQ-only mode.
    BOOST_CHECK(!KeyIO::IsValidPaymentAddressString("not-a-sapling-address"));
    BOOST_CHECK(KeyIO::IsValidPaymentAddressString(encodedAddress));
    BOOST_CHECK(!KeyIO::IsValidPaymentAddressString(wrongNetworkAddress));
    BOOST_CHECK_EQUAL(KeyIO::EncodePaymentAddress(
                          KeyIO::DecodePaymentAddress(encodedAddress)), encodedAddress);
    CheckRemovedSaplingRPC("validateaddress", {"[]", "[null]", "[\"" + encodedAddress + "\"]"});
}

BOOST_AUTO_TEST_CASE(rpc_wallet_getbalance)
{
    CheckRemovedSaplingRPC("getshieldbalance", {"[]", "[null]", "[\"*\"]"});
    CheckRemovedSaplingRPC("listreceivedbyshieldaddress", {"[]", "[null]", "[\"DMKU6mc52un1MThGCsnNwAtEvncaTdAuaZ\",-1]"});
}

BOOST_AUTO_TEST_CASE(rpc_wallet_sapling_importkey_paymentaddress)
{
    const auto foreignKey = ForeignSaplingSpendingKey();
    const std::string encodedKey = KeyIO::EncodeSpendingKey(foreignKey);
    BOOST_CHECK_EQUAL(KeyIO::EncodeSpendingKey(KeyIO::DecodeSpendingKey(encodedKey)), encodedKey);
    BOOST_CHECK(KeyIO::EncodeSpendingKey(KeyIO::DecodeSpendingKey("not-a-spending-key")).empty());
    CheckRemovedSaplingRPC("importsaplingkey", {"[]", "[null]", "[\"" + encodedKey + "\"]"});
}

/*
 * This test covers RPC commands listshieldaddresses, importsaplingkey, exportsaplingkey
 */
BOOST_AUTO_TEST_CASE(rpc_wallet_sapling_importexport)
{
    const std::string encodedKey = KeyIO::EncodeSpendingKey(ForeignSaplingSpendingKey());
    CheckRemovedSaplingRPC("importsaplingkey", {"[]", "[null]", "[\"" + encodedKey + "\"]"});
    CheckRemovedSaplingRPC("exportsaplingkey", {"[]", "[null]", "[\"DMKU6mc52un1MThGCsnNwAtEvncaTdAuaZ\"]"});
    CheckRemovedSaplingRPC("listshieldaddresses", {"[]", "[null]", "[true]"});
}

BOOST_AUTO_TEST_CASE(rpc_wallet_getnewshieldaddress)
{
    CheckRemovedSaplingRPC("getnewshieldaddress", {"[]", "[null]", "[\"label\"]"});
}

BOOST_AUTO_TEST_CASE(rpc_shieldsendmany_parameters)
{
    CheckRemovedSaplingRPC("shieldsendmany", {"[]", "[null]", "[\"from_transparent\",[]]"});
}

// TODO: test private methods
BOOST_AUTO_TEST_CASE(saplingOperationTests)
{
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        m_wallet.SetupSPKM(false);
    }
    auto consensusParams = Params().GetConsensus();
    WalletRegistration walletRegistration(m_wallet);

    // add keys manually
    auto transparentAddress = m_wallet.getNewAddress("");
    BOOST_REQUIRE(transparentAddress);
    const CTxDestination& taddr1 = *transparentAddress.getObjResult();
    const auto& zaddr1 = m_wallet.GenerateNewSaplingZKey();
    std::string ret;

    // there are no utxos to spend
    {
        std::vector<SendManyRecipient> recipients = { SendManyRecipient(zaddr1, COIN, "DEADBEEF", false) };
        SaplingOperation operation(consensusParams, &m_wallet);
        operation.setFromAddress(taddr1);
        auto res = operation.setRecipients(recipients)->buildAndSend(ret);
        BOOST_CHECK(!res);
        BOOST_CHECK(res.getError().find("Insufficient funds, no available UTXO to spend") != std::string::npos);
    }

    // minconf cannot be zero when sending from zaddr
    {
        std::vector<SendManyRecipient> recipients = { SendManyRecipient(zaddr1, COIN, "DEADBEEF", false) };
        SaplingOperation operation(consensusParams, &m_wallet);
        operation.setFromAddress(zaddr1);
        auto res = operation.setRecipients(recipients)->setMinDepth(0)->buildAndSend(ret);
        BOOST_CHECK(!res);
        BOOST_CHECK(res.getError().find("Minconf cannot be zero when sending from shielded address") != std::string::npos);
    }

    // there are no unspent notes to spend
    {
        std::vector<SendManyRecipient> recipients = { SendManyRecipient(taddr1, COIN, false) };
        SaplingOperation operation(consensusParams, &m_wallet);
        operation.setFromAddress(zaddr1);
        auto res = operation.setRecipients(recipients)->buildAndSend(ret);
        BOOST_CHECK(!res);
        BOOST_CHECK(res.getError().find("Insufficient funds, no available notes to spend") != std::string::npos);
    }

    // GetMemoFromString
    {
        std::string memoStr = "Sapling memo!";
        std::array<unsigned char, ZC_MEMO_SIZE> memo;

        BOOST_CHECK(GetMemoFromString(memoStr, memo));
        BOOST_CHECK_EQUAL(memo[0], 0x53);   // S
        BOOST_CHECK_EQUAL(memo[1], 0x61);   // a
        BOOST_CHECK_EQUAL(memo[2], 0x70);   // p
        BOOST_CHECK_EQUAL(memo[3], 0x6C);   // l
        BOOST_CHECK_EQUAL(memo[4], 0x69);   // i
        BOOST_CHECK_EQUAL(memo[5], 0x6E);   // n
        BOOST_CHECK_EQUAL(memo[6], 0x67);   // g
        BOOST_CHECK_EQUAL(memo[12], 0x21);  // !
        for (int i = 13; i < ZC_MEMO_SIZE; i++) {
            BOOST_CHECK_EQUAL(memo[i], 0x00);  // zero padding
        }

        // memo is longer than allowed
        std::vector<char> v (2 * (ZC_MEMO_SIZE+1));
        std::fill(v.begin(),v.end(), 'A');
        std::string bigmemo(v.begin(), v.end());

        OperationResult res = GetMemoFromString(bigmemo, memo);
        BOOST_CHECK(!res);
        const std::string& errStr = res.getError();
        BOOST_CHECK(errStr.find("too big") != std::string::npos);
    }

}


BOOST_AUTO_TEST_CASE(rpc_shieldsendmany_taddr_to_sapling)
{
    {
        LOCK2(cs_main, m_wallet.cs_wallet);
        m_wallet.SetupSPKM(false);
    }
    WalletRegistration walletRegistration(m_wallet);

    UniValue retValue;

    // add keys manually
    auto res = m_wallet.getNewAddress("");
    BOOST_CHECK(res);
    CTxDestination taddr = *res.getObjResult();
    std::string taddr1 = EncodeDestination(taddr);
    auto zaddr1 = m_wallet.GenerateNewSaplingZKey();

    auto consensusParams = Params().GetConsensus();

    // Add a fake transaction to the wallet
    CMutableTransaction mtx;
    mtx.vout.emplace_back(5 * COIN, GetScriptForDestination(taddr));
    // Add to wallet and get the updated wtx
    CWalletTx wtxIn(&m_wallet, MakeTransactionRef(mtx));
    m_wallet.LoadToWallet(wtxIn);
    CWalletTx& wtx = m_wallet.mapWallet.at(mtx.GetHash());

    // Fake-mine the transaction
    BOOST_CHECK_EQUAL(0, chainActive.Height());
    CBlock block;
    block.hashPrevBlock = chainActive.Tip()->GetBlockHash();
    block.vtx.emplace_back(wtx.tx);
    block.hashMerkleRoot = BlockMerkleRoot(block);
    auto blockHash = block.GetHash();
    CBlockIndex fakeIndex {block};
    fakeIndex.nHeight = 1;
    BlockMap::iterator mi = mapBlockIndex.emplace(blockHash, &fakeIndex).first;
    fakeIndex.phashBlock = &((*mi).first);
    chainActive.SetTip(&fakeIndex);
    BOOST_CHECK(chainActive.Contains(&fakeIndex));
    BOOST_CHECK_EQUAL(1, chainActive.Height());
    m_wallet.BlockConnected(std::make_shared<CBlock>(block), mi->second);
    BOOST_CHECK_MESSAGE(m_wallet.GetAvailableBalance() > 0, "tx not confirmed");

    std::vector<SendManyRecipient> recipients = { SendManyRecipient(zaddr1, 1 * COIN, "ABCD", false) };
    SaplingOperation operation(consensusParams, &m_wallet);
    operation.setFromAddress(taddr);
    BOOST_CHECK(operation.setRecipients(recipients)
                         ->setMinDepth(0)
                         ->build());

    // try from auto-selected transparent address
    std::vector<SendManyRecipient> recipients2 = { SendManyRecipient(zaddr1, 1 * COIN, "ABCD", false) };
    SaplingOperation operation2(consensusParams, &m_wallet);
    BOOST_CHECK(operation2.setSelectTransparentCoins(true)
                          ->setRecipients(recipients2)
                          ->setMinDepth(0)
                          ->build());

    // Get the transaction
    // Test mode does not send the transaction to the network.
    auto hexTx = EncodeHexTx(operation.getFinalTx());
    CDataStream ss(ParseHex(hexTx), SER_NETWORK, PROTOCOL_VERSION);
    CTransaction tx(deserialize, ss);
    BOOST_ASSERT(!tx.sapData->vShieldedOutput.empty());

    // We shouldn't be able to decrypt with the empty ovk
    BOOST_CHECK(!libzcash::AttemptSaplingOutDecryption(
            tx.sapData->vShieldedOutput[0].outCiphertext,
            uint256(),
            tx.sapData->vShieldedOutput[0].cv,
            tx.sapData->vShieldedOutput[0].cmu,
            tx.sapData->vShieldedOutput[0].ephemeralKey));

    BOOST_CHECK(libzcash::AttemptSaplingOutDecryption(
            tx.sapData->vShieldedOutput[0].outCiphertext,
            m_wallet.GetSaplingScriptPubKeyMan()->getCommonOVK(),
            tx.sapData->vShieldedOutput[0].cv,
            tx.sapData->vShieldedOutput[0].cmu,
            tx.sapData->vShieldedOutput[0].ephemeralKey));

    // Tear down
    chainActive.SetTip(nullptr);
    mapBlockIndex.erase(blockHash);
}

BOOST_AUTO_TEST_CASE(rpc_listshieldunspent_parameters)
{
    CheckRemovedSaplingRPC("listshieldunspent", {"[]", "[null]", "[1,999,true,[\"DMKU6mc52un1MThGCsnNwAtEvncaTdAuaZ\"]]"});
}

BOOST_AUTO_TEST_SUITE_END()
