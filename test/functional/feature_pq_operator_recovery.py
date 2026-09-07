#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Controller recovery keys are backed before exposure, never spending keys."""
from pathlib import Path
import shutil

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PQOperatorRecoveryTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                            "-createwalletbackups=0", "-nuparams=pq_masternodes:1"]]

    def run_test(self):
        node = self.nodes[0]
        root = Path(node.datadir)
        snapshot = root / "operator-snapshot.dat"
        password = "public-operator-recovery-passphrase"
        assert_equal(node.listpqoperators(), [])
        assert_raises_rpc_error(-1, "createpqoperator", node.createpqoperator)
        assert_raises_rpc_error(-15, "Encrypt", node.createpqoperator, str(snapshot))
        node.encryptwallet(password)
        assert_raises_rpc_error(-13, "walletpassphrase", node.createpqoperator, str(snapshot))
        node.walletpassphrase(password, 600)
        assert_raises_rpc_error(-4, "backup failed", node.createpqoperator, str(root / "missing" / "wallet.dat"))
        assert_equal(node.listpqoperators(), [])
        # Abrupt exit after the failed backup's DB checkpoint; pending identity
        # must survive privately and remain unpublished after restart.
        node.process.kill()
        node.process.wait(timeout=30)
        assert node.process.returncode != 0
        node.running = False
        node.process = None
        node.rpc_connected = False
        node.rpc = None
        self.start_node(0)
        assert_equal(node.listpqoperators(), [])
        node.walletpassphrase(password, 600)
        first = node.createpqoperator(str(snapshot))
        assert_equal(set(first), {"publickey", "recovery_only"})
        assert_equal(first["recovery_only"], True)
        assert_equal(len(bytes.fromhex(first["publickey"])), 1312)
        assert snapshot.is_file()
        assert_equal(node.listpqoperators(), [first["publickey"]])
        assert_equal(node.listpqaddresses()["addresses"], [])
        assert_equal(node.getrawmempool(), [])
        original = snapshot.read_bytes()
        assert_raises_rpc_error(-4, "backup failed", node.createpqoperator, str(snapshot))
        assert_equal(snapshot.read_bytes(), original)
        assert_equal(node.listpqoperators(), [first["publickey"]])
        self.restart_node(0)
        assert_equal(node.listpqoperators(), [first["publickey"]])
        node.walletpassphrase(password, 600)
        second = node.createpqoperator(str(root / "second.dat"))
        assert first["publickey"] != second["publickey"]
        assert_equal(set(node.listpqoperators()), {first["publickey"], second["publickey"]})
        self.stop_node(0)
        # The original snapshot predates its own backed marker. It can recover
        # the exact original key, but only after making another verified backup.
        restored = root / "regtest/wallets/operator-restored"
        restored.mkdir()
        shutil.copyfile(snapshot, restored / "wallet.dat")
        self.start_node(0, self.extra_args[0] + ["-wallet=operator-restored"])
        assert_equal(node.listpqoperators(), [])
        node.walletpassphrase(password, 600)
        recovered = node.createpqoperator(str(root / "recovered.dat"))
        assert_equal(recovered, first)
        assert_equal(node.listpqaddresses()["addresses"], [])
        assert_equal(node.getrawmempool(), [])


if __name__ == "__main__":
    PQOperatorRecoveryTest().main()
