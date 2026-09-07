#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""PQ-only reward issuance and transfers on disposable regtest nodes."""

from pathlib import Path

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PQOnlyTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [["-connect=0", "-dnsseed=0", "-staking=0",
                            "-createwalletbackups=0", "-acceptnonstdtxn=0"]
                           for _ in range(self.num_nodes)]

    def pq_address(self, node, name):
        backup = Path(node.datadir) / f"{name}.dat"
        address = node.getnewpqaddress(str(backup))["address"]
        assert backup.is_file()
        return address

    def run_test(self):
        miner, receiver = self.nodes
        for node in self.nodes:
            node.encryptwallet("pq-only-passphrase")
            node.walletpassphrase("pq-only-passphrase", 600)

        mining_address = self.pq_address(miner, "mining-key")
        receive_address = self.pq_address(receiver, "receive-key")
        assert_raises_rpc_error(-32601, "Method not found", miner.fundpqaddress,
                                receive_address, 1, str(Path(miner.datadir) / "unused.dat"))
        for command in [
                "getnewaddress", "dumpprivkey", "importprivkey", "dumpwallet",
                "signmessage", "sendtoaddress", "getnewstakingaddress", "delegatestake",
                "createmultisig", "verifymessage", "validateaddress",
                "createrawtransaction", "signrawtransaction", "getbestsaplinganchor",
                "getbestchainlock", "mnsync", "spork", "getmasternodecount",
                "generateblskeypair", "listquorums", "generate",
                "setgenerate"]:
            assert_raises_rpc_error(-32601, "Method not found", getattr(miner, command))

        blocks = miner.generatetoaddress(1, mining_address)
        first_reward = miner.getblock(blocks[0], 2)["tx"][0]["vout"][0]["value"]
        assert_equal(miner.getwalletinfo()["immature_balance"], first_reward)
        assert_equal(miner.getbalance(), 0)
        blocks += miner.generatetoaddress(100, mining_address)
        assert_equal(len(blocks), 101)
        self.sync_all()
        assert miner.listpqunspent()
        assert_equal(miner.getbalance(), first_reward)
        assert_equal(miner.getbalance(102), 0)
        assert_equal(miner.getwalletinfo()["balance"], first_reward)
        assert_raises_rpc_error(-5, "Invalid PQ address", miner.sendpqtoaddress,
                                "yjVR8Lk3Z4Dk2V5dYx9w1u7QfKcBz8nPqR", 1,
                                str(Path(miner.datadir) / "ordinary-recipient.dat"))

        backup = Path(miner.datadir) / "payment-change.dat"
        payment = miner.sendpqtoaddress(receive_address, 5, str(backup))
        assert backup.is_file()
        self.sync_mempools()
        for node in self.nodes:
            node.walletlock()
        assert_equal(receiver.getwalletinfo()["unconfirmed_balance"], 5)
        assert_equal(receiver.getunconfirmedbalance(), 5)
        assert_equal(receiver.getbalance(), 0)
        assert_equal(miner.getwalletinfo()["unconfirmed_balance"], first_reward - 5 - payment["fee"])
        miner.generatetoaddress(1, mining_address)
        self.sync_all()
        assert_equal(sum(coin["amount"] for coin in receiver.listpqunspent()), 5)
        assert_equal(receiver.getbalance(), 5)
        assert_equal(receiver.getwalletinfo()["balance"], 5)
        assert_equal(receiver.getunconfirmedbalance(), 0)
        assert_equal(miner.getbalance(), sum(coin["amount"] for coin in miner.listpqunspent()))
        for node in self.nodes:
            assert_equal(node.getwalletinfo()["unlocked_until"], 0)
        assert receiver.gettransaction(payment["txid"])["confirmations"] > 0


if __name__ == "__main__":
    PQOnlyTest().main()
