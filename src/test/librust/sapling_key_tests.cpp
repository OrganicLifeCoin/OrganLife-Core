// Copyright (c) 2016-2020 The ZCash developers
// Copyright (c) 2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_organiclife.h"

#include "sapling/address.h"
#include "sapling/key_io_sapling.h"

#include <string>
#include <vector>

#include <boost/test/unit_test.hpp>


BOOST_FIXTURE_TEST_SUITE(sapling_key_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(ps_address_test)
{
    SelectParams(CBaseChainParams::REGTEST);

    std::vector<unsigned char, secure_allocator<unsigned char>> rawSeed(32);
    HDSeed seed(rawSeed);
    auto m = libzcash::SaplingExtendedSpendingKey::Master(seed);

    for (uint32_t i = 0; i < 1000; i++) {
        auto sk = m.Derive(i);
        {
            std::string sk_string = KeyIO::EncodeSpendingKey(sk);
            const std::string& hrp = Params().Bech32HRP(CChainParams::SAPLING_EXTENDED_SPEND_KEY);
            BOOST_CHECK(sk_string.compare(0, hrp.size(), hrp) == 0);

            libzcash::SpendingKey spendingkey2 = KeyIO::DecodeSpendingKey(sk_string);
            BOOST_CHECK(IsValidSpendingKey(spendingkey2));

            BOOST_ASSERT(boost::get<libzcash::SaplingExtendedSpendingKey>(&spendingkey2) != nullptr);
            auto sk2 = boost::get<libzcash::SaplingExtendedSpendingKey>(spendingkey2);
            BOOST_CHECK(sk == sk2);
        }
        {
            libzcash::SaplingPaymentAddress addr = sk.DefaultAddress();

            std::string addr_string = KeyIO::EncodePaymentAddress(addr);
            const std::string& addrHrp = Params().Bech32HRP(CChainParams::SAPLING_PAYMENT_ADDRESS);
            BOOST_CHECK(addr_string.compare(0, addrHrp.size(), addrHrp) == 0);

            auto paymentaddr2 = KeyIO::DecodePaymentAddress(addr_string);
            BOOST_CHECK(IsValidPaymentAddress(paymentaddr2));

            BOOST_ASSERT(boost::get<libzcash::SaplingPaymentAddress>(&paymentaddr2) != nullptr);
            auto addr2 = boost::get<libzcash::SaplingPaymentAddress>(paymentaddr2);
            BOOST_CHECK(addr == addr2);
        }
    }
}

BOOST_AUTO_TEST_CASE(EncodeAndDecodeSapling)
{
    SelectParams(CBaseChainParams::MAIN);

    std::vector<unsigned char, secure_allocator<unsigned char>> rawSeed(32);
    HDSeed seed(rawSeed);
    libzcash::SaplingExtendedSpendingKey m = libzcash::SaplingExtendedSpendingKey::Master(seed);

    for (uint32_t i = 0; i < 1000; i++) {
        auto sk = m.Derive(i);
        {
            std::string sk_string = KeyIO::EncodeSpendingKey(sk);
            const std::string& hrp = Params().Bech32HRP(CChainParams::SAPLING_EXTENDED_SPEND_KEY);
            BOOST_CHECK(sk_string.substr(0, hrp.size()) == hrp);

            auto spendingkey2 = KeyIO::DecodeSpendingKey(sk_string);
            BOOST_CHECK(IsValidSpendingKey(spendingkey2));

            BOOST_CHECK(boost::get<libzcash::SaplingExtendedSpendingKey>(&spendingkey2) != nullptr);
            auto sk2 = boost::get<libzcash::SaplingExtendedSpendingKey>(spendingkey2);
            BOOST_CHECK(sk == sk2);
        }

        {
            auto extfvk = sk.ToXFVK();
            std::string vk_string = KeyIO::EncodeViewingKey(extfvk);
            const std::string& fvkHrp = Params().Bech32HRP(CChainParams::SAPLING_EXTENDED_FVK);
            BOOST_CHECK(vk_string.substr(0, fvkHrp.size()) == fvkHrp);

            auto viewingkey2 = KeyIO::DecodeViewingKey(vk_string);
            BOOST_CHECK(IsValidViewingKey(viewingkey2));

            BOOST_CHECK(boost::get<libzcash::SaplingExtendedFullViewingKey>(&viewingkey2) != nullptr);
            auto extfvk2 = boost::get<libzcash::SaplingExtendedFullViewingKey>(viewingkey2);
            BOOST_CHECK(extfvk == extfvk2);
        }

        {
            auto addr = sk.DefaultAddress();

            std::string addr_string = KeyIO::EncodePaymentAddress(addr);
            const std::string& addrHrp = Params().Bech32HRP(CChainParams::SAPLING_PAYMENT_ADDRESS);
            BOOST_CHECK(addr_string.substr(0, addrHrp.size()) == addrHrp);

            auto paymentaddr2 = KeyIO::DecodePaymentAddress(addr_string);
            BOOST_CHECK(IsValidPaymentAddress(paymentaddr2));

            BOOST_CHECK(boost::get<libzcash::SaplingPaymentAddress>(&paymentaddr2) != nullptr);
            auto addr2 = boost::get<libzcash::SaplingPaymentAddress>(paymentaddr2);
            BOOST_CHECK(addr == addr2);
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
