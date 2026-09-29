#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Automatic wallet backups include newly created PQ addresses."""

from pathlib import Path
import shutil

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal


class PQWalletBackupsTest(PivxTestFramework):
    def add_options(self, parser):
        parser.add_option("--mainnet", action="store_true", default=False)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [
            ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0"],
            ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0", "-createwalletbackups=0"],
        ]

    def setup_chain(self):
        if self.options.mainnet:
            self.chain = "main"
        super().setup_chain()

    def setup_network(self):
        if self.options.mainnet:
            # Mainnet mode is deliberately isolated: do not create even a
            # framework-managed connection between the local test nodes.
            self.setup_nodes()
        else:
            super().setup_network()

    def run_test(self):
        node = self.nodes[0]
        assert_equal(node.getblockchaininfo()["chain"], self.chain)
        wallet_path = Path(node.chain_path) / "wallets" / "wallet.dat"
        backups_dir = Path(node.chain_path) / "backups"
        assert wallet_path.is_file()
        initial_backups = list(backups_dir.iterdir())
        assert_equal(len(initial_backups), 1)
        initial_names = {path.name for path in initial_backups}

        address = node.getnewpqaddress()["address"]
        backups = list(backups_dir.iterdir())
        assert_equal(len(backups), 2)
        added_backups = [path for path in backups if path.name not in initial_names]
        assert_equal(len(added_backups), 1)
        newest = added_backups[0]

        recovered_path = wallet_path.parent / "pq-recovered.dat"
        assert not recovered_path.exists()
        self.stop_node(0)
        shutil.copyfile(newest, recovered_path)
        self.start_node(0, ["-wallet=pq-recovered.dat"] + self.extra_args[0])
        recovered = node.get_wallet_rpc(node.listwallets()[0])
        assert address in recovered.listpqaddresses()["addresses"]

        no_backup_node = self.nodes[1]
        assert (Path(no_backup_node.chain_path) / "wallets" / "wallet.dat").is_file()
        no_backup_node.getnewpqaddress()
        no_backup_dir = Path(no_backup_node.chain_path) / "backups"
        assert not no_backup_dir.exists() or not any(no_backup_dir.iterdir())


if __name__ == "__main__":
    PQWalletBackupsTest().main()
