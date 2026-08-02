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
    ("sP2tJC4YKGcB9dvbHEmKDTKhNX2ZWNdKm84qfbUYnj6QWEe4GR8toAN3Qq1uwEzxk8cpVSDiWdg4QRFtSmQzxdgNPc4UHfE7DSFmL4HmPebagqS",
     "sP2tSVmE4B7XTe3xxDie15THQnjwXNJmWzXtsg9jGrj6NAuUE74Wt1sP3XKJG2R4qYvGQ6muXcfwocuHV1iWphqZboqFYvMFvQVQAfRFRhHr3LD",
     0x80000000)
    ("sP2tJEKxJyueEu3aRqu2RDCLFZ8fqCBg9CYrMCuSkpUXURqLdhS5DPu8XyNKiWZfVSXMVeWHNsDV94KyRAP8ufQkTVXCVJ2N8k9yqzRKhJq28e9",
     "sP2tSY2e3tQzYuAx6prMCqKvHpr3rBs7u51uZHadEx7DLN6kbPMhJFQUAffi3HmQFGJE79fbtuzpBykACmkEUJ7kPsumco2QANCHsg46rrecj4v",
     1)
    ("sP2tJGW5WmTXdbveGHwU9U6CrJ2KtaAn9EH9HXaHM6FLR4dYsEwCwCKSVYGZ8b2EuPEhTN8LAHfbFepT1U7xNzae99M8nVsDcPAFUix6zFVovST",
     "sP2tSaCmFfxswc41wGtnw6DntZjhuZrDu6kCVcFTqDt2Gztxpvrq23pn8EZwTLxTrC5GZERHhjbFGghaLiGTbdujB3QD8Zz8E7y9YUNUPiBG2or",
     0x80000002)
    ("sP2tJK7MZJHPLUn48TjZQ4VaDJ4U7j3JQms8d4oxbRYVavzzH68Kjx9sPypUgcD5MFg1otE4mfeCdbPRToGF668BCbVyNPqMhLDwCQsF5UAWr4C",
     "sP2tScp3JCnjeUuRoSgtBgdAFZmr8iikAeLBq9V95ZBBSsGQEn3wpofD2g7s1PB5pRam4hSgJCYAh6XUZv1DBepvJ8qsVBYgP4D69k7h2Nce2rX",
     2)
    ("sP2tJMLkPjQLWxT3CwDH6Khf6oDnCYL33NBL4YhFrP4xuNZbeBkgoirEbZHQMwCTm3pkHCMEug89gLHY2DcFBPZNvByEHBPYXX7mD6n6E1itUyg",
     "sP2tSf3S8dugpxaQsvAbswqF94wADY1UoEePGdNSLWhemJq1bsgJtaMaEFanghaYH5zC8VaNv7mjbuye36ufP5NGVXRutRdmw92oJiarXsnRRGC",
     1000000000)
    ("sP2tJP4WsR1ae5qEjGzjCsGPQWDmzmuLrrzMro5p6fT77Sx6MKA5L96trfSTzm7QcJuTktdnR23xDXz8ce1GHF4DmT8M9iodKzhQ24Q1UCRUEdW",
     "sP2tSgmCcKWvx5xcQFx3zVPySmwA1mancjTR4skzao5nyPDWK15hQzcEVMjrKZMkC17GjtydhUkgSxWm3Gv2v9NzcV5TDvQ143nWb49DGygUHLX",
     0);

