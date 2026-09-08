#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Explicit regtest registry activation, persistent markers and rebuild/reorg safety."""

from pathlib import Path

from test_framework.test_framework import PivxTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal


class PQRegistryTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.base_args = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                          "-createwalletbackups=0", "-checklevel=4", "-checkblocks=0",
                          "-nuparams=v5_shield:0"]
        self.extra_args = [self.base_args + ["-nuparams=pq_masternodes:3"]]

    def run_test(self):
        node = self.nodes[0]
        node.encryptwallet("pq-registry-passphrase")
        node.walletpassphrase("pq-registry-passphrase", 600)
        address = node.getnewpqaddress(str(Path(node.datadir) / "mining.dat"))["address"]
        blocks = node.generatetoaddress(5, address)
        self.restart_node(0, self.extra_args[0])
        assert_equal(node.getbestblockhash(), blocks[-1])
        assert node.verifychain(0)  # all blocks, at the configured checklevel=4
        self.stop_node(0)
        # Changing or disabling rules on an already indexed chain requires an explicit rebuild.
        for change in [[], ["-nuparams=pq_masternodes:4"], ["-nuparams=pq_masternodes:6"]]:
            node.assert_start_raises_init_error(
                self.base_args + change, "Inconsistent block databases.*-reindex", ErrorMatch.PARTIAL_REGEX)
        for rebuild in ["-reindex-chainstate", "-reindex"]:
            self.start_node(0, self.extra_args[0] + [rebuild])
            assert_equal(node.getbestblockhash(), blocks[-1])
            assert node.verifychain(0)
            self.stop_node(0)
        self.start_node(0, self.extra_args[0])
        node.invalidateblock(blocks[2])
        assert_equal(node.getbestblockhash(), blocks[1])
        self.restart_node(0, self.extra_args[0])
        assert_equal(node.getbestblockhash(), blocks[1])
        node.reconsiderblock(blocks[2])
        assert_equal(node.getbestblockhash(), blocks[-1])


if __name__ == "__main__":
    PQRegistryTest().main()
