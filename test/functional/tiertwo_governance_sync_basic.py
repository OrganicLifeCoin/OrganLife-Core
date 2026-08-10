#!/usr/bin/env python3
# Copyright (c) 2020-2021 The PIVX Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.
"""
Test checking:
 1) Masternodes setup/creation.
 2) Proposal creation.
 3) Vote creation.
 4) Proposal and vote broadcast.
 5) Proposal and vote sync.
"""

import time

from test_framework.test_framework import PivxTier2TestFramework
from test_framework.util import (
    assert_equal,
    assert_true,
    get_datadir_path,
    satoshi_round,
    set_node_times
)
import shutil
import os

class Proposal:
    def __init__(self, name, link, cycles, payment_addr, amount_per_cycle):
        self.name = name
        self.link = link
        self.cycles = cycles
        self.paymentAddr = payment_addr
        self.amountPerCycle = amount_per_cycle
        self.feeTxId = ""
        self.proposalHash = ""

class MasternodeGovernanceBasicTest(PivxTier2TestFramework):

    def check_mns_status(self, node, proTxHash):
        status = node.getmasternodestatus()
        assert_equal(status["proTxHash"], proTxHash)
        assert_equal(status["dmnstate"]["PoSePenalty"], 0)
        assert_equal(status["status"], "Ready")

    def check_mn_list(self, node, txHashSet):
        # check masternode list from node
        mnlist = node.listmasternodes()
        assert_equal(len(mnlist), 3)
        foundHashes = set([mn.get("proTxHash", mn.get("txhash", "")) for mn in mnlist
                           if mn.get("proTxHash", mn.get("txhash", "")) in txHashSet])
        assert_equal(len(foundHashes), len(txHashSet))

    def check_mn_enabled_count(self, enabled, total):
        for node in self.nodes:
            node_count = node.getmasternodecount()
            assert_equal(node_count['enabled'], enabled)
            assert_equal(node_count['total'], total)

    def wait_until_mn_removed(self, proTxHash, _timeout, node):
        # wait until the DMN disappears from the list of the given node
        time.sleep(2)
        timeout = time.time() + _timeout
        while time.time() < timeout:
            if len(node.listmasternodes(proTxHash)) == 0:
                return
            time.sleep(2)
        raise AssertionError("MN %s still in the list of node %d" % (proTxHash, self.nodes.index(node)))

    def spend_dmn_collateral(self, mnOwner, dmn, miner):
        # spends the collateral outpoint of a DMN, removing it from the list
        collateral = dmn.collateral.to_json()
        send_value = satoshi_round(100 - 0.001)
        inputs = [collateral]
        outputs = {mnOwner.getnewaddress(): float(send_value)}
        rawtx = mnOwner.createrawtransaction(inputs, outputs)
        signedtx = mnOwner.signrawtransaction(rawtx)
        assert_equal(signedtx['complete'], True)
        txid = miner.sendrawtransaction(signedtx['hex'])
        self.sync_mempools()
        self.log.info("Collateral spent in %s" % txid)
        self.stake(1, [self.remoteTwo])

    def check_budget_finalization_sync(self, votesCount, status):
        for i in range(0, len(self.nodes)):
            node = self.nodes[i]
            budFin = node.mnfinalbudget("show")
            assert_true(len(budFin) == 1, "MN budget finalization not synced in node" + str(i))
            budget = budFin[next(iter(budFin))]
            assert_equal(budget["VoteCount"], votesCount)
            assert_equal(budget["Status"], status)

    def mine_blocks(self, num_blocks):
        # Bulk-mine PoS blocks with the daemon: much faster than the framework's
        # per-block stake() (blocks advance by the 15s time slots at a fixed
        # mocktime, no per-block RPC round-trips).
        remaining = num_blocks
        while remaining > 0:
            n = min(remaining, 250)
            self.miner.generate(n)
            remaining -= n
        self.sync_blocks()

    def broadcastbudgetfinalization(self, node, with_ping_mns=None):
        if with_ping_mns is None:
            with_ping_mns = []
        self.log.info("suggesting the budget finalization..")
        assert node.mnfinalbudgetsuggest() is not None

        self.log.info("confirming the budget finalization..")
        time.sleep(1)
        self.stake(4, with_ping_mns)

        self.log.info("broadcasting the budget finalization..")
        return node.mnfinalbudgetsuggest()

    def check_proposal_existence(self, proposalName, proposalHash):
        for node in self.nodes:
            proposals = node.getbudgetinfo(proposalName)
            assert len(proposals) > 0
            assert_equal(proposals[0]["Hash"], proposalHash)

    def check_vote_existence(self, proposalName, mnCollateralHash, voteType, voteValid):
        for i in range(0, len(self.nodes)):
            node = self.nodes[i]
            node.syncwithvalidationinterfacequeue()
            votesInfo = node.getbudgetvotes(proposalName)
            assert len(votesInfo) > 0
            found = False
            for voteInfo in votesInfo:
                if (voteInfo["mnId"].split("-")[0] == mnCollateralHash) :
                    assert_equal(voteInfo["Vote"], voteType)
                    assert_equal(voteInfo["fValid"], voteValid)
                    found = True
            assert_true(found, "Error checking vote existence in node " + str(i))

    def get_proposal_obj(self, Name, URL, Hash, FeeHash, BlockStart, BlockEnd,
                             TotalPaymentCount, RemainingPaymentCount, PaymentAddress,
                             Ratio, Yeas, Nays, Abstains, TotalPayment, MonthlyPayment,
                             IsEstablished, IsValid, Allotted, TotalBudgetAllotted, IsInvalidReason = ""):
        obj = {}
        obj["Name"] = Name
        obj["URL"] = URL
        obj["Hash"] = Hash
        obj["FeeHash"] = FeeHash
        obj["BlockStart"] = BlockStart
        obj["BlockEnd"] = BlockEnd
        obj["TotalPaymentCount"] = TotalPaymentCount
        obj["RemainingPaymentCount"] = RemainingPaymentCount
        obj["PaymentAddress"] = PaymentAddress
        obj["Ratio"] = Ratio
        obj["Yeas"] = Yeas
        obj["Nays"] = Nays
        obj["Abstains"] = Abstains
        obj["TotalPayment"] = TotalPayment
        obj["MonthlyPayment"] = MonthlyPayment
        obj["IsEstablished"] = IsEstablished
        obj["IsValid"] = IsValid
        if IsInvalidReason != "":
            obj["IsInvalidReason"] = IsInvalidReason
        obj["Allotted"] = Allotted
        obj["TotalBudgetAllotted"] = TotalBudgetAllotted
        return obj

    def check_budgetprojection(self, expected):
        for i in range(self.num_nodes):
            assert_equal(self.nodes[i].getbudgetprojection(), expected)
            self.log.info("Budget projection valid for node %d" % i)

    def create_proposals_tx(self, props):
        nextSuperBlockHeight = self.miner.getnextsuperblock()
        for entry in props:
            proposalFeeTxId = self.miner.preparebudget(
                entry.name,
                entry.link,
                entry.cycles,
                nextSuperBlockHeight,
                entry.paymentAddr,
                entry.amountPerCycle)
            entry.feeTxId = proposalFeeTxId
        return props

    def propagate_proposals(self, props):
        nextSuperBlockHeight = self.miner.getnextsuperblock()
        for entry in props:
            proposalHash = self.miner.submitbudget(
                entry.name,
                entry.link,
                entry.cycles,
                nextSuperBlockHeight,
                entry.paymentAddr,
                entry.amountPerCycle,
                entry.feeTxId)
            entry.proposalHash = proposalHash
        return props

    def submit_proposals(self, props):
        props = self.create_proposals_tx(props)
        # generate 3 blocks to confirm the tx (and update the mnping)
        self.stake(3, [self.remoteOne, self.remoteTwo])
        # check fee tx existence
        for entry in props:
            txinfo = self.miner.gettransaction(entry.feeTxId)
            assert_equal(txinfo['amount'], -100.00)
        # propagate proposals
        props = self.propagate_proposals(props)
        # let's wait a little bit and see if all nodes are sync
        time.sleep(1)
        for entry in props:
            self.check_proposal_existence(entry.name, entry.proposalHash)
            self.log.info("proposal %s broadcast successful!" % entry.name)
        return props

    def setup_3_dmns_network(self):
        self.ownerOne = self.nodes[self.ownerOnePos]
        self.remoteOne = self.nodes[self.remoteOnePos]
        self.ownerTwo = self.nodes[self.ownerTwoPos]
        self.remoteTwo = self.nodes[self.remoteTwoPos]
        self.miner = self.nodes[self.minerPos]
        self.remoteDMN1 = self.nodes[self.remoteDMN1Pos]

        self.log.info("generating 256 blocks..")
        # First mine 250 PoW blocks
        for i in range(250):
            self.mocktime = self.generate_pow(self.minerPos, self.mocktime)
        self.sync_blocks()
        # Then start staking
        self.stake(6)
        # Re-anchor the mocktime to the tip time: the fork's block times follow the
        # adjusted time only when it is ahead of the tip (otherwise the 15s time
        # slots apply, breaking time-based governance rules such as the 5min proposal
        # establishment).
        self.mocktime = self.miner.getblock(self.miner.getbestblockhash())["time"] + 60
        set_node_times(self.nodes, self.mocktime)

        self.log.info("masternodes setup..")
        # Register 3 deterministic masternodes (registration on-chain, collateral embedded).
        # The controllers (ownerOne/ownerTwo) hold the owner/voting keys used to vote.
        self.dmn1 = self.register_new_dmn(self.remoteOnePos, self.minerPos, self.ownerOnePos, "fund")
        self.dmn2 = self.register_new_dmn(self.remoteTwoPos, self.minerPos, self.ownerTwoPos, "fund")
        self.dmn3 = self.register_new_dmn(self.remoteDMN1Pos, self.minerPos, self.ownerOnePos, "fund")
        self.sync_all()

        self.log.info("masternodes setup completed, initializing them..")
        self.stake(1)
        self.remoteOne.initmasternode(self.dmn1.operator_sk)
        self.remoteTwo.initmasternode(self.dmn2.operator_sk)
        self.remoteDMN1.initmasternode(self.dmn3.operator_sk)

        # wait until mnsync complete on all nodes
        self.stake(1)
        self.wait_until_mnsync_finished()
        self.log.info("tier two synced, all masternodes started!")

    def run_test(self):
        self.enable_mocktime()
        self.setup_3_dmns_network()
        txHashSet = set([self.dmn1.proTx, self.dmn2.proTx, self.dmn3.proTx])
        # check mn list from miner
        self.check_mn_list(self.miner, txHashSet)
        # enabled/total masternodes: 3/3
        self.check_mn_enabled_count(3, 3)

        # check status of masternodes
        self.check_mns_status(self.remoteOne, self.dmn1.proTx)
        self.log.info("MN1 active")
        self.check_mns_status(self.remoteTwo, self.dmn2.proTx)
        self.log.info("MN2 active")
        self.check_mns_status(self.remoteDMN1, self.dmn3.proTx)
        self.log.info("DMN1 active")

        # activate sporks
        self.activate_spork(self.minerPos, "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT")
        self.activate_spork(self.minerPos, "SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT")
        self.activate_spork(self.minerPos, "SPORK_13_ENABLE_SUPERBLOCKS")
        nextSuperBlockHeight = self.miner.getnextsuperblock()

        # Submit first proposal
        self.log.info("preparing budget proposal..")
        firstProposal = Proposal(
            "super-cool",
            "https://forum.pivx.org/t/test-proposal",
            2,
            self.miner.getnewaddress(),
            300
        )
        self.submit_proposals([firstProposal])

        # Create 15 more proposals to have a higher tier two net gossip movement
        props = []
        for i in range(15):
            props.append(Proposal("prop_"+str(i),
                         "https://link_"+str(i)+".com",
                         3,
                         self.miner.getnewaddress(),
                         11 * (i + 1)))
        self.submit_proposals(props)

        # Proposals are established after 5 minutes. Mine 7 blocks
        # Proposal needs to be on the chain > 5 min.
        self.stake(7, [self.remoteOne, self.remoteTwo])
        # Check proposals existence
        for i in range(self.num_nodes):
            assert_equal(len(self.nodes[i].getbudgetinfo()), 16)

        # now let's vote for the proposal with the first MN
        self.log.info("Voting with MN1...")
        voteResult = self.ownerOne.mnbudgetvote("alias", firstProposal.proposalHash, "yes", self.dmn1.proTx)
        assert_equal(voteResult["detail"][0]["result"], "success")

        # check that the vote was accepted everywhere
        self.stake(1, [self.remoteOne, self.remoteTwo])
        self.check_vote_existence(firstProposal.name, self.dmn1.proTx, "YES", True)
        self.log.info("all good, MN1 vote accepted everywhere!")

        # before broadcast the second vote, let's drop the budget data of ownerOne.
        # so the node is forced to send a single proposal sync when the, now orphan, proposal vote is received.
        self.log.info("Testing single proposal re-sync based on an orphan vote, dropping budget data...")
        self.ownerOne.cleanbudget(try_sync=False)
        assert_equal(self.ownerOne.getbudgetprojection(), []) # empty
        assert_equal(self.ownerOne.getbudgetinfo(), [])

        # now let's vote for the proposal with the second MN
        self.log.info("Voting with MN2...")
        voteResult = self.ownerTwo.mnbudgetvote("alias", firstProposal.proposalHash, "yes", self.dmn2.proTx)
        assert_equal(voteResult["detail"][0]["result"], "success")

        # check orphan vote proposal re-sync
        self.log.info("checking orphan vote based proposal re-sync...")
        time.sleep(5) # wait a bit before check it
        self.check_proposal_existence(firstProposal.name, firstProposal.proposalHash)
        self.check_vote_existence(firstProposal.name, self.dmn1.proTx, "YES", True)
        self.log.info("all good, orphan vote based proposal re-sync succeeded")

        # check that the vote was accepted everywhere
        self.stake(1, [self.remoteOne, self.remoteTwo])
        self.check_vote_existence(firstProposal.name, self.dmn2.proTx, "YES", True)
        self.log.info("all good, MN2 vote accepted everywhere!")

        # now let's vote for the proposal with the third MN
        self.log.info("Voting with DMN1...")
        voteResult = self.ownerOne.mnbudgetvote("alias", firstProposal.proposalHash, "yes", self.dmn3.proTx)
        assert_equal(voteResult["detail"][0]["result"], "success")

        # check that the vote was accepted everywhere
        self.stake(1, [self.remoteOne, self.remoteTwo])
        self.check_vote_existence(firstProposal.name, self.dmn3.proTx, "YES", True)
        self.log.info("all good, DMN1 vote accepted everywhere!")

        # Now check the budget
        blockStart = nextSuperBlockHeight
        blockEnd = blockStart + firstProposal.cycles * 145
        TotalPayment = firstProposal.amountPerCycle * firstProposal.cycles
        Allotted = firstProposal.amountPerCycle
        RemainingPaymentCount = firstProposal.cycles
        expected_budget = [
            self.get_proposal_obj(firstProposal.name, firstProposal.link, firstProposal.proposalHash, firstProposal.feeTxId, blockStart,
                                  blockEnd, firstProposal.cycles, RemainingPaymentCount, firstProposal.paymentAddr, 1,
                                  3, 0, 0, satoshi_round(TotalPayment), satoshi_round(firstProposal.amountPerCycle),
                                  True, True, satoshi_round(Allotted), satoshi_round(Allotted))
                           ]
        self.check_budgetprojection(expected_budget)

        # Quick block count check.
        assert_equal(self.ownerOne.getblockcount(), 280)

        self.log.info("starting budget finalization sync test..")
        self.stake(2, [self.remoteOne, self.remoteTwo])

        # assert that there is no budget finalization first.
        assert_equal(len(self.ownerOne.mnfinalbudget("show")), 0)

        # suggest the budget finalization and confirm the tx (+4 blocks).
        budgetFinHash = self.broadcastbudgetfinalization(self.miner,
                                                         with_ping_mns=[self.remoteOne, self.remoteTwo])
        assert budgetFinHash != ""
        time.sleep(2)

        self.log.info("checking budget finalization sync..")
        self.check_budget_finalization_sync(0, "OK")

        self.log.info("budget finalization synced!, now voting for the budget finalization..")
        # Connecting owner to all the other nodes.
        self.connect_to_all(self.ownerOnePos)

        # Finalized budget votes are cast from the masternodes themselves (operator key)
        voteResult = self.remoteOne.mnfinalbudget("vote", budgetFinHash)
        assert_equal(voteResult["detail"][0]["result"], "success")
        self.log.info("Remote One voted successfully.")
        voteResult = self.remoteTwo.mnfinalbudget("vote", budgetFinHash)
        assert_equal(voteResult["detail"][0]["result"], "success")
        self.log.info("Remote Two voted successfully.")
        time.sleep(2) # wait a bit
        self.stake(1, [self.remoteOne, self.remoteTwo])
        self.check_budget_finalization_sync(2, "OK")

        # before broadcast the third finalization vote, let's drop the budget data of remoteOne.
        # so the node is forced to send a single fin sync when the, now orphan, vote is received.
        self.log.info("Testing single fin re-sync based on an orphan vote, dropping budget data...")
        self.remoteOne.cleanbudget(try_sync=False)
        assert_equal(self.remoteOne.getbudgetprojection(), []) # empty
        assert_equal(self.remoteOne.getbudgetinfo(), [])

        # vote for finalization with the third MN
        voteResult = self.remoteDMN1.mnfinalbudget("vote", budgetFinHash)
        assert_equal(voteResult["detail"][0]["result"], "success")
        self.log.info("DMN voted successfully.")
        time.sleep(2) # wait a bit

        self.log.info("checking finalization votes (and mining the superblock)..")
        self.stake(2, [self.remoteOne, self.remoteTwo])
        self.check_budget_finalization_sync(3, "OK")
        self.log.info("orphan vote based finalization re-sync succeeded")

        self.stake(6, [self.remoteOne, self.remoteTwo])
        addrInfo = self.miner.listreceivedbyaddress(0, False, False, firstProposal.paymentAddr)
        assert_equal(addrInfo[0]["amount"], firstProposal.amountPerCycle)

        self.log.info("budget proposal paid!, all good")

        # Check that the proposal info returns updated payment count
        expected_budget[0]["RemainingPaymentCount"] -= 1
        self.check_budgetprojection(expected_budget)

        self.stake(1, [self.remoteOne, self.remoteTwo])

        self.log.info("checking resync (1): cleaning budget data only..")
        # now let's drop budget data and try to re-sync it.
        self.remoteOne.cleanbudget(True)
        assert_equal(self.remoteOne.mnsync("status")["RequestedMasternodeAssets"], 0)
        assert_equal(self.remoteOne.getbudgetprojection(), []) # empty
        assert_equal(self.remoteOne.getbudgetinfo(), [])

        self.log.info("budget cleaned, starting resync")
        self.wait_until_mnsync_finished()
        self.check_budgetprojection(expected_budget)
        for i in range(self.num_nodes):
            assert_equal(len(self.nodes[i].getbudgetinfo()), 16)

        self.log.info("resync (1): budget data resynchronized successfully!")

        self.log.info("checking resync (2): stop node and reindex from scratch..")
        # Restart the node with -reindex: this rebuilds the chainstate AND
        # resets the tier-two state (evodb, llmq, spork DB), like a real node
        # recovering from a lost/corrupt chainstate. A manual partial wipe that
        # keeps stale tier-two caches is not a valid node state (it would reject
        # re-synced quorum-commitment blocks: HasMinedCommitment reads the
        # persisted evodb and would see commitments from the old chain).
        self.restart_node(self.ownerTwoPos, self.extra_args[self.ownerTwoPos] + ["-reindex"])
        self.ownerTwo.setmocktime(self.mocktime)
        self.connect_to_all(self.ownerTwoPos)
        self.stake(2, [self.remoteOne, self.remoteTwo])
        time.sleep(5) # wait a little bit

        self.log.info("syncing node..")
        self.wait_until_mnsync_finished()
        for i in range(self.num_nodes):
            assert_equal(len(self.nodes[i].getbudgetinfo()), 16)
        self.log.info("resync (2): budget data resynchronized successfully!")

        # Let's now verify the remote budget data relay.
        # Drop the budget data and generate blocks until someone incrementally sync us
        # (this is done once every 28 blocks on regtest).
        self.log.info("Testing incremental sync from peers, dropping budget data...")
        self.remoteDMN1.cleanbudget(try_sync=False)
        assert_equal(self.remoteDMN1.getbudgetprojection(), []) # empty
        assert_equal(self.remoteDMN1.getbudgetinfo(), [])
        self.log.info("Generating blocks until someone syncs the node..")
        self.stake(40, [self.remoteOne, self.remoteTwo])
        time.sleep(5) # wait a little bit
        self.log.info("Checking budget sync..")
        for i in range(self.num_nodes):
            assert_equal(len(self.nodes[i].getbudgetinfo()), 16)
        self.check_vote_existence(firstProposal.name, self.dmn1.proTx, "YES", True)
        self.check_vote_existence(firstProposal.name, self.dmn2.proTx, "YES", True)
        self.check_vote_existence(firstProposal.name, self.dmn3.proTx, "YES", True)
        self.check_budget_finalization_sync(3, "OK")
        self.log.info("Remote incremental sync succeeded")

        # now let's verify that votes expire properly.
        # Drop one DMN (spend its collateral)
        self.log.info("expiring MN1..")
        self.spend_dmn_collateral(self.ownerOne, self.dmn1, self.miner)
        self.wait_until_mn_removed(self.dmn1.proTx, 30, self.remoteTwo)
        self.stake(15, [self.remoteTwo]) # create blocks to remove staled votes
        time.sleep(2) # wait a little bit
        self.check_vote_existence(firstProposal.name, self.dmn1.proTx, "YES", False)
        self.check_budget_finalization_sync(2, "OK") # budget finalization vote removal
        self.log.info("MN1 vote expired after collateral spend, all good")

        self.log.info("expiring DMN1..")
        self.spend_dmn_collateral(self.ownerOne, self.dmn3, self.miner)
        self.wait_until_mn_removed(self.dmn3.proTx, 30, self.remoteTwo)
        self.stake(15, [self.remoteTwo]) # create blocks to remove staled votes
        time.sleep(2) # wait a little bit
        self.check_vote_existence(firstProposal.name, self.dmn3.proTx, "YES", False)
        self.check_budget_finalization_sync(1, "OK") # budget finalization vote removal
        self.log.info("DMN vote expired after collateral spend, all good")

        # Check that the finalized budget is kept for history until the end of the
        # governance history retention window (nBlockEnd + 2 * nBudgetCycleBlocks * 2 on regtest,
        # i.e. nBlockEnd + 5040 blocks), and removed after that.
        assert_equal(len(self.miner.mnfinalbudget("show")), 1)
        blocks_to_mine = nextSuperBlockHeight + 200 - self.miner.getblockcount()
        self.log.info("Mining %d more blocks to check the 200-blocks history point..." % blocks_to_mine)
        self.mine_blocks(blocks_to_mine - 1)
        # finalized budget must still be there (still valid)
        self.miner.checkbudgets()
        assert_equal(len(self.miner.mnfinalbudget("show")), 1)
        # after one more block it is expired but still kept for history
        self.stake(1, [self.remoteTwo])
        self.miner.checkbudgets()
        assert_equal(len(self.miner.mnfinalbudget("show")), 1)
        self.log.info("Finalized budget correctly kept for history 200 blocks after the payment")

        blocks_to_remove = nextSuperBlockHeight + 5040 - self.miner.getblockcount()
        self.log.info("Mining %d more blocks to check the finalized budget removal..." % blocks_to_remove)
        self.mine_blocks(blocks_to_remove - 1)
        # finalized budget must still be there
        self.miner.checkbudgets()
        assert_equal(len(self.miner.mnfinalbudget("show")), 1)
        # after one more block it must be removed
        self.stake(1, [self.remoteTwo])
        self.miner.checkbudgets()
        assert_equal(len(self.miner.mnfinalbudget("show")), 0)
        self.log.info("All good.")

if __name__ == '__main__':
    MasternodeGovernanceBasicTest().main()
