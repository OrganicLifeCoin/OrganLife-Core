#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Fresh mainnet startup, wallet preparation and prelaunch refusal on isolated nodes."""

from pathlib import Path
import time

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PQMainnetTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = "main"
        self.num_nodes = 1
        self.extra_args = [["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                            "-createwalletbackups=0"]]

    def run_test(self):
        node = self.nodes[0]
        genesis = "0000091cf3aeeed50f65d6e640a35029d7b541b4e42950232385b13e88a12fdb"
        assert_equal(node.getblockchaininfo()["chain"], "main")
        assert_equal(node.getblockcount(), 0)
        assert_equal(node.getbestblockhash(), genesis)
        assert_equal(node.getnetworkinfo()["version"], 1010000)
        node.encryptwallet("isolated-mainnet-test")
        node.walletpassphrase("isolated-mainnet-test", 600)
        backup = Path(node.datadir) / "mainnet-address.dat"
        created = node.getnewpqaddress(str(backup))
        address = created["address"]
        assert address.startswith("olcpq1p")
        assert backup.is_file()
        assert created["payable"]
        assert_equal(node.getbalance(), 0)
        assert_equal(node.getpqfinalityinfo()["committee_size"], 0)
        assert_raises_rpc_error(-1, "regression testing", node.setmocktime, 1789567201)
        if time.time() < 1789567200:
            assert_raises_rpc_error(-8, "Mainnet launches on 2026-09-16 at 14:00 UTC",
                                    node.getblocktemplate)
        node.walletlock()
        self.restart_node(0, self.extra_args[0] + ["-mocktime=1789567201"])
        log = (Path(node.chain_path) / "debug.log").read_text(encoding="utf8")
        assert "block database contains a block which appears to be from the future" not in log
        assert "Reindexing block file" not in log
        assert_equal(node.getbestblockhash(), genesis)
        assert address in node.listpqaddresses()["addresses"]
        assert_equal(node.getwalletinfo()["unlocked_until"], 0)
        if time.time() < 1789567200:
            assert_raises_rpc_error(-8, "Mainnet launches on 2026-09-16 at 14:00 UTC",
                                    node.getblocktemplate)


if __name__ == "__main__":
    PQMainnetTest().main()
