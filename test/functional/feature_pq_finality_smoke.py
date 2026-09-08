#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Five-node finality progress, with PoS and payments during quorum loss.

Includes optional reindex and round-recovery regressions, not complete partition
or crash qualification.
"""
from decimal import Decimal
from io import BytesIO
from pathlib import Path
import hashlib
import http.client
import time

from test_framework.authproxy import JSONRPCException
from test_framework.permissions import make_private_directory
from test_framework.messages import (CBlock, CBlockHeader, CTransaction, COIN,
                                     deser_string, deser_vector, ser_string)
from test_framework.test_framework import PivxTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, assert_raises_rpc_error, set_node_times, wait_until


class FinalityTransaction(CTransaction):
    """Test-only PQ envelope support for a coinbase with no Sapling bundle."""
    def deserialize(self, stream):
        super().deserialize(stream)
        assert not self.sapData
        self.payload = None
        if self.nVersion >> 16:
            assert_equal(stream.read(1), b"\x01")  # optional extraPayload is present
            self.payload = deser_string(stream)

    def serialize_without_witness(self):
        return super().serialize_without_witness() + (b"\x01" + ser_string(self.payload) if self.payload is not None else b"")


class PQFinalitySmokeTest(PivxTestFramework):
    def add_options(self, parser):
        parser.add_option("--round-recovery", action="store_true", default=False)
        parser.add_option("--reindex", action="store_true", default=False)
        parser.add_option("--bootstrap-mismatch", action="store_true", default=False)
        parser.add_option("--committee-transitions", action="store_true", default=False)
        parser.add_option("--collateral-spend", action="store_true", default=False)
        parser.add_option("--crash-recovery", action="store_true", default=False)
        parser.add_option("--conflicting-fork", action="store_true", default=False)
        parser.add_option("--certified-fork", action="store_true", default=False)
        parser.add_option("--peer-fork", action="store_true", default=False)
        parser.add_option("--service-heartbeats", action="store_true", default=False)
        parser.add_option("--emergency-recovery", action="store_true", default=False)
        parser.add_option("--service-envelopes", action="store_true", default=False)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 5
        args = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                "-createwalletbackups=0", "-nuparams=pq_masternodes:1",
                "-nuparams=PoS:130", "-nuparams=PoS_v2:130",
                "-pqfinalitytimeoutscale=100", "-debug=pq"]
        self.extra_args = [args[:]] + [args + ["-disablewallet"] for _ in range(self.num_nodes - 1)]
        self.online = list(range(self.num_nodes))

    def setup_chain(self):
        # Options are parsed after set_test_params, before chain/node setup.
        if self.options.service_heartbeats or self.options.service_envelopes:
            for args in self.extra_args:
                args += ["-nuparams=pq_service:121"]
        if self.options.committee_transitions or self.options.collateral_spend or self.options.emergency_recovery:
            self.num_nodes = 6
            self.extra_args.append(self.extra_args[1][:])
            for args in self.extra_args:
                args += ["-nuparams=PoS:124", "-nuparams=PoS_v2:124"]
            self.online = list(range(self.num_nodes))
        super().setup_chain()

    def mesh(self):
        for i in self.online:
            for j in self.online:
                if i < j:
                    self.connect_nodes(i, j)

    def mine(self):
        hashes = []
        def produce():
            self.mocktime += 60
            set_node_times([self.nodes[i] for i in self.online], self.mocktime)
            try:
                hashes.extend(self.nodes[0].generatetoaddress(1, self.miner))
                return True
            except JSONRPCException as error:
                if "Couldn't create new blocks" not in error.error["message"]:
                    raise
                return False
        wait_until(produce, timeout=120)
        self.sync_blocks([self.nodes[i] for i in self.online])
        return hashes[0]

    def run_test(self):
        controller = self.nodes[0]
        root = Path(controller.datadir)
        controller.encryptwallet("finality-smoke")
        controller.walletpassphrase("finality-smoke", 99999)
        def address(label):
            return controller.getnewpqaddress(str(root / (label + ".dat")))["address"]
        self.miner = address("miner")
        controller.generatetoaddress(110, self.miner)
        credentials = root / "credentials"
        make_private_directory(credentials)
        registrations, keys = [], []
        for i in range(4):
            key = controller.createpqoperator(str(root / ("operator%d.dat" % i)))["publickey"]
            options = {"collateral_address": address("bond%d" % i),
                       "owner_address": address("owner%d" % i),
                       "operator_publickey": key, "payout_address": address("payout%d" % i),
                       "service": "127.0.0.1:%d" % (22000 + i)}
            registrations.append(controller.sendpqmasternode("register", options,
                str(root / ("registration%d.dat" % i)))["txid"])
            keys.append(key)
            controller.generatetoaddress(1, self.miner)
            controller.exportpqoperator(key, str(credentials / str(i)))
        controller.generatetoaddress(6, self.miner)
        self.sync_all()
        self.mocktime = controller.getblock(controller.getbestblockhash())["time"]
        initial = controller.getblockcount()
        bootstrap = "%d:%s:%s" % (initial, controller.getbestblockhash(),
            ":".join("%s:%s" % pair for pair in zip(registrations, keys)))
        if self.options.service_heartbeats:
            for i in range(1, 5):
                self.extra_args[i] += ["-pqoperatorcredentials=" + str(credentials / str(i - 1)),
                                       "-pqoperatorid=" + registrations[i - 1]]
                self.restart_node(i, self.extra_args[i])
            self.mesh()
            self.service_heartbeats(root, registrations, address)
            return
        assert_raises_rpc_error(-1, "configured walletless operator", controller.initpqjournal)
        for i in range(self.num_nodes):
            if 1 <= i <= 4:
                self.extra_args[i] += ["-pqoperatorcredentials=" + str(credentials / str(i - 1)),
                                       "-pqoperatorid=" + registrations[i - 1]]
                self.restart_node(i, self.extra_args[i])
                assert_equal(self.nodes[i].initpqjournal(), True)
                assert_raises_rpc_error(-1, "already-initialized", self.nodes[i].initpqjournal)
            self.extra_args[i] += ["-pqbootstrap=" + bootstrap]
            if self.options.round_recovery:
                self.extra_args[i] += ["-pqfinalitytimeoutscale=1"]
            self.restart_node(i, self.extra_args[i])
        controller.walletpassphrase("finality-smoke", 99999)
        set_node_times(self.nodes, self.mocktime)
        self.mesh()
        for node in self.nodes:
            wait_until(lambda: node.getpqfinalityinfo()["validated"], timeout=30)
            assert_equal(node.getpqfinalityinfo()["anchor_height"], initial)

        if self.options.round_recovery:
            self.round_recovery(initial)
            return
        if self.options.emergency_recovery:
            self.emergency_recovery(initial, bootstrap, root, credentials, registrations, address)
            return
        if self.options.committee_transitions or self.options.collateral_spend:
            self.committee_transitions(initial, bootstrap, root, credentials, registrations, address)
            return

        # Retain one isolated observer at the bootstrap checkpoint. Three of
        # the four registered voters still form a valid quorum.
        self.stop_node(4)
        self.online = [0, 1, 2, 3]

        # Wait for each real commitment before mining its carrier. This also
        # avoids falsely expecting finality without further certificate carriers.
        for expected in range(initial + 1, initial + 4):
            self.mine()
            wait_until(lambda: all(self.nodes[i].getpqfinalityinfo()["anchor_height"] >= expected
                                   for i in self.online), timeout=45)
            if expected == initial + 1:
                # The next miner loses all in-memory pending certificates.
                self.restart_node(0, self.extra_args[0])
                controller.walletpassphrase("finality-smoke", 99999)
                set_node_times([self.nodes[i] for i in self.online], self.mocktime)
                self.mesh()

        if self.options.conflicting_fork or self.options.certified_fork or self.options.peer_fork:
            self.conflicting_fork(initial)
            return

        if self.options.crash_recovery:
            self.crash_recovery(initial)
            return

        if self.options.bootstrap_mismatch:
            wrong = bootstrap.split(":")
            wrong[3], wrong[5] = wrong[5], wrong[3] # Valid distinct keys, wrong pinned identity association.
            args = [arg for arg in self.extra_args[4] if not arg.startswith("-pqbootstrap=")]
            self.start_node(4, args + ["-pqbootstrap=" + ":".join(wrong)])
            observer = self.nodes[4]
            assert_equal(observer.getpqfinalityinfo()["validated"], False)
            assert_equal(observer.submitblock(controller.getblock(controller.getblockhash(initial + 1), 0)), None)
            assert_equal(observer.submitblock(controller.getblock(controller.getblockhash(initial + 2), 0)),
                         "bad-pq-bootstrap")
            # This node already stored the real bootstrap before restart. A
            # wrong new configuration disables voting, not anchor protection.
            assert_equal(observer.getpqfinalityinfo()["anchor_height"], initial)
            return

        if self.options.reindex:
            for flag in ["-reindex-chainstate", "-reindex"]:
                self.mine() # Publish another certificate before replaying history.
                saved = controller.getpqfinalityinfo()["anchor_height"]
                tip = controller.getbestblockhash()
                registry = controller.listpqmasternodes()
                assert any(record["last_cert_height"] is not None for record in registry)
                self.restart_node(0, self.extra_args[0] + [flag])
                assert_equal(controller.getbestblockhash(), tip)
                assert_equal(controller.listpqmasternodes(), registry)
                assert controller.getpqfinalityinfo()["anchor_height"] >= saved
                # RPC warmup ends before the finality driver's first pass.
                wait_until(lambda: controller.getpqfinalityinfo()["validated"], timeout=30)
                controller.walletpassphrase("finality-smoke", 99999)
                set_node_times([self.nodes[i] for i in self.online], self.mocktime)
                self.mesh()
                self.sync_blocks([self.nodes[i] for i in self.online])
                self.mine()
                wait_until(lambda: controller.getpqfinalityinfo()["anchor_height"] > saved, timeout=60)
            return

        # A valid certificate inside an invalid carrier must not change the
        # observer's irreversible anchor. The target block itself is valid.
        self.start_node(4, self.extra_args[4])
        observer = self.nodes[4]
        observer.setmocktime(self.mocktime)
        wait_until(lambda: observer.getpqfinalityinfo()["anchor_height"] == initial, timeout=30)
        target = controller.getblock(controller.getblockhash(initial + 1), 0)
        assert_equal(observer.submitblock(target), None)
        bad = CBlock()
        raw = controller.getblock(controller.getblockhash(initial + 2), 0)
        stream = BytesIO(bytes.fromhex(raw))
        CBlockHeader.deserialize(bad, stream)
        bad.vtx = deser_vector(stream, FinalityTransaction)
        assert_equal(len(stream.read()), 0)  # this carrier is PoW, with no block signature
        assert_equal(bad.serialize().hex(), raw)
        bad.vtx[0].vout[0].nValue += COIN
        bad.vtx[0].rehash()
        bad.hashMerkleRoot = bad.calc_merkle_root()
        bad.solve()
        assert_equal(observer.submitblock(bad.serialize().hex()), "bad-blk-amount")
        assert_equal(observer.getpqfinalityinfo()["anchor_height"], initial)
        self.online = list(range(5))
        self.mesh()
        self.sync_blocks()

        # Remove two of four voters while retaining the funded staking wallet.
        # Previously formed certificates may still be published; no new vote
        # height can reach three signatures in the remaining two-member group.
        self.stop_node(3)
        self.stop_node(4)
        self.online = [0, 1, 2]
        self.mine()
        time.sleep(2)
        anchors = [self.nodes[i].getpqfinalityinfo()["anchor_height"] for i in self.online]
        recipient = address("recipient")
        txid = controller.sendpqtoaddress(recipient, 1, str(root / "payment.dat"))["txid"]
        included = False
        for _ in range(12):
            block = controller.getblock(self.mine(), 2)
            included |= any(tx["txid"] == txid for tx in block["tx"])
        assert included, "ordinary payment must confirm without a finality quorum"
        assert "hashProofOfStake" in block, "this must exercise actual PoS, not only PoW"
        paid = [out for out in block["tx"][0]["vout"] if out["value"] > 0]
        assert_equal(sum(out["value"] for out in paid), Decimal("6"))
        assert_equal([self.nodes[i].getpqfinalityinfo()["anchor_height"] for i in self.online], anchors)

        for i in [3, 4]:
            self.start_node(i, self.extra_args[i])
        self.online = list(range(5))
        set_node_times(self.nodes, self.mocktime)
        self.mesh()
        self.sync_blocks()
        # A two-versus-two round split cannot skip with the required three
        # reports. Allow natural timeout convergence: at scale100 the first
        # twelve rounds total 273 seconds, before network/scheduling margin.
        wait_until(lambda: all(node.getpqfinalityinfo()["anchor_height"] > anchors[0]
                               for node in self.nodes), timeout=300)
        self.mine()  # publish the recovered quorum's certificate through PoS

    def emergency_recovery(self, initial, bootstrap, root, credentials, registrations, address):
        controller = self.nodes[0]
        revoked = controller.sendpqmasternode("revoke", {"registration": registrations[3], "sequence": "1"},
                                              str(root / "emergency-revoke.dat"))["txid"]
        assert revoked in [tx["txid"] for tx in controller.getblock(self.mine(), 2)["tx"]]
        key = controller.createpqoperator(str(root / "emergency-operator.dat"))["publickey"]
        options = {"collateral_address": address("emergency-bond"), "owner_address": address("emergency-owner"),
                   "operator_publickey": key, "payout_address": address("emergency-payout"),
                   "service": "127.0.0.1:22005"}
        replacement = controller.sendpqmasternode("register", options,
                                                  str(root / "emergency-register.dat"))["txid"]
        self.mine()
        controller.exportpqoperator(key, str(credentials / "emergency"))
        base = [a for a in self.extra_args[5] if not a.startswith("-pqbootstrap=")]
        self.extra_args[5] = base + ["-pqoperatorcredentials=" + str(credentials / "emergency"),
                                    "-pqoperatorid=" + replacement]
        self.restart_node(5, self.extra_args[5])
        assert_equal(self.nodes[5].initpqjournal(), True)
        self.extra_args[5] += ["-pqbootstrap=" + bootstrap]
        self.restart_node(5, self.extra_args[5])
        set_node_times(self.nodes, self.mocktime)
        self.mesh()
        payment = controller.sendpqtoaddress(address("emergency-recipient"), 1,
                                             str(root / "emergency-payment.dat"))["txid"]
        included = False
        while controller.getblockcount() < 132:
            block = controller.getblock(self.mine(), 2)
            included |= payment in [tx["txid"] for tx in block["tx"]]
        assert included and "hashProofOfStake" in block
        assert_equal(sum(out["value"] for out in block["tx"][0]["vout"]), Decimal("6"))
        assert_equal([n.getpqfinalityinfo()["anchor_height"] for n in self.nodes], [initial] * 6)
        checkpoint, checkpoint_hash = block["height"], block["hash"]
        self.mine() # The restored checkpoint and its next height must both exist.
        for i, node in enumerate(self.nodes):
            self.extra_args[i] += ["-pqemergencycheckpoint=%d:%s" % (checkpoint, checkpoint_hash)]
            self.restart_node(i, self.extra_args[i])
        controller.walletpassphrase("finality-smoke", 99999)
        set_node_times(self.nodes, self.mocktime)
        self.mesh()
        wait_until(lambda: all(n.getpqfinalityinfo()["anchor_height"] >= checkpoint + 1 for n in self.nodes), timeout=45)
        self.mine()
        # Approval is durable, including when the argument is removed, and
        # history replay restores the recovery root without deleting journals.
        for flag in [None, "-reindex-chainstate", "-reindex"]:
            args = ([a for a in self.extra_args[0] if not a.startswith("-pqemergencycheckpoint=")] + [flag]
                    if flag else self.extra_args[0])
            self.restart_node(0, args)
            assert controller.getpqfinalityinfo()["anchor_height"] >= checkpoint
            assert_raises_rpc_error(-20, "PQ finalized anchor", controller.invalidateblock, checkpoint_hash)
            controller.walletpassphrase("finality-smoke", 99999)
            set_node_times(self.nodes, self.mocktime)
            self.mesh()
            self.sync_blocks()
        old = controller.getpqfinalityinfo()["anchor_height"]
        self.mine()
        wait_until(lambda: controller.getpqfinalityinfo()["anchor_height"] > old, timeout=45)

    def service_heartbeats(self, root, registrations, address):
        controller = self.nodes[0]
        self.mine() # activation; operators need neither a bootstrap nor a journal
        def all_published():
            self.mine()
            return all(record.get("last_heartbeat_height", 0) > 0
                       for record in controller.listpqmasternodes())
        wait_until(all_published, timeout=30)
        assert_equal(controller.getpqfinalityinfo()["configured"], False)
        self.stop_node(4)
        self.online = [0, 1, 2, 3]
        recipient = address("heartbeat-recipient")
        txid = controller.sendpqtoaddress(recipient, 1, str(root / "heartbeat-payment.dat"))["txid"]
        included = False
        # More than the regtest service window plus activation grace.
        for _ in range(30):
            block = controller.getblock(self.mine(), 2)
            included |= any(tx["txid"] == txid for tx in block["tx"])
        assert included
        assert "hashProofOfStake" in block
        records = {r["registration"]: r for r in controller.listpqmasternodes()}
        assert_equal(records[registrations[3]]["eligible"], False)
        assert all(records[id]["eligible"] for id in registrations[:3])
        paid = [out for out in block["tx"][0]["vout"] if out["value"] > 0]
        assert_equal(sum(out["value"] for out in paid), Decimal("6"))
        for flag in ["-reindex-chainstate", "-reindex"]:
            before = controller.listpqmasternodes()
            self.restart_node(0, self.extra_args[0] + [flag])
            assert_equal(controller.listpqmasternodes(), before)
            controller.walletpassphrase("finality-smoke", 99999)
            set_node_times([self.nodes[i] for i in self.online], self.mocktime)
            self.mesh()
        self.start_node(4, self.extra_args[4])
        self.online = list(range(5))
        self.nodes[4].setmocktime(self.mocktime)
        self.mesh()
        self.sync_blocks()
        def revived():
            self.mine()
            return next(r for r in controller.listpqmasternodes()
                        if r["registration"] == registrations[3])["eligible"]
        wait_until(revived, timeout=30)

    def conflicting_fork(self, initial):
        controller, isolated = self.nodes[0], self.nodes[4]
        saved = controller.getpqfinalityinfo()
        assert_equal(saved["anchor_height"], initial + 3)
        # A passive wallet-enabled miner builds the isolated PoW branch. Do not
        # give it operator credentials or change walletless operator RPC rules.
        fork_args = [arg for arg in self.extra_args[4] if arg != "-disablewallet"
                     and not arg.startswith(("-pqoperatorcredentials=", "-pqoperatorid="))]
        self.start_node(4, fork_args)
        isolated.setmocktime(self.mocktime + 60)
        fork = isolated.generatetoaddress(9, self.miner)
        assert_equal(isolated.getblockcount(), initial + 9)
        assert int(isolated.getblockheader(fork[-1])["chainwork"], 16) > int(
            controller.getblockheader(saved["anchor_hash"])["chainwork"], 16)
        assert_equal(isolated.getpqfinalityinfo()["anchor_height"], initial)
        if self.options.peer_fork:
            target = controller.getblockhash(initial + 1)
            tip = controller.getbestblockhash()
            assert_raises_rpc_error(-5, "Block not found", isolated.getblockheader, target)
            self.online = list(range(5))
            set_node_times(self.nodes, self.mocktime)
            self.mesh()
            self.sync_blocks()
            assert_equal(isolated.getbestblockhash(), tip)
            assert_equal(isolated.getblockhash(initial + 1), target)
            wait_until(lambda: isolated.getpqfinalityinfo()["anchor_height"] >= initial + 2, timeout=45)
            saved = isolated.getpqfinalityinfo()["anchor_height"]
            self.restart_node(4, fork_args)
            assert_equal(isolated.getbestblockhash(), tip)
            assert_equal(isolated.getpqfinalityinfo()["anchor_height"], saved)
            assert isolated.verifychain(4)
            self.log.info("Peer-only download adopted the certified lower-work branch and survived restart")
            return
        if self.options.certified_fork:
            self.certified_fork(initial, fork[-1])
            return
        # The higher-work fork conflicts with a saved anchor. It must not
        # cause DisconnectTip to abort the node that already has finality.
        for block in fork:
            assert controller.submitblock(isolated.getblock(block, 0)) in (None, "inconclusive")
            assert_equal(controller.getblockhash(initial + 3), saved["anchor_hash"])
            assert_equal(controller.getpqfinalityinfo()["anchor_height"], initial + 3)
        assert next(tip for tip in controller.getchaintips() if tip["hash"] == fork[-1])["status"] != "invalid"
        # Administrative candidate re-entry must respect the same anchor.
        controller.invalidateblock(fork[-1])
        controller.reconsiderblock(fork[-1])
        assert_equal(controller.getbestblockhash(), saved["anchor_hash"])
        self.mine()
        wait_until(lambda: controller.getpqfinalityinfo()["anchor_height"] == initial + 4, timeout=45)
        assert controller.verifychain(4)
        self.stop_node(2)
        self.stop_node(3)
        self.online = [0, 1]
        self.mine()
        self.mine()
        assert_equal(controller.getpqfinalityinfo()["anchor_height"], initial + 4)
        tip = controller.getbestblockhash()
        assert int(isolated.getblockheader(fork[-1])["chainwork"], 16) > int(
            controller.getblockheader(tip)["chainwork"], 16)
        # Refusing an administrative rollback must not poison block validity
        # flags or disconnect the unfinalized tail before checking finality.
        for protected in [saved["anchor_hash"], controller.getblockhash(initial)]:
            assert_raises_rpc_error(-20, "PQ finalized anchor", controller.invalidateblock, protected)
            assert_equal(controller.getbestblockhash(), tip)
        for flags in [[], ["-reindex-chainstate"], ["-reindex"]]:
            self.restart_node(0, self.extra_args[0] + flags)
            assert_equal(controller.getbestblockhash(), tip)
            assert_equal(controller.getpqfinalityinfo()["anchor_height"], initial + 4)
            assert_equal(controller.getblockhash(initial + 3), saved["anchor_hash"])
            assert controller.verifychain(4)
        self.log.info("A longer conflicting fork did not stop finality, block production, restart or reindex")

    def certified_fork(self, initial, longer_tip):
        controller, isolated = self.nodes[0], self.nodes[4]
        target = controller.getblockhash(initial + 1)
        carrier = controller.getblockhash(initial + 2)
        assert_equal(isolated.submitblock(controller.getblock(target, 0)), "inconclusive")
        assert_equal(isolated.getbestblockhash(), longer_tip)
        registry = isolated.listpqmasternodes()
        raw = controller.getblock(carrier, 0)
        bad = CBlock()
        stream = BytesIO(bytes.fromhex(raw))
        CBlockHeader.deserialize(bad, stream)
        bad.vtx = deser_vector(stream, FinalityTransaction)
        assert_equal(len(stream.read()), 0)
        assert_equal(bad.serialize().hex(), raw)
        payload = bad.vtx[0].payload
        bad.vtx[0].payload = payload[:-1] + bytes([payload[-1] ^ 1])
        bad.vtx[0].rehash()
        bad.hashMerkleRoot = bad.calc_merkle_root()
        bad.solve()
        assert_equal(isolated.submitblock(bad.serialize().hex()), "inconclusive")
        assert_equal(isolated.getbestblockhash(), longer_tip)
        assert_equal(isolated.getpqfinalityinfo()["anchor_height"], initial)
        bad.vtx[0].payload = payload
        # A genuine certificate is not permission to accept an invalid carrier.
        bad.vtx[0].vout[0].nValue += COIN
        bad.vtx[0].rehash()
        bad.hashMerkleRoot = bad.calc_merkle_root()
        bad.solve()
        assert_equal(isolated.submitblock(bad.serialize().hex()), "bad-blk-amount")
        assert_equal(isolated.getbestblockhash(), longer_tip)
        assert_equal(isolated.getpqfinalityinfo()["anchor_height"], initial)
        assert_equal(isolated.listpqmasternodes(), registry)
        passive_args = [arg for arg in self.extra_args[4]
                        if not arg.startswith(("-pqoperatorcredentials=", "-pqoperatorid="))]
        self.restart_node(4, passive_args)
        isolated.setmocktime(self.mocktime + 60)
        assert_equal(isolated.getbestblockhash(), longer_tip)
        assert_equal(isolated.getpqfinalityinfo()["anchor_height"], initial)
        assert_equal(isolated.listpqmasternodes(), registry)
        # Valid certificate-bearing history must override greater unfinalized work.
        assert_equal(isolated.submitblock(raw), None)
        assert_equal(isolated.getbestblockhash(), carrier)
        assert_equal(isolated.getpqfinalityinfo()["anchor_height"], initial + 1)
        assert_equal(isolated.getblockhash(initial + 1), target)
        assert isolated.verifychain(4)
        self.restart_node(4, passive_args)
        assert_equal(isolated.getbestblockhash(), carrier)
        assert_equal(isolated.getpqfinalityinfo()["anchor_height"], initial + 1)
        self.online = list(range(5))
        set_node_times(self.nodes, self.mocktime)
        self.mesh()
        self.sync_blocks()
        self.mine()
        wait_until(lambda: all(node.getpqfinalityinfo()["anchor_height"] >= initial + 3
                               for node in self.nodes), timeout=60)
        self.log.info("A certified lower-work branch was adopted; an invalid carrier restored the original chain")

    def crash_recovery(self, initial):
        controller, observer = self.nodes[0], self.nodes[4]

        def crash(node):
            # Deliberate process death: do not allow shutdown to flush caches.
            assert node.process.poll() is None
            node.process.kill()
            assert node.process.wait(timeout=30) != 0
            node.running = False
            node.process = None
            node.rpc_connected = False
            node.rpc = None

        # Passive observer: only an in-block certificate can advance its anchor.
        args = [arg for arg in self.extra_args[4]
                if not arg.startswith(("-pqoperatorcredentials=", "-pqoperatorid="))]
        self.start_node(4, args)
        observer.setmocktime(self.mocktime)
        for height in [initial + 1, initial + 2]:
            assert_equal(observer.submitblock(controller.getblock(controller.getblockhash(height), 0)), None)
        assert_equal(observer.getpqfinalityinfo()["anchor_height"], initial + 1)
        saved = observer.listpqmasternodes()
        crash(observer)
        self.start_node(4, args)
        assert_equal(observer.getblockcount(), initial + 2)
        assert_equal(observer.getpqfinalityinfo()["anchor_height"], initial + 1)
        assert_equal(observer.listpqmasternodes(), saved)
        self.log.info("Abrupt carrier-node restart retained finalized history and service evidence")

        # Operator: gossip finalization has no carrier yet for this target.
        operator = self.nodes[3]
        finalized = operator.getpqfinalityinfo()["anchor_height"]
        assert_equal(finalized, initial + 3)
        crash(operator)
        self.start_node(3, self.extra_args[3])
        assert operator.getblockcount() >= finalized
        assert_equal(operator.getpqfinalityinfo()["anchor_height"], finalized)
        self.log.info("Abrupt voting-node restart retained the unpublished finalized block")

        # Existing coin-DB fault injection exits during the first partial batch.
        # The anchor must not advance ahead of this failed prerequisite flush.
        self.restart_node(4, args + ["-dbcrashratio=1", "-dbbatchsize=1"])
        def anchor_logs():
            return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in (Path(observer.chain_path) / "pqanchors").glob("*.log")}
        before = anchor_logs()
        assert before, "the anchor database must have a write-ahead log"
        raw = controller.getblock(controller.getblockhash(initial + 3), 0)
        with observer.assert_debug_log(["Simulating a crash. Goodbye."]):
            try:
                observer.submitblock(raw)
            except (ConnectionError, http.client.HTTPException):
                pass
            observer.wait_until_stopped()
        assert_equal(anchor_logs(), before) # The failed flush must precede any new anchor record.
        observer.assert_start_raises_init_error(args, "Inconsistent block databases.*-reindex", ErrorMatch.PARTIAL_REGEX)
        self.start_node(4, args + ["-reindex"])
        assert_equal(observer.getblockcount(), initial + 3)
        assert_equal(observer.getpqfinalityinfo()["anchor_height"], initial + 2)
        assert observer.verifychain(4)
        self.restart_node(4, args)
        assert_equal(observer.getpqfinalityinfo()["anchor_height"], initial + 2)
        self.log.info("Interrupted coin flush refused ordinary startup; explicit reindex preserved finality")

    def committee_transitions(self, initial, bootstrap, root, credentials, registrations, address):
        """Real 4→5→4→5 handoffs; key rotation and removal preserve PoS."""
        controller = self.nodes[0]

        def anchored(height):
            return all(self.nodes[i].getpqfinalityinfo()["anchor_height"] >= height for i in self.online)

        def settle(height):
            # Each new carrier can publish one missing certificate; an earlier
            # quorum outage may leave a backlog of otherwise valid blocks.
            for _ in range(40):
                if anchored(height):
                    return
                previous = min(self.nodes[i].getpqfinalityinfo()["anchor_height"] for i in self.online)
                self.mine()
                wait_until(lambda: anchored(previous + 1), timeout=45)
            assert anchored(height), "committee handoff did not finalize"

        def sizes(expected):
            wait_until(lambda: all(self.nodes[i].getpqfinalityinfo()["committee_size"] == expected
                                   for i in self.online), timeout=30)

        def operation(action, registration, sequence, fields):
            txid = controller.sendpqmasternode(action, dict(fields, registration=registration, sequence=str(sequence)),
                str(root / (action + "-" + str(sequence) + "-" + registration + ".dat")))["txid"]
            block = controller.getblock(self.mine(), 2)
            assert txid in [tx["txid"] for tx in block["tx"]]
            height = block["height"]
            settle(height)
            return height

        key = controller.createpqoperator(str(root / "operator-fifth.dat"))["publickey"]
        payout = address("payout-fifth")
        options = {"collateral_address": address("bond-fifth"), "owner_address": address("owner-fifth"),
                   "operator_publickey": key, "payout_address": payout, "service": "127.0.0.1:22004"}
        fifth = controller.sendpqmasternode("register", options, str(root / "registration-fifth.dat"))["txid"]
        registrations.append(fifth)
        controller.exportpqoperator(key, str(credentials / "fifth"))
        self.mine()
        wait_until(lambda: anchored(initial + 1), timeout=45)
        sizes(5)
        self.log.info("Old four-member committee finalized the five-member handoff")

        base = [arg for arg in self.extra_args[5] if not arg.startswith("-pqbootstrap=")]
        self.extra_args[5] = base + ["-pqoperatorcredentials=" + str(credentials / "fifth"), "-pqoperatorid=" + fifth]
        self.restart_node(5, self.extra_args[5])
        assert_equal(self.nodes[5].initpqjournal(), True)
        self.extra_args[5] += ["-pqbootstrap=" + bootstrap]
        self.restart_node(5, self.extra_args[5])
        set_node_times(self.nodes, self.mocktime)
        self.mesh()
        self.mine()
        wait_until(lambda: anchored(initial + 2), timeout=45)
        assert_equal(self.nodes[5].getpqfinalityinfo()["member"], True)

        self.stop_node(4)
        self.stop_node(5)
        self.online = [0, 1, 2, 3]
        frozen = controller.getpqfinalityinfo()["anchor_height"]
        for _ in range(3):
            block = controller.getblock(self.mine(), 2)
        time.sleep(3)
        assert "hashProofOfStake" in block
        assert_equal(sum(out["value"] for out in block["tx"][0]["vout"]), Decimal("6"))
        assert_equal([self.nodes[i].getpqfinalityinfo()["anchor_height"] for i in self.online], [frozen] * 4)
        self.start_node(4, self.extra_args[4])
        self.online = [0, 1, 2, 3, 4] # Exactly four of five voters, fifth remains offline.
        set_node_times([self.nodes[i] for i in self.online], self.mocktime)
        self.mesh()
        # All four live voters must overlap a round; use the normal smoke-test
        # timing budget, not millisecond rounds shorter than reconnect latency.
        wait_until(lambda: anchored(frozen + 1), timeout=300)
        self.log.info("Three of five did not finalize; four of five recovered while PoS continued")

        replacement = controller.createpqoperator(str(root / "operator-fifth-replacement.dat"))["publickey"]
        operation("update", fifth, 1, {"operator_publickey": replacement, "payout_address": payout})
        sizes(4) # Rotation clears service until the new key announces it.
        operation("service", fifth, 2, {"service": "127.0.0.1:22004"})
        sizes(5)
        controller.exportpqoperator(replacement, str(credentials / "fifth-replacement"))
        # Catch up with the pinned checkpoint but without a signing identity.
        # A no-bootstrap node correctly refuses certificate-bearing blocks.
        self.start_node(5, base + ["-pqbootstrap=" + bootstrap])
        self.nodes[5].setmocktime(self.mocktime)
        self.connect_nodes(0, 5)
        self.sync_blocks([controller, self.nodes[5]])
        self.extra_args[5] = base + ["-pqoperatorcredentials=" + str(credentials / "fifth-replacement"),
                                     "-pqoperatorid=" + fifth]
        self.restart_node(5, self.extra_args[5])
        assert_equal(self.nodes[5].initpqjournal(), True)
        self.extra_args[5] += ["-pqbootstrap=" + bootstrap]
        self.restart_node(5, self.extra_args[5])
        self.online = list(range(6))
        set_node_times(self.nodes, self.mocktime)
        self.mesh()
        settle(controller.getblockcount())
        wait_until(lambda: self.nodes[5].getpqfinalityinfo()["member"], timeout=30)
        if self.options.collateral_spend:
            record = next(item for item in controller.listpqmasternodes() if item["registration"] == fifth)
            selected = [{"txid": record["collateral_txid"], "vout": record["collateral_vout"]}]
            spent = controller.sendpqtoaddress(address("retired-bond"), 1,
                        str(root / "retire-fifth.dat"), selected)["txid"]
            decoded = controller.decoderawtransaction(controller.gettransaction(spent)["hex"])
            assert_equal([(item["txid"], item["vout"]) for item in decoded["vin"]],
                         [(record["collateral_txid"], record["collateral_vout"])])
            block = controller.getblock(self.mine(), 2)
            assert spent in [tx["txid"] for tx in block["tx"]]
            removed_height = block["height"]
            settle(removed_height)
            for node in self.nodes:
                assert fifth not in [item["registration"] for item in node.listpqmasternodes()]
                assert_equal(node.gettxout(record["collateral_txid"], record["collateral_vout"]), None)
            # The historical five-member committee certifies its own removal;
            # the following height must then finalize with the remaining four.
            settle(removed_height + 1)
            self.restart_node(5, self.extra_args[5])
            self.nodes[5].setmocktime(self.mocktime)
            self.mesh()
            self.sync_blocks()
            assert self.nodes[5].verifychain(0)
        else:
            operation("revoke", fifth, 3, {})
        sizes(4)
        assert_equal(self.nodes[5].getpqfinalityinfo()["member"], False)
        self.log.info("Rotated key rejoined with new journal; retired operator left the finalized committee")

        # Fewer than four configured registrations means no next committee.
        # Do not lower the threshold or pretend that this handoff finalized.
        removed = controller.sendpqmasternode("revoke", {"registration": registrations[3], "sequence": "1"},
                                              str(root / "below-four.dat"))["txid"]
        block = controller.getblock(self.mine(), 2)
        assert removed in [tx["txid"] for tx in block["tx"]]
        frozen = block["height"] - 1
        settle(frozen)
        assert_equal(sum(record["eligible"] for record in controller.listpqmasternodes()), 3)
        payment = controller.sendpqtoaddress(address("below-four-recipient"), 1,
                                             str(root / "below-four-payment.dat"))["txid"]
        included = False
        for _ in range(3):
            block = controller.getblock(self.mine(), 2)
            included |= payment in [tx["txid"] for tx in block["tx"]]
            assert "hashProofOfStake" in block
            assert_equal(sum(out["value"] for out in block["tx"][0]["vout"]), Decimal("6"))
        assert included
        assert_equal([node.getpqfinalityinfo()["anchor_height"] for node in self.nodes], [frozen] * 6)
        self.log.info("Below four registrations: finality stayed pinned; payments, PoS and the 6 OLC share continued")

    def round_recovery(self, initial):
        """Isolated voters pass round99, adopt an unfinalized fork, then recover."""
        controller = self.nodes[0]
        for i in range(1, 5):
            self.stop_node(i)
        self.online = [0]
        original = self.mine()
        raw = controller.getblock(original, 0)
        for i in range(1, 5):
            self.start_node(i, self.extra_args[i])
            self.nodes[i].setmocktime(self.mocktime)
            assert_equal(self.nodes[i].submitblock(raw), None)

        def reached(round_number):
            message = "advanced to round %d at height %d" % (round_number, initial + 1)
            return all(message in (Path(node.chain_path) / "debug.log").read_text()
                       for node in self.nodes[1:])
        wait_until(lambda: reached(1), timeout=30)

        # A longer valid branch replaces the candidate without taking the
        # voters through an empty height or resetting their signing journals.
        controller.invalidateblock(original)
        replacement = self.mine()
        assert replacement != original
        longer = self.mine()
        for block in [replacement, longer]:
            raw = controller.getblock(block, 0)
            for node in self.nodes[1:]:
                result = node.submitblock(raw)
                if block == replacement:
                    assert result in (None, "inconclusive") # Valid side branch, not selected yet.
                else:
                    assert_equal(result, None)
                    assert_equal(node.getbestblockhash(), longer)
        self.sync_blocks()
        while controller.getblockcount() < 132:
            last = controller.getblock(self.mine())
        assert "hashProofOfStake" in last
        self.log.info("Waiting for four isolated voters to pass round99; PoS has continued")
        wait_until(lambda: reached(100), timeout=300)
        for node in self.nodes:
            assert_equal(node.getpqfinalityinfo()["anchor_height"], initial)
        self.online = list(range(5))
        set_node_times(self.nodes, self.mocktime)
        self.mesh()
        self.sync_blocks()
        wait_until(lambda: all(node.getpqfinalityinfo()["anchor_height"] >= initial + 1
                               for node in self.nodes), timeout=60)
        for node in self.nodes:
            assert_equal(node.getpqfinalityinfo()["anchor_hash"], replacement)
        self.mine()
        wait_until(lambda: all(node.getpqfinalityinfo()["anchor_height"] >= initial + 2
                               for node in self.nodes), timeout=60)


if __name__ == "__main__":
    PQFinalitySmokeTest().main()
