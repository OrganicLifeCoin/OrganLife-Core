// Copyright (c) 2011-2013 The Bitcoin Core developers
// Copyright (c) 2017-2020 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//
// Unit tests for block-chain checkpoints
//

#include "checkpoints.h"

#include "chainparams.h"
#include "uint256.h"
#include "test_organiclife.h"

#include <boost/test/unit_test.hpp>


BOOST_FIXTURE_TEST_SUITE(Checkpoints_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(sanity)
{
    const uint256 genesisHash = Params().GetConsensus().hashGenesisBlock;
    const uint256 wrongHash = uint256S("0x0000000000000000000000000000000000000000000000000000000000000001");

    BOOST_CHECK(Checkpoints::CheckBlock(0, genesisHash));
    BOOST_CHECK(!Checkpoints::CheckBlock(0, wrongHash));

    // ... but any hash not at a checkpoint should succeed:
    BOOST_CHECK(Checkpoints::CheckBlock(1, wrongHash));

    BOOST_CHECK_EQUAL(Checkpoints::GetTotalBlocksEstimate(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
