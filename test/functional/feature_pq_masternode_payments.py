#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""One whole PQ masternode reward per block, with an undoable fair queue."""
from decimal import Decimal
from pathlib import Path
from copy import deepcopy

from test_framework.authproxy import JSONRPCException
from test_framework.bech32 import CHARSET, convertbits
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTxOut, COIN
from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, set_node_times, wait_until


class PQMasternodePaymentsTest(PivxTestFramework):
    def add_options(self, parser):
        parser.add_option("--pos", action="store_true", default=False)

    def setup_network(self):
        if self.options.pos:
            for args in self.extra_args:
                args.extend(["-nuparams=PoS:130", "-nuparams=PoS_v2:130"])
        super().setup_network()

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        args = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                "-createwalletbackups=0", "-nuparams=pq_masternodes:1"]
        self.extra_args = [args, args + ["-disablewallet"]]

    def run_test(self):
        controller, observer = self.nodes
        root = Path(controller.datadir)
        controller.encryptwallet("pq-payments-test")
        controller.walletpassphrase("pq-payments-test", 600)

        def address(label):
            return controller.getnewpqaddress(str(root / (label + ".dat")))["address"]

        miner = address("miner")
        controller.generatetoaddress(110, miner)
        payouts = []
        registrations = []
        for i in range(4):
            payout = address("payout" + str(i))
            # Address came from this node; decode its v1 program for output checks.
            data = [CHARSET.index(c) for c in payout.rsplit("1", 1)[1]]
            payouts.append("ff5120" + bytes(convertbits(data[1:-6], 5, 8, False)).hex())
            options = {"collateral_address": address("bond" + str(i)),
                       "owner_address": address("owner" + str(i)),
                       "operator_publickey": controller.createpqoperator(str(root / ("operator" + str(i) + ".dat")))["publickey"],
                       "payout_address": payout, "service": "[::1]:" + str(20000 + i)}
            registrations.append(controller.sendpqmasternode("register", options,
                                 str(root / ("register" + str(i) + ".dat")))["txid"])
            controller.generatetoaddress(1, miner)
        self.sync_all()
        self.mocktime = controller.getblock(controller.getbestblockhash())["time"]

        def mine():
            hashes = []
            def produce():
                self.mocktime += 60
                set_node_times(self.nodes, self.mocktime)
                try:
                    hashes.extend(controller.generatetoaddress(1, miner))
                    return True
                except JSONRPCException as error:
                    if "Couldn't create new blocks" not in error.error["message"]:
                        raise
                    return False
            wait_until(produce, timeout=120)
            return hashes[0]

        # A correctly funded reward to the wrong node must fail consensus, not
        # merely be omitted by the local block builder.
        previous = controller.getblock(controller.getbestblockhash())
        coinbase = create_coinbase(previous["height"] + 1)
        coinbase.vout[0].scriptPubKey = bytes.fromhex("ff5120" + "00" * 32)
        for payments in [[], [CTxOut(6 * COIN, bytes.fromhex("ff5120" + "11" * 32))],
                         [CTxOut(3 * COIN // 2, bytes.fromhex(script)) for script in payouts]]:
            candidate = deepcopy(coinbase)
            candidate.vout[0].nValue -= sum(out.nValue for out in payments)
            candidate.vout.extend(payments)
            candidate.rehash()
            bad = create_block(int(previous["hash"], 16), candidate, previous["time"] + 1)
            bad.solve()
            assert_equal(controller.submitblock(bad.serialize().hex()), "bad-pqmn-payee")

        def winner(block_hash):
            block = controller.getblock(block_hash, 2)
            outputs = block["tx"][0]["vout"]
            paid = [out for out in outputs if out["scriptPubKey"]["hex"] in payouts]
            assert_equal(len(paid), 1)
            assert_equal(paid[0]["value"], Decimal("6"))
            return payouts.index(paid[0]["scriptPubKey"]["hex"])

        rounds = []
        for _ in range(20):
            rounds.append(winner(mine()))
            record = next(row for row in controller.listpqmasternodes()
                          if row["registration"] == registrations[rounds[-1]])
            assert_equal(record["last_paid_height"], controller.getblockcount())
        assert_equal(sorted(rounds[:4]), [0, 1, 2, 3])
        for start in range(4, len(rounds), 4):
            assert_equal(rounds[:4], rounds[start:start + 4])
        if self.options.pos:
            assert "hashProofOfStake" in controller.getblock(controller.getbestblockhash())
        self.sync_all()

        # Disconnecting and reconnecting must restore the same queue position.
        next_block = mine()
        expected = winner(next_block)
        self.sync_all()
        for node in self.nodes:
            node.invalidateblock(next_block)
        replacement = mine()
        assert_equal(winner(replacement), expected)
        self.sync_all()
        self.restart_node(0)
        controller.walletpassphrase("pq-payments-test", 600)
        for node in self.nodes:
            node.setmocktime(self.mocktime)
        self.connect_nodes(0, 1)
        assert_equal(winner(mine()), rounds[1])
        self.sync_all()

        # Revocation/configuration are chain-derived eligibility, not a local
        # reachability guess. A returning node must join the back of the queue.
        returning = rounds[1]
        registration = registrations[returning]
        record = next(row for row in controller.listpqmasternodes() if row["registration"] == registration)
        controller.sendpqmasternode("revoke", {"registration": registration, "sequence": "1"},
                                    str(root / "revoke.dat"))
        mine()
        for _ in range(4):
            assert winner(mine()) != returning
        new_operator = controller.createpqoperator(str(root / "replacement-operator.dat"))["publickey"]
        controller.sendpqmasternode("update", {"registration": registration, "sequence": "2",
                                    "operator_publickey": new_operator, "payout_address": record["payout_address"]},
                                    str(root / "rotate.dat"))
        mine()
        for _ in range(4):
            assert winner(mine()) != returning
        controller.sendpqmasternode("service", {"registration": registration, "sequence": "3",
                                    "service": "[::1]:20200"}, str(root / "revive.dat"))
        mine()
        record = next(row for row in controller.listpqmasternodes() if row["registration"] == registration)
        assert_equal(record["revived_height"], controller.getblockcount())
        # The node paid in the SERVICE block ties the revival height. Preserve
        # the legacy registration-hash tie break (uint256's internal byte order),
        # rather than requiring the restored node to lose that tie artificially.
        queue = sorted(controller.listpqmasternodes(), key=lambda row: (
            max(row["last_paid_height"], row["revived_height"]) or row["registered_height"],
            bytes.fromhex(row["registration"])[::-1]))
        for row in queue:
            assert_equal(winner(mine()), registrations.index(row["registration"]))
        self.sync_all()
        assert controller.verifychain(0)
        assert observer.verifychain(0)


if __name__ == "__main__":
    PQMasternodePaymentsTest().main()
