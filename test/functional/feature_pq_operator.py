#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Opt-in operator credentials are separate from wallets and service authority."""
import json
from pathlib import Path
import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.permissions import make_private_directory, set_credential_permissions
from test_framework.test_framework import PivxTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, assert_raises_rpc_error


class PQOperatorTest(PivxTestFramework):
    def add_options(self, parser):
        parser.add_option("--walletless", action="store_true", default=False,
                          help="Exercise a binary configured with wallet support disabled")

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.base_args = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                          "-disablewallet", "-createwalletbackups=0"]
        self.extra_args = [self.base_args, self.base_args + ["-listen=1", "-nuparams=pq_masternodes:1"]]

    def setup_network(self):
        self.setup_nodes()  # Keep peers separate until the pending-identity sync check.

    def run_test(self):
        node = self.nodes[0]
        assert_equal(node.getpqoperatorinfo(), {"configured": False})
        assert_raises_rpc_error(-1, "getpqoperatorinfo", node.getpqoperatorinfo, True)
        assert_raises_rpc_error(-32601, "Method not found", node.signrawtransaction, "00")
        assert "version" in node.getnetworkinfo()
        assert_equal(node.getbudgetinfo(), [])
        assert node.getnextsuperblock() > 0
        if self.options.walletless:
            for method in ["getwalletinfo", "createpqproposal", "creategovvotelock", "castgovvote", "listgovlocks"]:
                assert_raises_rpc_error(-32601, "Method not found", getattr(node, method))
        directory = Path(node.datadir) / "private-operator"
        make_private_directory(directory)
        record = bytes.fromhex(json.loads((Path(__file__).parent / "data/pq_operator.json").read_text())["record"])
        key_file = directory / "olc-pq-operator-key"
        record_file = directory / "olc-pq-operator-record"
        key_file.write_bytes(b"*" * 32)  # Public dummy wrapping key; never use outside tests.
        record_file.write_bytes(record)
        set_credential_permissions(key_file, 0o400)
        set_credential_permissions(record_file, 0o400)
        identity = "12" * 32
        credentials = "-pqoperatorcredentials=" + str(directory)
        registration = "-pqoperatorid=" + identity
        enabled = self.base_args + ["-nuparams=pq_masternodes:1"]
        self.stop_node(0)

        def reject(args, message):
            node.assert_start_raises_init_error(args, message, ErrorMatch.PARTIAL_REGEX)

        for args in [[credentials], [registration], [credentials, credentials, registration],
                     [credentials, registration, registration]]:
            reject(enabled + args, "exactly one PQ operator credential source")
        for invalid in ["", "0" * 64, "12", "g" * 64, "12" * 33, "0x" + identity]:
            reject(enabled + [credentials, "-pqoperatorid=" + invalid], "64 hexadecimal characters and nonzero")
        reject(self.base_args + [credentials, registration], "require scheduled test-chain PQ masternode activation")
        if not self.options.walletless:
            reject(enabled + [credentials, registration, "-disablewallet=0"], "require -disablewallet")
        for activation in ["0", "-1"]:
            reject(self.base_args + ["-nuparams=pq_masternodes:" + activation, credentials, registration],
                   "require scheduled test-chain PQ masternode activation")
        reject(enabled + ["-pqoperatorcredentials=relative", registration], "Could not load private PQ operator credentials")

        # Unknown/unconfirmed identity can sync, but configured must never mean active.
        options = enabled + [credentials, registration]
        for _ in range(2):
            self.start_node(0, options)
            assert_equal(node.getpqoperatorinfo(), {"configured": True, "registration": identity,
                                                   "publickey": record[1:1313].hex()})
            assert_equal(node.getblockcount(), 0)
            self.stop_node(0)

        # Inline credentials are config-file-only and must load the same key
        # without creating or reading a credential directory.
        inline_config = Path(node.datadir) / "inline-operator.conf"
        inline_payload = (record + (b"*" * 32)).hex()
        inline_config.write_text("disablewallet=1\nnuparams=pq_masternodes:1\n"
                                f"pqoperatorid={identity}\n"
                                f"pqoperatorconfig={inline_payload}\n" +
                                (Path(node.datadir) / "pivx.conf").read_text())
        set_credential_permissions(inline_config, 0o400)
        inline_options = self.base_args + ["-conf=" + str(inline_config)]
        self.start_node(0, inline_options)
        assert_equal(node.getpqoperatorinfo(), {"configured": True, "registration": identity,
                                               "publickey": record[1:1313].hex()})
        self.stop_node(0)
        set_credential_permissions(inline_config, 0o404)
        reject(inline_options, "private owner-only config file")
        set_credential_permissions(inline_config, 0o400)
        reject(inline_options + [credentials], "exactly one PQ operator credential source")
        for option in ["pqoperatorconfig", "nopqoperatorconfig", "test.pqoperatorconfig", "regtest.nopqoperatorconfig"]:
            reject(self.base_args + ["-" + option + "=" + inline_payload, "-pqoperatorid=" + identity],
                   "never on the command line")
        assert inline_payload not in (Path(node.chain_path) / "debug.log").read_text()

        # A pending identity must still download/validate blocks using ordinary P2P.
        # Build PoW fixtures without wallet APIs so this also tests walletless nodes.
        peer = self.nodes[1]
        for height in range(1, 4):
            coinbase = create_coinbase(height)
            coinbase.vout[0].scriptPubKey = bytes.fromhex("ff5120" + "00" * 32)
            coinbase.rehash()
            block = create_block(int(peer.getbestblockhash(), 16), coinbase, int(time.time()) + height)
            block.solve()
            assert_equal(peer.submitblock(block.serialize().hex()), None)
        self.start_node(0, options)
        self.connect_nodes(0, 1)
        self.sync_blocks()
        assert_equal(node.getblockcount(), 3)
        assert_equal(node.getpqoperatorinfo()["registration"], identity)
        decoded = node.decoderawtransaction(coinbase.serialize().hex())
        assert_equal(decoded["txid"], coinbase.hash)
        assert_equal(node.getrawtransaction(coinbase.hash, True, block.hash)["confirmations"], 1)
        node.invalidateblock(block.hash)
        assert_equal(node.getblockcount(), 2)
        node.reconsiderblock(block.hash)
        self.sync_blocks()
        assert_equal(node.getblockcount(), 3)
        self.stop_node(0)

        # Each restart re-reads credentials; it must not reuse a cached decrypted key.
        set_credential_permissions(key_file, 0o600)
        key_file.write_bytes(b"+" * 32)
        set_credential_permissions(key_file, 0o400)
        reject(options, "Could not load private PQ operator credentials")
        set_credential_permissions(key_file, 0o600)
        key_file.write_bytes(b"*" * 32)
        set_credential_permissions(key_file, 0o400)
        set_credential_permissions(record_file, 0o404)
        reject(options, "Could not load private PQ operator credentials")
        set_credential_permissions(record_file, 0o400)
        self.start_node(0, enabled)
        assert_equal(node.getpqoperatorinfo(), {"configured": False})
        self.stop_node(0)
        # A failure after loading (network initialization) must cleanly shut down.
        reject(options + ["-onlynet=invalid"], "Unknown network specified")
        self.start_node(0, options)
        assert_equal(node.getpqoperatorinfo()["registration"], identity)
        log = (Path(node.chain_path) / "debug.log").read_text()
        assert (b"*" * 32).hex() not in log
        assert (b"\x11" * 32).hex() not in log
        self.stop_node(0)
        # Scheduled testnet activation must not accept a regtest credential.
        reject(self.base_args + ["-regtest=0", "-testnet=1", credentials, registration],
               "Could not load private PQ operator credentials")
        self.start_node(0, enabled)


if __name__ == "__main__":
    PQOperatorTest().main()
