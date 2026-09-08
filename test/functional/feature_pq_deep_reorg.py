#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""PQ wallets survive a peer-driven reorg deeper than the legacy witness cache."""
from pathlib import Path

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal


class PQDeepReorgTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        # PoW isolates wallet disconnect handling from stake-input maturity.
        self.extra_args = [["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                            "-createwalletbackups=0", "-maxreorg=1000", "-nuparams=v5_shield:1",
                            "-nuparams=PoS:10000", "-nuparams=PoS_v2:10000"] for _ in range(2)]

    def run_test(self):
        addresses = []
        for node in self.nodes:
            node.encryptwallet("deep-reorg-test")
            node.walletpassphrase("deep-reorg-test", 0)
            addresses.append(node.getnewpqaddress(str(Path(node.datadir) / "address.dat"))["address"])
        self.nodes[0].generatetoaddress(101, addresses[0])
        self.sync_blocks()
        self.disconnect_nodes(0, 1)
        losing = self.nodes[0].generatetoaddress(102, addresses[0])
        winning = self.nodes[1].generatetoaddress(103, addresses[1])
        losing_reward = self.nodes[0].getblock(losing[0])["tx"][0]
        assert any(coin["txid"] == losing_reward for coin in self.nodes[0].listpqunspent())
        self.connect_nodes(0, 1)
        self.sync_blocks(timeout=60)
        for node in self.nodes:
            assert_equal(node.getbestblockhash(), winning[-1])
            assert_equal(node.listbanned(), [])
            assert node.getconnectioncount() > 0
            assert all(coin["txid"] != losing_reward for coin in node.listpqunspent())
        assert self.nodes[0].gettransaction(losing_reward)["confirmations"] <= 0
        last = self.nodes[1].generatetoaddress(2, addresses[1])[-1]
        self.sync_blocks()
        for index in range(2):
            self.restart_node(index, self.extra_args[index])
            assert_equal(self.nodes[index].getbestblockhash(), last)
            self.nodes[index].listpqunspent()


if __name__ == "__main__":
    PQDeepReorgTest().main()
