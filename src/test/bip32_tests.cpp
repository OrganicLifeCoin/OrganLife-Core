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
    ("sqmNqPMpujqeaDkkJf5KgDx4sPgqmBPyaCZ7LAFFDj6BVmV5oRfNwW5FKibcR8tt22MN7UFXFXudY8ZbEVKCVx1Evb7DU9TmorJE8niuDU13w4y",
     "sqmNyh4WeeLztDt7ye2eTr5eufQDnB5RL52AYEvRhrisMhkVm7b12MaaxQtzjvJz7Sep28oiGWuWwLCzGjciN2AS8nszjQavWpXryPrPFUakGjE",
     0x80000000)
    ("sqmNqRdEuT97fUsjTGD2syphkRnx5zxKxH381mg9BpUJTxgNAhxZMjcLSrx2CQTamLFu7gY67mT4GmdgCtHLSyjczUZwfnG2jACSeirTX5m8YXw",
     "sqmNyjKveMeTyV178FAMfbxHnhWL6zdmi9WBDrMKfx6zKtwn8PtBSb7g5ZFQXBfKXA2mjBhQdpEPKh3rzVeS1cScvrxWoHG4knEkgQVEgckMxdY",
     1)
    ("sqmNqToN7Eh14BkoHiFUcEiaMAgc9NwRxJmQx6Lyn6F7QbUaQFTh5Y2eQRrFcUvABGyF5QA8uBuAPN89oC29vJuWg8Psxz6tCoCiHTPEp1WtvxL",
     "sqmNymW3r9CMNBtAxhCoPrrAPSPzANcsiBEUAB2AGDsoGXjzMwPKAPXz389dwErP85opBGT6SdppQQ1H8SAf8xEbi2SxK4DnpY1cMCocDYxAn48",
     0x80000002)
    ("sqmNqWQe9mWrm4cD9t3Zrq7wiAikNXoxDrMQHdaf2RYGaTr1p6eotHs5JsQBAW6zd9QZRvFsWZsmmJh8FXASdQT3jaYiYt52HkGQ19JNuJZMhrJ",
     "sqmNyp7Ktg2D54japrzteTFXkSS8PXVPyipTViFqWZAxSQ7RmnaRy9NQwZhZVH516KKJgjUV36mjpoqBMduQiy9nq7tcffnLyUFYxUYpr7PjE6Q",
     2)
    ("sqmNqYe2zCdowYHCEMXHZ6L2bft4TM6grSfbj7TxHP4jtuQdBCHAx4ZSWSs6qq6P2wZHuEP3eaMip3bEowWSihtFTB1yTfdD7wAE1qDE3sAR8Mc",
     "sqmNyrLij79AFYQZuLUcLiTcdwbSULn8cK8ewC98mWhRkqg38tCo2v4n99AVAbUTYyijkXcBf21JjdHLpporvPh92WUf4usSXZ5G7T1zMdp1GUB",
     1000000000)
    ("sqmNqaMoTtF44ffPkhJjfdtkuNt4FafzfwUdXMrWXfSt6yo7tKgZUUp6mZ2AUf1KtCe1Nvfb9vHXMFHqQMuTpZP6JSB6LD3HvQjrpnq9J1ddi1F",
     "sqmNyt4VCnkQNfnmRgG4TG2LwebSGaMSRowgjSXh1o5Zxv4Xr1cBZLKSQFKYoTFfTtqpMw1SSNzFafpTpzpETThs9U8CQQdfeTpyPnaM6oeLpmM",
     0);

