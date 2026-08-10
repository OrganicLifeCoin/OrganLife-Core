// Copyright (c) 2013 The Bitcoin Core developers
// Copyright (c) 2019-2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>

#include "key.h"
#include "key_io.h"
#include "test/test_organiclife.h"

#include <string>
#include <vector>

struct TestDerivation {
    std::string pub;
    std::string prv;
    unsigned int nChild;
};

struct TestVector {
    std::string strHexMaster;
    std::vector<TestDerivation> vDerive;

    explicit TestVector(std::string strHexMasterIn) : strHexMaster(strHexMasterIn) {}

    TestVector& operator()(std::string pub, std::string prv, unsigned int nChild) {
        vDerive.emplace_back();
        TestDerivation &der = vDerive.back();
        der.pub = pub;
        der.prv = prv;
        der.nChild = nChild;
        return *this;
    }
};

TestVector test1 =
  TestVector("000102030405060708090a0b0c0d0e0f")
    ("sqmNqPMpujqoPFVGct12KxMiiow1SqWxwg3JjSCYHbgUK3qA6HtEBFtdemKn84ybtScZuFLLcMWG7CWFQgeGm9pHh82gc97GNGa46oXpDXM6au6",
     "sqmNyh4WeeM9hFceHrxM7aVJm5ePTqCQhYWMwWsimjKAAz6a3yorG7Pxn1a6a4mz8MvBZStwJBMNqdhtRbMwMykxa4XW3zBVRAApbcnP2oMrLNX",
     0x80000000)
    ("sqmNsQxo4Hh32HqZHtRbKp1UdJpPZtDGXcFtMRBsWfcfkasJpQVvkGA1b7KwWCJk4YFqUyAwEF59nYu7W4s1LFmBHyf811jPubo4bVHTTGWZBiw",
     "sqmP1ifUoCCPLHxvxsNv7S94faXmastiHUiwZVs3zoFMcX8in6RYq7fLgxeFc1taTu2JzPQrp5VDmbwqpqTHkKCZuQcgkrusbxbytyAQQSc3cjR",
     1)
    ("sqmNsiqaiF7VHxzfhcKgcjeLihtuLeB1tnBmcVWiZvHKWS6ezbVdu4ecbXJV7EUQjmEMaW3Cqx1LiAre9CikT1UihQExPZVx9rVCEHHjAGcYvnt",
     "sqmP22YGT9cqby83NbH1QMmvkycHMdrTeeeppaBu43v1NNN4xHRFyv9wZ5ZvYTSFPdvHSQ2wNHA6HanHTghGx85f3RHqyEeACCsEy368mKK7e4t",
     0x80000002)
    ("sqmNyUq8a3VSe45MAEJTaTnYtrqc2aUDDGv6ejBCPKWfB7Du6WWva838Yx4h3GwMEQNRrJiERPZLwtvmwRR4MQPqJhMNJSMmZHnR3xRax6LUWJe",
     "sqmP7nXpJwznx4CiqDFnN5v8w8Yz3a9ey9P9rorNsT9M33VK4CSYeyYTdFYP46ytLc2oqCXTTBR5MCtmmxs7gHTVdn36vwD5qtQEJ6PMuwofsHq",
     2)
    ("sqmNqmwTDA4i1QuxojTCkkbtKDSS7MTL684McZeYWvRX2NgiVfkAVEbi2bPKok5VDbqe1Y3Cm3rFatba8hprmGHutiKWNhw8eNGeyyt8qpTrRcv",
     "sqmNz5e8x4a4KR3LUiQXYNjUMV9p8M8mqzXQpeKj144CtJx8TMfna67334qSUwjzY9cbwbcFejv9GkZu3DjRrhQgMCQB8RCoqXQ7zZnQ1LZwCMz",
     1000000000)
    ("sqmNxQEW6nNVTPW1gHoJvGZpA6TgdZjcoa3aEAqC9Mgb7qfBEg78jjVCMi3SLGRtV6tmoo1zdkBNmpKq3i8XwH6F8a3ngBxaa5JJYeT8a9q4Cga",
     "sqmP6hwBqgsqmPdPMGkdhthQCNB4eZR4ZSWdSFWNdVKGymvbCN2kpazXaPuXYrBrKcjr7TURaPNFDG6j7sE4YRBPozuVvs4MsnZiLUGXXiiqYLP",
     0);

