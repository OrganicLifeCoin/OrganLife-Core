#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Mine and reconnect ML-DSA-authorized PoS blocks on disposable regtest nodes."""

from pathlib import Path
import time

from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, set_node_times, wait_until


class PQPoSTest(PivxTestFramework):
    def add_options(self, parser):
        parser.add_option("--pq-registry", action="store_true", default=False,
                          help="Also exercise the opt-in regtest registry during staking and reorgs")

    def setup_network(self):
        if self.options.pq_registry:
            for args in self.extra_args:
                args.append("-nuparams=pq_masternodes:1")
        super().setup_network()

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                            "-createwalletbackups=0", "-acceptnonstdtxn=0",
                            "-nuparams=PoS:130", "-nuparams=PoS_v2:130"]
                           for _ in range(self.num_nodes)]
        self.mocktime = int(time.time())

    def pq_address(self, node, name):
        backup = Path(node.datadir) / f"{name}.dat"
        address = node.getnewpqaddress(str(backup))["address"]
        assert backup.is_file()
        return address

    def sync_mempools(self, nodes=None, wait=1, timeout=60, flush_scheduler=True):
        peers = nodes or self.nodes

        def relayed():
            # Normal peers randomize inventory announcements using mock time.
            # Let that clock advance instead of bypassing relay with a whitelist.
            self.mocktime += 1
            set_node_times(self.nodes, self.mocktime)
            return len({tuple(sorted(node.getrawmempool())) for node in peers}) == 1

        wait_until(relayed, timeout=timeout)
        super().sync_mempools(peers, wait, timeout, flush_scheduler)

    def mine(self, address, count=1, node_index=0, sync=True):
        hashes = []
        for _ in range(count):
            def produce():
                self.mocktime += 60
                set_node_times(self.nodes, self.mocktime)
                try:
                    hashes.extend(self.nodes[node_index].generatetoaddress(1, address))
                    return True
                except JSONRPCException as error:
                    if "Couldn't create new blocks" not in error.error["message"]:
                        raise
                    return False
            wait_until(produce, timeout=120)
            if sync:
                self.sync_all()
        return hashes

    def assert_pq_stake(self, blockhash):
        block = self.nodes[0].getblock(blockhash)
        assert "hashProofOfStake" in block
        stake = self.nodes[0].getrawtransaction(block["tx"][1], True)
        assert_equal(stake["version"], 3)
        assert_equal(stake["type"], 8)
        assert stake["extraPayload"].startswith("010201")
        assert_equal(stake["vin"][0]["scriptSig"]["hex"], "")
        assert_equal(stake["vout"][0]["scriptPubKey"]["hex"], "")
        assert stake["vout"][1]["scriptPubKey"]["hex"].startswith("ff5120")
        for node in self.nodes:
            assert_equal(node.getbestblockhash(), blockhash)

    def run_test(self):
        miner, receiver = self.nodes
        for node in self.nodes:
            node.encryptwallet("pq-pos-passphrase")
            node.walletpassphrase("pq-pos-passphrase", 0)
        mining_address = self.pq_address(miner, "mining")
        receive_address = self.pq_address(receiver, "receive")
        return_address = self.pq_address(miner, "return")

        self.mine(mining_address, 101)
        payment_backup = Path(miner.datadir) / "payment.dat"
        payment = miner.sendpqtoaddress(receive_address, 5, str(payment_backup))
        self.mine(mining_address)
        assert receiver.gettransaction(payment["txid"])["confirmations"] > 0
        self.mine(mining_address, 27)
        assert_equal(miner.getblockcount(), 129)
        assert miner.getstakingstatus()["stakeablecoins"] > 0

        miner.walletlock()
        miner.walletpassphrase("pq-pos-passphrase", 0, True)
        first_stake = self.mine(mining_address)[0]
        self.assert_pq_stake(first_stake)

        receiver_payment_backup = Path(receiver.datadir) / "return-payment.dat"
        receiver_payment = receiver.sendpqtoaddress(return_address, 1, str(receiver_payment_backup))
        self.sync_mempools()
        second_stake = self.mine(mining_address)[0]
        self.assert_pq_stake(second_stake)
        assert receiver.gettransaction(receiver_payment["txid"])["confirmations"] > 0

        snapshots = [node.listpqunspent() for node in self.nodes]
        for node in self.nodes:
            node.invalidateblock(second_stake)
        self.sync_blocks()
        for node in self.nodes:
            node.reconsiderblock(second_stake)
        self.sync_all()
        assert_equal([node.listpqunspent() for node in self.nodes], snapshots)

        # Rejoin a genuine network partition using ordinary peer validation:
        # no whitelist, ban clearing, invalidation, or manual block submission.
        self.mine(mining_address, 20)
        self.disconnect_nodes(0, 1)
        shorter_tip = self.mine(receive_address, node_index=1, sync=False)[0]
        stronger_tip = self.mine(mining_address, 2, sync=False)[-1]
        assert shorter_tip != stronger_tip
        self.connect_nodes(0, 1)
        self.sync_all()
        for node in self.nodes:
            assert_equal(node.getbestblockhash(), stronger_tip)
            assert_equal(node.listbanned(), [])
            assert node.getconnectioncount() > 0


if __name__ == "__main__":
    PQPoSTest().main()
