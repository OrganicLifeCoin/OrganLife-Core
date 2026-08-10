#!/usr/bin/env python3
# Copyright (c) 2021 The PIVX Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import PivxTestFramework
from test_framework.util import (
    assert_equal,
    set_node_times,
)

import time

class GovernanceInvalidBudgetTest(PivxTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        # 3 nodes:
        # - 1 miner/mncontroller
        # - 2 remote mns
        self.num_nodes = 3
        self.extra_args = [["-sporkkey=932HEevBSujW2ud7RfB1YF91AFygbBRQj3de3LyaCRqNzKKgWXi"],
                           ["-listen", "-externalip=127.0.0.1"],
                           ["-listen", "-externalip=127.0.0.1"],
                           ]
        # v5_shield at 249 (standard SAPLING-version txes) and v6_evo at 250
        # (post-v6 semantics apply to all the PoS blocks)
        for i in range(self.num_nodes):
            self.extra_args[i] += ["-nuparams=v5_shield:249", "-nuparams=v6_evo:250"]
        self.enable_mocktime()

        self.minerAPos = 0
        self.remoteOnePos = 1
        self.remoteTwoPos = 2

    def run_test(self):
        self.minerA = self.nodes[self.minerAPos]     # also controller of mn1 and mn2
        self.mn1 = self.nodes[self.remoteOnePos]
        self.mn2 = self.nodes[self.remoteTwoPos]
        self.setupContext()

        # Create a valid proposal and vote on it
        next_superblock = self.minerA.getnextsuperblock()
        payee = self.minerA.getnewaddress()
        self.log.info("Creating a proposal to be paid at block %d" % next_superblock)
        proposalFeeTxId = self.minerA.preparebudget("test1", "https://test1.org", 2,
                                               next_superblock, payee, 300)
        self.stake_and_ping(self.minerAPos, 3, [self.mn1, self.mn2])
        proposalHash = self.minerA.submitbudget("test1", "https://test1.org", 2,
                                           next_superblock, payee, 300, proposalFeeTxId)
        time.sleep(1)
        self.stake_and_ping(self.minerAPos, 7, [self.mn1, self.mn2])
        self.log.info("Vote for the proposal and check projection...")
        self.minerA.mnbudgetvote("alias", proposalHash, "yes", self.dmn1.proTx)
        self.minerA.mnbudgetvote("alias", proposalHash, "yes", self.dmn2.proTx)
        time.sleep(1)
        self.stake_and_ping(self.minerAPos, 1, [self.mn1, self.mn2])
        projection = self.mn1.getbudgetprojection()[0]
        assert_equal(projection["Name"], "test1")
        assert_equal(projection["Hash"], proposalHash)
        assert_equal(projection["Yeas"], 2)

        # Try to create an invalid finalized budget, paying to an nonexistent proposal
        self.log.info("Creating invalid budget finalization...")
        self.stake_and_ping(self.minerAPos, 5, [self.mn1, self.mn2])

        budgetname = "invalid finalization"
        blockstart = self.minerA.getnextsuperblock()
        proposals = []
        badPropId = "aa0061d705de36385c37701e7632408bd9d2876626b1299a17f7dc818c0ad285"
        badPropPayee = "8c988f1a4a4de2161e0f50aac7f17e7f9555caa4"
        badPropAmount = 500
        proposals.append({"proposalid": badPropId, "payee": badPropPayee, "amount": badPropAmount})
        res = self.minerA.createrawmnfinalbudget(budgetname, blockstart, proposals)
        assert res["result"] == "tx_fee_sent"
        feeBudgetId = res["id"]
        time.sleep(1)
        self.stake_and_ping(self.minerAPos, 4, [self.mn1, self.mn2])
        res = self.minerA.createrawmnfinalbudget(budgetname, blockstart, proposals, feeBudgetId)
        assert res["result"] == "error"  # not accepted

        self.log.info("Good, invalid budget not accepted.")

    def setupContext(self):
        # First mine 250 PoW blocks (250 with minerA)
        self.log.info("Generating 259 blocks...")
        for _ in range(250):
            self.mocktime = self.generate_pow(self.minerAPos, self.mocktime)
        self.sync_blocks()
        # Then stake 9 blocks with minerA
        self.stake_and_ping(self.minerAPos, 9, [])
        for n in self.nodes:
            assert_equal(n.getblockcount(), 259)
        # Re-anchor the mocktime to the tip time: the fork's block times follow the
        # adjusted time only when it is ahead of the tip (otherwise the 15s time
        # slots apply, breaking time-based governance rules such as the 5min proposal
        # establishment).
        self.mocktime = self.minerA.getblock(self.minerA.getbestblockhash())["time"] + 60
        set_node_times(self.nodes, self.mocktime)

        # Setup Masternodes (DMN registration on-chain, collateral embedded)
        self.log.info("Masternodes setup...")
        self.dmn1 = self.register_new_dmn(self.remoteOnePos, self.minerAPos, self.minerAPos, "fund")
        self.dmn2 = self.register_new_dmn(self.remoteTwoPos, self.minerAPos, self.minerAPos, "fund")
        self.sync_all()
        self.mn1.initmasternode(self.dmn1.operator_sk)
        self.mn2.initmasternode(self.dmn2.operator_sk)

        # Activate masternodes
        self.log.info("Masternodes activation...")
        self.stake_and_ping(self.minerAPos, 1, [])
        self.wait_until_mnsync_finished()
        for n in self.nodes:
            assert_equal(n.getmasternodecount()["enabled"], 2)
            assert_equal(n.getmasternodecount()["total"], 2)
        self.log.info("Masternodes enabled.")

        # activate sporks
        self.log.info("Activating sporks.")
        self.activate_spork(self.minerAPos, "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT")
        self.activate_spork(self.minerAPos, "SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT")
        self.activate_spork(self.minerAPos, "SPORK_13_ENABLE_SUPERBLOCKS")


if __name__ == '__main__':
    GovernanceInvalidBudgetTest().main()