TestVector test2 =
  TestVector("fffcf9f6f3f0edeae7e4e1dedbd8d5d2cfccc9c6c3c0bdbab7b4b1aeaba8a5a29f9c999693908d8a8784817e7b7875726f6c696663605d5a5754514e4b484542")
    ("sP2tJC4YKGcB9dvbGrGeKcPypikKkZmkSkKfMBGH5fqEUckvZQA5yK8yqjBWHM5wTNynPzTWRJLDoVm9AZ9E5pbXXX4R9bRa3vkReRxsfWqEjeT",
     "sP2tSVmE4B7XTe3xwqDy7EXZrzThmZTCCcniZFwTZoTvLZ2LX65i4AeKURUtc6C824UMe9XLGu8Q5iwUaYScs8EGDBA21LRW4kQ8jv3Unekv6TA",
     0)
    ("sP2tJFLHazJ8dekfo7rwTPmrtX9r7XX19iowuM1eFUrgZ2ctFNc4YaV3zZkkrp3qFTzDcJwgHXKyCac7KiZuxyHws7ihReAExGtomv5Y6m31Aj2",
     "sP2tSZ2yKtoUwet3U6pGF1uSvnsE8XCSubH17RgpjcVNQxtJD4XgdRzPdG49BbTvf8KC3omAAuY8WtMbw45SRQjK9QKR1WqZTEZssKx39uYjp5g",
     0xFFFFFFFF)
    ("sP2tJGVLqbKf1Yvmxz57sqmWMrqQWGjuhCydzW6BAsmTCmPGhfqXATuNCdYMUCHx1CbKPpsDwNBaVo2hzPSFjsn4bGBKxinJK19peJM8wYByo9r",
     "sP2tSaC2aVq1KZ49dy2SfTu6Q8YnXGRMT5ShCamMf1Q94hegfMm9FKQhqKqjnwwuPP3fx363wvjmZtriNHpD9nLSihkySF4uDkJ8f7HZvPNtbd6",
     1)
    ("sP2tJKJKFcJbBCDy4bkFwVv5KomdqocQoPtuJwb31wRnWejiNVhgTyS56XvjJzV5o5Hbz1Kmn1Gp8vLY5BhGenD1rbeEYzz9XYb2a526BxHxyqx",
     "sP2tSczzzWowVCMLjahaj83fN5V1roHrZGMxX2GDW54UNb18LBdJYpwQjEE7dk9XR6FNFzSkhDp9ySXvgzbNCfrpjBaGa1bi4rGX8uiJQ22QVqG",
     0xFFFFFFFE)
    ("sP2tJLUMAaEwnbBgMYyrfyjchKTpkY5mrNXQLeJvYf8CMQzxpr2cZeN5d4HpwecazSp3wSyQaWLXBXfPiprfLfKXZxLgpuJe5qgSSJ6fixSbZ42",
     "sP2tSeB2uUkJ6bK42XwBTbsCjbBCmXmDcEzTYiz72nktDMGNnXxEeVsRFkbDGSsv3WdeoZjT13b9Mp7WW7gjicmKR2FSPx92D4J9UVipTm957CV",
     2)
    ("sP2tJMqP81TTJmVv6pAkQ3sJg4qo9bct6hSUzovoxSUgzCrYu3NBjHHqodEq23gs82AkTWBdrRqw5EgPDFvAAm1RtDCGU58j2CyB7bHrPYELSj6",
     "sP2tSfY4ruxocmdHmo85BfztiLZBAbJKrZuYCtbzSa7Nr97xrjHop8oBSKYDLrYvCXiiqsbBHoDxouxCYfaFSw8YTzzACoeXGURsAs4NYMkKtsc",
     0);

TestVector test3 =
  TestVector("4b381541583be4423346c643850da4b320e46a87ae3d2a4e6da11eba819cd4acba45d239319ac14f863b8d5ab5a0d0c64d2e8a1e7d1457df2e5a3c51c73235be")
    ("sP2tJC4YKGcB9dvbFuioqztn2i1MdD6rCcUw4X2qUxrnkJqiXePMdTNwnzR2mLcvGyZRa5DNFJDECAazbrn8vXdxwriZGqKnnVCGS9UaC1ayEzd",
     "sP2tSVmE4B7XTe3xvtg8dd2N4yijeCnHxUwzGbi1y6VUcF78VLJyiJtHRgiR65vHy5hgirC5qppVz9gBwC3w6ZNgzefGXcz9GBk72dhqSZkR45A",
      0x80000000)
    ("sP2tJERk4ESJq1HwTSpD59EH5xm2KZiEMTtqU1GnJCYdgcm5nfWpCx9osTCk54TS1LAAmSiD2Fxy37nVEKiUrma9X6zVwwGgLLh4vKqUrQMRdd1",
     "sP2tSY8Ro8wf91RK8RmXrmMs8EUQLZPg7LMtg5wxnLBKYZ2VkMSSHof9W9W8PrGeK32cAzqVrmcNdtJeajh5fmxSkQrtxkuC1M1v9efVPxtasXk",
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
