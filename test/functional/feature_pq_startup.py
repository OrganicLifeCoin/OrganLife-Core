#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Refuse stale/partially flushed PQ databases; rebuild only when explicitly requested."""

import hashlib
from pathlib import Path
import shutil

from test_framework.test_framework import PivxTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal


class PQStartupTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                            "-createwalletbackups=0"]]

    def run_test(self):
        node = self.nodes[0]
        node.encryptwallet("pq-startup-passphrase")
        node.walletpassphrase("pq-startup-passphrase", 600)
        address = node.getnewpqaddress(str(Path(node.datadir) / "mining-backup.dat"))["address"]
        node.generatetoaddress(2, address)
        self.stop_node(0)
        chain = Path(node.chain_path)
        stale = Path(node.datadir) / "stale-evodb"
        shutil.copytree(chain / "evodb", stale)
        self.start_node(0, self.extra_args[0])
        tip = node.generatetoaddress(1, address)[0]
        self.stop_node(0)
        # Model a completed coin flush followed by loss of the newer EvoDB cache.
        (chain / "evodb").rename(Path(node.datadir) / "newer-evodb")
        shutil.copytree(stale, chain / "evodb")
        block_files = {path: hashlib.sha256(path.read_bytes()).digest()
                       for path in (chain / "blocks").glob("blk*.dat")}
        for _ in range(2):
            node.assert_start_raises_init_error(
                self.extra_args[0], "Inconsistent block databases.*-reindex", ErrorMatch.PARTIAL_REGEX)
            for path, digest in block_files.items():
                assert_equal(hashlib.sha256(path.read_bytes()).digest(), digest)
        self.start_node(0, self.extra_args[0] + ["-reindex"])
        assert_equal(node.getbestblockhash(), tip)
        self.stop_node(0)

        # Exercise the existing real process-exit injection after the first partial coin batch.
        self.start_node(0, self.extra_args[0] + ["-dbcrashratio=1", "-dbbatchsize=1"])
        crash_tip = node.generatetoaddress(1, address)[0]
        with node.assert_debug_log(["Simulating a crash. Goodbye."]):
            self.stop_node(0)
        node.assert_start_raises_init_error(
            self.extra_args[0], "Inconsistent block databases.*-reindex", ErrorMatch.PARTIAL_REGEX)
        self.start_node(0, self.extra_args[0] + ["-reindex"])
        assert_equal(node.getbestblockhash(), crash_tip)
        self.restart_node(0, self.extra_args[0])
        assert_equal(node.getbestblockhash(), crash_tip)


if __name__ == "__main__":
    PQStartupTest().main()