TestVector test2 =
  TestVector("fffcf9f6f3f0edeae7e4e1dedbd8d5d2cfccc9c6c3c0bdbab7b4b1aeaba8a5a29f9c999693908d8a8784817e7b7875726f6c696663605d5a5754514e4b484542")
    ("sqmNqPMpujqkr4Bco1WNYoTHkb83ZueVTSktkWqUC15yb71Ck2LtEg4eo52DVYiywQQ39tRQiRv5z5mWx347gUMDp7qr6A89GvjKSqXzsQ5J3MU",
     "sqmNyh4WeeM7A4JzTzThLRasnrqRauKwDKDwxbWeg8ifT3GchiGWKXZyfCvZEWKdAWK1PbUkDu3UoRSPyWmEJ5YtQHsx1L6VQvRJczYMMAcbjmf",
     0)
    ("sqmNxpFrUTNhq12Rbw7Ryu5CFm2YchLVq47JyekwcrBBpU2i5WjCjMSgcv3MtcagdXbvkMZb4JQZ3ZPyyzSN93U45KxCXrHWJbLXMFDACwpUrWf",
     "sqmP77xYDMt4919oGv4kmXCnJ2jvdh1wavaNBjS86yosgQJ83CeppCx1kADCB8cJN7wJHgbMFkmzRGc7fJgiRxNp98AdEAZVppxthKarhatZpr1",
     0xFFFFFFFF)
    ("sqmNv2SCexrZckYqRMr4UFqikqcmVmBngSnyzRk146EdwbDV4rVvQy1rhG4CSKQgYaP7c1YMRwSReVgQHQ53x6iFzmsomKnWuwfV1S5obmeWF65",
     "sqmP4L8tPsMuvkgD6LoPFsyJo7L9WksESKG3CWRBYDsKoXUu2YRYVpXBcysJ7ik9SpLMLwy3hD9Np1fFtJSYjH14LxypztFzwMXZKcT5kGBEh1n",
     1)
    ("sqmNsArH7uKMn2vd8t19jc1jU8PWyoydZH5H2AZ5CWVWyYw2CWAVx5NH9dJeLGzZK3aQ6KBJQv7ghtaevYtCiawCDpkcPEWfbMpD6bNG8gTMQu3",
     "sqmP1UYxropi633zorxUXE9KWQ6tzof5K9YLEFEFge8CqVCSAC682vsc5QJXSF55JMWHPetzY1tjYs9nw3HnndPXvBBxanjYkpEFUXWEwPX8SJT",
     0xFFFFFFFE)
    ("sqmNsJNpZ3NRDPven3Z3tYq4yuSoPEqC2hdMPWgsbV5Fdr5JVzUGGxA1ksgD5Srwgf2jFmexesnMZmRDefcrob5vwRjtXovR2bejUA74z8PaxHp",
     "sqmP1c5WHwsmXQ42T2WNgAxf2BABQEWdna6QbbN45chwVnLiTgPtMofLyg2Ni9XkPQn87fnHCkgPvHtgGyxyGCunrnzXhZxSxNgY16UBiwf5Vgw",
     2)
    ("sqmNwMakr96EdQ9yqUNp23yci8WkMukYMu36zm6XgbMakEshT7YAxugaFpezdRiyaVSYVcfSJr88EkBvkP5SM6yLArLp1hu8Tcm8GQPeBh3CL1d",
     "sqmP5fHSb3bawQHMWTL8og7CkQE8NuRz7mWACqmiAizGcB97QoTo3mBuYYEEFk9R7MWABf7UiQcgzwHNboXGMvRjUsvtV3X4kktVb8obFEE9r4K",
     0);

TestVector test3 =
  TestVector("4b381541583be4423346c643850da4b320e46a87ae3d2a4e6da11eba819cd4acba45d239319ac14f863b8d5ab5a0d0c64d2e8a1e7d1457df2e5a3c51c73235be")
    ("sqmNqPMpujqeh7A7AAze6aY6J8Yw74Y7R864jErPUf15iwHbYKF8WJdW1oKRCJD9neCuJXMcVBaF7e3cX6zjzD4gK6toum9YqixcbbCZCYLYVrm",
     "sqmNyh4WeeM117HUq9wxtCfgLQGK84DZAzZ7wKXZxndmasZ1W1AkbA8puCTRVybZ8emK46F4XtD9LoEBJ7DEQwHnMpsndiJSx8iR9kV78hM7vrz",
      0x80000000)
    ("sqmNszqEWR1b8p72NDjFqHw8itWW9y2jSnrASS3WLeTxJGqQiVHcC545cXEoYfEnkrBhpu95dBFPhpJZGkrAPCDTeTcEsAazgtiYgvdpVfXSj7c",
     "sqmP2JXvFKWwSpEQ3Cgacv4imADtAxiBCfKDeWigpn6eAD6pgBDEGvZQnNgzfib5VQPzYYzwuRuToc4UWaAkoBNWTPK282B8fZehPMduq7uyNCF",
      0);

static void RunTest(const TestVector &test) {
    std::vector<unsigned char> seed = ParseHex(test.strHexMaster);
    CExtKey key;
    CExtPubKey pubkey;
    key.SetSeed(seed.data(), seed.size());
    pubkey = key.Neuter();
    for (const TestDerivation &derive : test.vDerive) {
        unsigned char data[74];
        key.Encode(data);
        pubkey.Encode(data);

        // Test private key
        BOOST_CHECK(KeyIO::EncodeExtKey(key) == derive.prv);
        BOOST_CHECK(KeyIO::DecodeExtKey(derive.prv) == key); //ensure a base58 decoded key also matches

        // Test public key
        BOOST_CHECK(KeyIO::EncodeExtPubKey(pubkey) == derive.pub);
        BOOST_CHECK(KeyIO::DecodeExtPubKey(derive.pub) == pubkey); //ensure a base58 decoded pubkey also matches

        // Derive new keys
        CExtKey keyNew;
        BOOST_CHECK(key.Derive(keyNew, derive.nChild));
        CExtPubKey pubkeyNew = keyNew.Neuter();
        if (!(derive.nChild & 0x80000000)) {
            // Compare with public derivation
            CExtPubKey pubkeyNew2;
            BOOST_CHECK(pubkey.Derive(pubkeyNew2, derive.nChild));
            BOOST_CHECK(pubkeyNew == pubkeyNew2);
        }
        key = keyNew;
        pubkey = pubkeyNew;
    }
}

BOOST_FIXTURE_TEST_SUITE(bip32_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(bip32_test1) {
    RunTest(test1);
}

BOOST_AUTO_TEST_CASE(bip32_test2) {
    RunTest(test2);
}

BOOST_AUTO_TEST_CASE(bip32_test3) {
    RunTest(test3);
}

BOOST_AUTO_TEST_SUITE_END()