TestVector test2 =
  TestVector("fffcf9f6f3f0edeae7e4e1dedbd8d5d2cfccc9c6c3c0bdbab7b4b1aeaba8a5a29f9c999693908d8a8784817e7b7875726f6c696663605d5a5754514e4b484542")
    ("sqmNqPMpujqeaDkkJGaenP2MKbQc1NYQFpow1k2yWfq1U9bx6Qga7erBkcmCmEyrjGiL22VKACZnwD4qxH3Rd8vQ4W7AL5fEeLntTAQ1VQaSDun",
     "sqmNyh4WeeLztDt7yFXya19wMs7z2NDr1hGzDpi9zoThL5sN46cCCWMXPK4b5z63HxCuGBZ91oMyDSFBNGLpQSZ8kACmBpfAfASbYeUccNi4Q7Z",
     0)
    ("sqmNqSdaBTXc4EappYAwvAQEPPp8NLHexoJDZunLgUrTYZTunP8YgvCFuTLTLhwkXMimELyV2RZYLHup7SU7WHcpQ6mSc8PuYgwGaeWfvbiD9Yk",
     "sqmNykLFvN2xNEiCVX8GhnXpRfXWPKy6ifmGmzTXAcV9QVjKk54AmmhbY9dqfVMqw23jfqnxuomhebfJimydxj4BgPNAC15E3ecLg4PAykp9enX",
     0xFFFFFFFF)
    ("sqmNqTndS4Z8S8kvzQP8LcPsrjVgm5WZWHTuf4rsbsmECJEJEgN1Joca7X83x6BsH6Ks1ru2gGR9dWLQn7LTHC6w8FE59D1xuRCHT2nGmNQ3qUr",
     "sqmNymVKAy4Uk8tJfPLT8EXTu1D4n5C1G9vxs9Y461Pv4EViCNHdPf7ukDRSGqqpfGnDa57rgpyLhcARA1iQh6fKFgoicjJZpALbTqihk8mp3ox",
     1)
    ("sqmNqWbbr5Y4bn48624GQGYSpgRv6cP4cUPAyWMjSwRZWBajuWEAcK9H1RWRntP14y29c3MaWuWPGdeErubUC6XtPagyjVDp7xdVNoTE1rAipAx",
     "sqmNypJHaz3QunBVm11bBtg2rx9J7c4WNLrEBb2uw54FN7r9sC9nhAece7op7e3Sgyyut2UZS83j79qdUiVZjzBhGAd1kVqNfGJywe9SDqLYB7T",
     0xFFFFFFFE)
    ("sqmNqXmdm3URDB1qNyHs8kMzCC871LrRfT1g1D5cyf7yLwqzMrZ6hz5HXwsXRYWWGLYbZV1DKQa6KEy6WYkrsyeQ6wPS1PYJgFiuF2XoYhLx4MW",
     "sqmNyqUKVwymXB9D3xFBvNVaETqV2LXsRKUjDHkoTnkfCt7QKYUinqadAeAukLmqKQNCRbmFjwpiVXRDHqawFw6Bx1JBaSNgoULcHE9xHZu3JT5",
     2)
    ("sqmNqZ8fiUgvjML58EUkrpVgAwW5QQPXumvkfNhWPSUTyjhaS3tfsd13iWpXVwanPuuJ5YDSbL5WCwz5zypMi5LJRCF1eZNPcd1dvKizDM8EyqP",
     "sqmNyrqMTPCH3MTSoDS5eSdGDDDTRQ4yfePosTNgsa79qfxzPjpHxUWPMD7upkSqURTGTucz2hTXwdFuLPUSzFTQzz2uPHtBrtUKybVWN8con72",
     0);

TestVector test3 =
  TestVector("4b381541583be4423346c643850da4b320e46a87ae3d2a4e6da11eba819cd4acba45d239319ac14f863b8d5ab5a0d0c64d2e8a1e7d1457df2e5a3c51c73235be")
    ("sqmNqPMpujqeaDkkHL2pJmX9Xafdt1sW1gyCj5oXuxrZjqgk4euqmo69hszjFEWqYsHyC7FAzCSoKsthPagLTqxqUqmJTKZTNuEjEsui1oVDTTp",
     "sqmNyh4WeeLztDt7xJz96PejZrP1u1YwmZSFwAUiQ6VFbmxA2LqTrebVLaJ7ZypDEySELtDtaj457rytiux8dshZXdi1i7DorbnZqN8yGQU4Wu8",
      0x80000000)
    ("sqmNqRj2ehfnFb86Us8DXureaqRJaNUtAYP78a3UjCYQg9c7Kg3JMHs1nLnSYxMMHDtiPUk1mACYAq6C23cgQ5u2463F8RWLvkjXj4GcgEGWeNY",
     "sqmNyjRiPcB8ZbFU9r5YKXzEd78gbNAKvQrALeifDLB6Y5sXHMxvS9NMR35pskAZavm9o2sJbfqwmbcMNTbHD6HKHPue9F8rbm4NxP6dDob2Xvs",
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
        BOOST_CHECK_EQUAL(KeyIO::EncodeExtKey(key), derive.prv);
        BOOST_CHECK(KeyIO::DecodeExtKey(derive.prv) == key); //ensure a base58 decoded key also matches

        // Test public key
        BOOST_CHECK_EQUAL(KeyIO::EncodeExtPubKey(pubkey), derive.pub);
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
