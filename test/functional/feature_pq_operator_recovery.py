#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Controller recovery keys are backed before exposure, never spending keys."""
from pathlib import Path
import shutil

from test_framework.permissions import assert_private_permissions, make_private_directory
from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PQOperatorRecoveryTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        wallet_args = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                       "-createwalletbackups=0", "-nuparams=pq_masternodes:1"]
        self.extra_args = [wallet_args, ["-connect=0", "-dnsseed=0", "-discover=0",
                                        "-staking=0", "-disablewallet", "-nuparams=pq_masternodes:1"]]

    def run_test(self):
        node = self.nodes[0]
        export_help = node.help("exportpqoperator")
        assert "Windows (local fixed NTFS)" in export_help
        assert "Linux/macOS only" not in export_help
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

        # Export only fully-unlocked, backed operators into a new private
        # directory.  The wrapping key is credentials-only material; it is
        # never returned by the RPC and cannot be overwritten in place.
        export_parent = root / "private-export-parent"
        make_private_directory(export_parent)
        exported = export_parent / "operator-credentials"
        node.walletlock()
        assert_raises_rpc_error(-13, "walletpassphrase", node.exportpqoperator,
                                first["publickey"], str(exported))
        node.walletpassphrase(password, 600, True)
        assert_raises_rpc_error(-13, "walletpassphrase", node.exportpqoperator,
                                first["publickey"], str(exported))
        node.walletpassphrase(password, 600)
        assert_raises_rpc_error(-8, "public key", node.exportpqoperator, "00", str(exported))
        assert_raises_rpc_error(-4, "operator", node.exportpqoperator,
                                "00" * 1312, str(export_parent / "unknown"))
        exported_result = node.exportpqoperator(first["publickey"], str(exported))
        assert_equal(exported_result, {"publickey": first["publickey"], "credentials_only": True})
        record_file = exported / "olc-pq-operator-record"
        key_file = exported / "olc-pq-operator-key"
        assert_private_permissions(exported, 0o700)
        assert_private_permissions(record_file, 0o400)
        assert_private_permissions(key_file, 0o400)
        record_bytes = record_file.read_bytes()
        key_bytes = key_file.read_bytes()
        assert_equal(record_bytes[0], 2)  # credentials use the v2 operator record
        assert_equal(len(key_bytes), 32)
        assert_raises_rpc_error(-4, "destination", node.exportpqoperator,
                                first["publickey"], str(exported))
        assert_equal(record_file.read_bytes(), record_bytes)
        assert_equal(key_file.read_bytes(), key_bytes)

        # A walletless node can load the exported pair for an unknown yet
        # syntactically valid registration.  It remains pending, but its
        # identity is stable across restart and no wallet APIs are present.
        node1 = self.nodes[1]
        registration = "12" * 32
        self.stop_node(1)
        operator_args = self.extra_args[1] + ["-pqoperatorcredentials=" + str(exported),
                                               "-pqoperatorid=" + registration]
        self.start_node(1, operator_args)
        assert_equal(node1.getpqoperatorinfo(), {"configured": True,
                                                  "registration": registration,
                                                  "publickey": first["publickey"]})
        assert_raises_rpc_error(-32601, "Method not found", node1.getwalletinfo)
        self.stop_node(1)
        self.start_node(1, operator_args)
        assert_equal(node1.getpqoperatorinfo()["publickey"], first["publickey"])
        self.stop_node(1)

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
        assert_raises_rpc_error(-4, "operator", node.exportpqoperator,
                                first["publickey"], str(export_parent / "restored-before-backup"))
        recovered = node.createpqoperator(str(root / "recovered.dat"))
        assert_equal(recovered, first)
        recovered_export = export_parent / "restored-operator-credentials"
        assert_equal(node.exportpqoperator(recovered["publickey"], str(recovered_export)),
                     {"publickey": first["publickey"], "credentials_only": True})
        assert_equal(node.listpqaddresses()["addresses"], [])
        assert_equal(node.getrawmempool(), [])


if __name__ == "__main__":
    PQOperatorRecoveryTest().main()
