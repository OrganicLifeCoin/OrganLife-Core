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
        # Independent mature inputs keep the second staker eligible across
        # repeated forks; a just-used coinstake output must mature again.
        for index in range(5):
            miner.sendpqtoaddress(receive_address, 1, str(Path(miner.datadir) / f"stake-input-{index}.dat"))
            self.mine(mining_address)
        self.mine(mining_address, 22)
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

        # Reproduce two stakers extending equal-work branches. Local arrival
        # order must not keep otherwise connected nodes on equal-work forks.
        for depth in (1, 3, 1):
            self.log.info("Rejoining equal-work PoS branches of depth %d", depth)
            self.disconnect_nodes(0, 1)
            for _ in range(depth):
                tips = [None, None]
                def produce_pair():
                    self.mocktime += 120
                    set_node_times(self.nodes, self.mocktime)
                    for index, address in enumerate((mining_address, receive_address)):
                        if tips[index] is not None:
                            continue
                        try:
                            tips[index] = self.nodes[index].generatetoaddress(1, address)[0]
                        except JSONRPCException as error:
                            if "Couldn't create new blocks" not in error.error["message"]:
                                raise
                    return all(tips)
                # Kernel eligibility is probabilistic. Retry only a missing
                # block, never extend one island ahead of the other. Regtest's
                # fixed difficulty gives equal work even after a missed slot.
                wait_until(produce_pair, timeout=120)
            headers = [node.getblockheader(tip) for node, tip in zip(self.nodes, tips)]
            assert tips[0] != tips[1]
            assert_equal(headers[0]["height"], headers[1]["height"])
            assert_equal(headers[0]["chainwork"], headers[1]["chainwork"])
            expected = min(tips, key=lambda tip: int(tip, 16))
            self.connect_nodes(0, 1)
            self.sync_blocks(timeout=20)
            for node in self.nodes:
                assert_equal(node.getbestblockhash(), expected)
                assert_equal(node.listbanned(), [])

        # On-disk block indices lose their arrival sequence on restart. The
        # choice must still agree without the peer forcing a heavier branch.
        for index in range(self.num_nodes):
            self.restart_node(index, extra_args=self.extra_args[index] + ["-mocktime=%d" % self.mocktime])
            assert_equal(self.nodes[index].getbestblockhash(), expected)


if __name__ == "__main__":
    PQPoSTest().main()
