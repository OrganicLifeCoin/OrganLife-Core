#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""A reusable cached chain must retain encrypted, spendable PQ wallets."""
from pathlib import Path
import tempfile

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PQCacheTest(PivxTestFramework):
    def set_test_params(self):
        self.num_nodes = 5  # Four funded wallets plus a block-only cache clone.
        self.extra_args = [["-staking=0", "-connect=0", "-dnsseed=0", "-discover=0",
                            "-createwalletbackups=0", "-whitelist=127.0.0.1"] for _ in range(self.num_nodes)]
        self.enable_mocktime()

    def setup_chain(self):
        Path(self.options.cachedir).mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="unrelated-cache-", dir=self.options.cachedir) as sibling:
            marker = Path(sibling) / "keep"
            marker.write_text("unrelated fixture")
            super().setup_chain()
            assert_equal(marker.read_text(), "unrelated fixture")

    def run_test(self):
        for node in self.nodes:
            assert_equal(node.getblockcount(), 200)
        assert_equal(self.nodes[4].listpqunspent(), [])
        for node in self.nodes[:4]:
            assert_equal(node.getwalletinfo()["unlocked_until"], 0)
            assert_equal(len(node.listpqaddresses()["addresses"]), 1)
            assert_equal(len(node.listpqunspent()), 25)
            assert_raises_rpc_error(-32601, "Method not found", node.generate, 1)
            node.walletpassphrase("public-pq-cache-passphrase", 0)
        sender, receiver = self.nodes[:2]
        receive_address = receiver.listpqaddresses()["addresses"][0]
        balance = sum(coin["amount"] for coin in receiver.listpqunspent())
        backup = Path(sender.datadir) / "cached-payment.dat"
        payment = sender.sendpqtoaddress(receive_address, 1, str(backup))
        assert backup.is_file()
        self.sync_mempools()
        sender.generatetoaddress(1, sender.listpqaddresses()["addresses"][0])
        self.sync_all()
        assert_equal(receiver.gettransaction(payment["txid"])["confirmations"], 1)
        assert_equal(sum(coin["amount"] for coin in receiver.listpqunspent()), balance + 1)
        for node in self.nodes[:4]:
            node.walletlock()


if __name__ == "__main__":
    PQCacheTest().main()
