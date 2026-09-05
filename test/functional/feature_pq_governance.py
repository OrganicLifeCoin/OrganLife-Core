#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Exercise PQ proposals, coin-vote locks, votes, persistence, and payouts."""

from pathlib import Path
import time

from test_framework.authproxy import JSONRPCException
from test_framework.bech32 import CHARSET, convertbits
from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, set_node_times, wait_until


class PQGovernanceTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                            "-createwalletbackups=0", "-acceptnonstdtxn=0",
                            "-nuparams=PoS:130", "-nuparams=PoS_v2:130"]]
        self.mocktime = int(time.time())

    def pq_address(self, node, name):
        backup = Path(node.datadir) / f"{name}.dat"
        address = node.getnewpqaddress(str(backup))["address"]
        assert backup.is_file()
        return address

    @staticmethod
    def pq_script(address):
        data = [CHARSET.index(char) for char in address[address.rfind("1") + 1:-6]]
        assert_equal(data[0], 1)
        key_id = bytes(convertbits(data[1:], 5, 8, False))
        assert_equal(len(key_id), 32)
        return "ff5120" + key_id.hex()

    def mine(self, address, count=1):
        hashes = []
        for _ in range(count):
            def produce():
                self.mocktime += 60
                set_node_times(self.nodes, self.mocktime)
                try:
                    hashes.extend(self.nodes[0].generatetoaddress(1, address))
                    return True
                except JSONRPCException as error:
                    if "Couldn't create new blocks" not in error.error["message"]:
                        raise
                    return False
            wait_until(produce, timeout=120)
        return hashes

    def run_test(self):
        node = self.nodes[0]
        node.encryptwallet("pq-governance-passphrase")
        node.walletpassphrase("pq-governance-passphrase", 0)
        mining_address = self.pq_address(node, "mining")
        payment_address = self.pq_address(node, "proposal-payment")

        self.mine(mining_address, 101)
        start = node.getnextsuperblock()
        proposal = node.createpqproposal(
            "pq-test", "https://example.invalid/pq-test", 1, start, payment_address, 10)
        self.mine(mining_address)

        info = node.getbudgetinfo("pq-test")
        assert_equal(len(info), 1)
        assert_equal(info[0]["Hash"], proposal["proposal_hash"])
        assert_equal(info[0]["PaymentAddress"], payment_address)

        self.restart_node(0, self.extra_args[0])
        node = self.nodes[0]
        node.walletpassphrase("pq-governance-passphrase", 0)
        info = node.getbudgetinfo("pq-test")
        assert_equal(len(info), 1)
        assert_equal(info[0]["Hash"], proposal["proposal_hash"])

        lock = node.creategovvotelock(
            proposal["proposal_hash"], 5, info[0]["BlockEnd"] + 10)
        self.mine(mining_address)
        vote_txid = node.castgovvote(proposal["proposal_hash"], "yes", [lock["outpoint"]])
        self.mine(mining_address)
        assert node.gettransaction(vote_txid)["confirmations"] > 0
        status = node.getgovvotestatus(proposal["proposal_hash"])
        assert_equal(status["coin_yes"], 5)
        assert_equal(status["coin_no"], 0)

        self.mine(mining_address, start - node.getblockcount() - 1)
        payout_block = self.mine(mining_address)[0]
        assert_equal(node.getblockcount(), start)
        payout_txid = node.getblock(payout_block)["tx"][1]
        payout_tx = node.getrawtransaction(payout_txid, True)
        matching = [out for out in payout_tx["vout"]
                    if out["value"] == 10 and
                    out["scriptPubKey"]["hex"] == self.pq_script(payment_address)]
        assert_equal(len(matching), 1)

        node.invalidateblock(payout_block)
        assert_equal(node.getblockcount(), start - 1)
        node.reconsiderblock(payout_block)
        assert_equal(node.getbestblockhash(), payout_block)
        assert_equal(node.getgovvotestatus(proposal["proposal_hash"])["coin_yes"], 5)

        self.restart_node(0, self.extra_args[0] + ["-reindex"])
        node = self.nodes[0]
        assert_equal(node.getbestblockhash(), payout_block)
        assert_equal(node.getgovvotestatus(proposal["proposal_hash"])["coin_yes"], 5)


if __name__ == "__main__":
    PQGovernanceTest().main()
