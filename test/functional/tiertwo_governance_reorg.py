#!/usr/bin/env python3
# Copyright (c) 2021 The PIVX Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.

from decimal import Decimal
import time

from test_framework.test_framework import PivxTestFramework
from test_framework.authproxy import JSONRPCException
from test_framework.util import (
    assert_equal,
    set_node_times,
    wait_until,
)


class GovernanceReorgTest(PivxTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        # 4 nodes:
        # - 1 miner/mncontroller
        # - 2 remote mns
        # - 1 other node to stake a forked chain
        self.num_nodes = 4
        self.extra_args = [["-sporkkey=932HEevBSujW2ud7RfB1YF91AFygbBRQj3de3LyaCRqNzKKgWXi"],
                           ["-listen", "-externalip=127.0.0.1"],
                           ["-listen", "-externalip=127.0.0.1"],
                           [],
                           ]
        # v5_shield at 249 (standard SAPLING-version txes) and v6_evo at 250
        # (post-v6 semantics apply to all the PoS blocks)
        for i in range(self.num_nodes):
            self.extra_args[i] += ["-nuparams=v5_shield:249", "-nuparams=v6_evo:250"]
        self.enable_mocktime()

        self.minerAPos = 0
        self.minerBPos = 1
        self.remoteOnePos = 1
        self.remoteTwoPos = 2

    def run_test(self):
        minerA = self.nodes[self.minerAPos]     # also controller of mn1 and mn2
        minerB = self.nodes[self.minerBPos]
        mn1 = self.nodes[self.remoteOnePos]
        mn2 = self.nodes[self.remoteTwoPos]

        # First mine 250 PoW blocks (50 with minerB, 200 with minerA)
        self.log.info("Generating 259 blocks...")
        for _ in range(2):
            for _ in range(25):
                self.mocktime = self.generate_pow(self.minerBPos, self.mocktime)
            self.sync_blocks()
            for _ in range(100):
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
        self.mocktime = minerA.getblock(minerA.getbestblockhash())["time"] + 60
        set_node_times(self.nodes, self.mocktime)

        # Setup Masternodes (DMN registration on-chain, collateral embedded)
        self.log.info("Masternodes setup...")
        self.dmn1 = self.register_new_dmn(self.remoteOnePos, self.minerAPos, self.minerAPos, "fund")
        self.dmn2 = self.register_new_dmn(self.remoteTwoPos, self.minerAPos, self.minerAPos, "fund")
        self.sync_all()
        mn1.initmasternode(self.dmn1.operator_sk)
        mn2.initmasternode(self.dmn2.operator_sk)

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

        # Create a proposal and vote on it
        next_superblock = minerA.getnextsuperblock()
        payee = minerA.getnewaddress()
        self.log.info("Creating a proposal to be paid at block %d" % next_superblock)
        proposalFeeTxId = minerA.preparebudget("test1", "https://test1.org", 2,
                                               next_superblock, payee, 300)
        self.stake_and_ping(self.minerAPos, 3, [mn1, mn2])
        proposalHash = minerA.submitbudget("test1", "https://test1.org", 2,
                                           next_superblock, payee, 300, proposalFeeTxId)
        time.sleep(1)
        self.stake_and_ping(self.minerAPos, 7, [mn1, mn2])
        self.log.info("Vote for the proposal and check projection...")
        minerA.mnbudgetvote("alias", proposalHash, "yes", self.dmn1.proTx)
        minerA.mnbudgetvote("alias", proposalHash, "yes", self.dmn2.proTx)
        time.sleep(1)
        self.stake_and_ping(self.minerAPos, 1, [mn1, mn2])
        projection = minerB.getbudgetprojection()[0]
        assert_equal(projection["Name"], "test1")
        assert_equal(projection["Hash"], proposalHash)
        assert_equal(projection["Yeas"], 2)

        # Create the finalized budget and vote on it (from the masternodes)
        self.log.info("Finalizing the budget...")
        self.stake_and_ping(self.minerAPos, 5, [mn1, mn2])
        assert minerA.mnfinalbudgetsuggest() is not None
        time.sleep(1)
        self.stake_and_ping(self.minerAPos, 4, [mn1, mn2])
        budgetFinHash = minerA.mnfinalbudgetsuggest()
        assert budgetFinHash != ""
        time.sleep(1)
        self.sync_all()
        assert_equal(mn1.mnfinalbudget("vote", budgetFinHash)["detail"][0]["result"], "success")
        assert_equal(mn2.mnfinalbudget("vote", budgetFinHash)["detail"][0]["result"], "success")
        self.stake_and_ping(self.minerAPos, 2, [mn1, mn2])
        budFin = minerB.mnfinalbudget("show")
        budget = budFin[next(iter(budFin))]
        assert_equal(budget["VoteCount"], 2)

        # Stake up until the block before the superblock.
        skip_blocks = next_superblock - minerA.getblockcount() - 1
        self.stake_and_ping(self.minerAPos, skip_blocks, [mn1, mn2])

        # Split the network.
        self.log.info("Splitting the chain at block %d" % minerA.getblockcount())
        self.split_network()

        # --- Chain A ---
        self.nodes.pop(self.minerBPos)
        # mine the superblock and check payment
        self.log.info("Checking superblock on chain A...")
        self.create_and_check_superblock(minerA, next_superblock, payee)
        # Add 10 blocks on top
        self.log.info("Staking 10 blocks...")
        self.stake_and_ping(self.nodes.index(minerA), 10, [mn1, mn2])

        # --- Chain B ---
        other_nodes = self.nodes.copy()
        self.nodes = [minerB]
        # mine the superblock and check payment
        self.log.info("Checking superblock on chain B...")
        self.create_and_check_superblock(minerB, next_superblock, payee)
        # Add 1 single block on top
        self.log.info("Staking 1 block...")
        self.stake_and_ping(self.nodes.index(minerB), 1, [])

        # --- Reconnect nodes --
        self.log.info("Reconnecting and re-organizing blocks...")
        self.nodes = other_nodes
        self.nodes.insert(self.minerBPos, minerB)
        set_node_times(self.nodes, self.mocktime)
        self.reconnect_nodes()
        self.sync_all()
        assert_equal(minerB.getblockcount(), next_superblock + 10)
        assert_equal(minerB.getbestblockhash(), minerA.getbestblockhash())

        self.log.info("All good.")

    def split_network(self):
        for i in range(self.num_nodes):
            if i != self.minerBPos:
                self.disconnect_node_pair(i, self.minerBPos)
                self.disconnect_node_pair(self.minerBPos, i)
        # by-pass ring connection
        assert self.minerBPos > 0
        self.connect_nodes(self.minerBPos-1, self.minerBPos+1)

    def disconnect_node_pair(self, a, b):
        # Robust version of the framework's disconnect_nodes: the daemon can briefly
        # stall (e.g. DKG phase processing), so retry the disconnect until the peer is gone.
        from_connection = self.nodes[a]
        for attempt in range(8):
            for addr in [peer['addr'] for peer in from_connection.getpeerinfo()
                         if "testnode%d" % b in peer['subver']]:
                try:
                    from_connection.disconnectnode(addr)
                except JSONRPCException as e:
                    if e.error['code'] != -29:  # RPC_CLIENT_NODE_NOT_CONNECTED
                        raise
            try:
                wait_until(lambda: [peer['addr'] for peer in from_connection.getpeerinfo()
                                    if "testnode%d" % b in peer['subver']] == [], timeout=15)
                return
            except AssertionError:
                self.log.info("node %d didn't disconnect from node %d (attempt %d), retrying..." % (a, b, attempt))
        raise AssertionError("Unable to disconnect node %d from node %d" % (a, b))

    def reconnect_nodes(self):
        for i in range(self.num_nodes):
            if i != self.minerBPos:
                self.connect_nodes(i, self.minerBPos)
                self.connect_nodes(self.minerBPos, i)

    def create_and_check_superblock(self, node, next_superblock, payee):
        self.stake_and_ping(self.nodes.index(node), 1, [])
        assert_equal(node.getblockcount(), next_superblock)
        # Post-v6 the budget payment is in the coinbase (first tx) of PoS blocks
        coinbase = node.getrawtransaction(node.getblock(node.getbestblockhash())["tx"][0], True)
        budget_payment_out = coinbase["vout"][-1]
        assert_equal(budget_payment_out["value"], Decimal("300"))
        assert_equal(budget_payment_out["scriptPubKey"]["addresses"][0], payee)


if __name__ == '__main__':
    GovernanceReorgTest().main()
