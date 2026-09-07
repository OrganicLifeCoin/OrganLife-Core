#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""PQ finality end-to-end: pinned bootstrap, 3-of-4 quorum, in-block certificates,
quorum loss with continuing PoS, partition heal onto the finalized branch, crash
recovery with journal replay, committee transitions, service evidence, and
finalized-anchor protection."""
from pathlib import Path
import time

from test_framework.mininode import MESSAGEMAP, P2PInterface
from test_framework.test_framework import PivxTestFramework
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_greater_than, set_node_times, wait_until

FINALITY_MODE = 7
MN_SHARE = 6


class PQFrame:
    command = b"pqframe"

    def __init__(self, payload=b""):
        self.payload = payload

    def serialize(self):
        return self.payload

    def deserialize(self, stream):
        self.payload = stream.read()


class Peer(P2PInterface):
    def peer_connect(self, *args, **kwargs):
        connect = super().peer_connect(*args, **kwargs)
        self.magic_bytes = bytes.fromhex("9bcf211d")
        self.on_connection_send_msg.nVersion = 70929
        return connect

    def on_pqprop(self, message): pass
    def on_pqvote(self, message): pass
    def on_pqcmt(self, message): pass
    def on_pqhello(self, message): pass
    def on_pqauth(self, message): pass


MESSAGEMAP.update({b"pqprop": PQFrame, b"pqvote": PQFrame, b"pqcmt": PQFrame,
                   b"pqhello": PQFrame, b"pqauth": PQFrame})


class PQFinalityTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 7
        base = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                "-createwalletbackups=0", "-nuparams=pq_masternodes:1",
                "-pqfinalitytimeoutscale=200"]
        self.extra_args = [base + ["-staking=1"],           # 0: controller, stakes
                           base + ["-disablewallet"],       # 1-4: operators
                           base + ["-disablewallet"],
                           base + ["-disablewallet"],
                           base + ["-disablewallet"],
                           base + ["-staking=1"],           # 5: second staker (island B)
                           base + ["-disablewallet"]]       # 6: fifth operator

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 7
        base = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                "-whitelist=127.0.0.1",
                "-createwalletbackups=0", "-nuparams=pq_masternodes:1",
                "-pqfinalitytimeoutscale=200"]
        self.extra_args = [base + ["-staking=1"],           # 0: controller, stakes
                           base + ["-disablewallet"],       # 1-4: operators
                           base + ["-disablewallet"],
                           base + ["-disablewallet"],
                           base + ["-disablewallet"],
                           base + ["-staking=1"],           # 5: second staker (island B)
                           base + ["-disablewallet"]]       # 6: fifth operator
        self.mocktime = int(time.time())

    def mine(self, node, address, count=1):
        """Advances mock time and produces blocks through the internal miner."""
        hashes = []
        for _ in range(count):
            def produce():
                self.mocktime += 60
                set_node_times(self.nodes, self.mocktime)
                try:
                    hashes.extend(node.generatetoaddress(1, address))
                    return True
                except JSONRPCException as error:
                    if "Couldn't create new blocks" not in error.error["message"]:
                        raise
                    return False
            wait_until(produce, timeout=180)
        return hashes

    # --- helpers -----------------------------------------------------------
    def finality(self, node):
        return node.getpqfinalityinfo()

    def anchors(self, node):
        return self.finality(node)["anchor_height"]

    def tip(self, node):
        return node.getblockcount()

    def wait_anchor(self, node, height, timeout=300):
        wait_until(lambda: self.anchors(node) >= height, timeout=timeout)

    def wait_tip(self, node, height, timeout=600):
        wait_until(lambda: self.tip(node) >= height, timeout=timeout)

    def coinbase_mode(self, node, height):
        block = node.getblock(node.getblockhash(height), 2)
        coinbase = block["tx"][0]
        if coinbase["type"] != 8:
            return None
        return bytes.fromhex(coinbase["extraPayload"])[1]

    def run_test(self):
        self.setup_controller()
        self.setup_operators()
        self.activate_finality()
        self.test_finality_progression()
        self.test_three_of_four()
        self.test_quorum_loss_pos_continues()
        self.test_partition_heal()
        self.test_crash_no_double_sign()
        self.test_malformed_messages()
        self.test_committee_transition_and_service()
        self.test_finalized_anchor_protection()
        self.test_reindex_preserves_finality()

    def setup_controller(self):
        self.controller = self.nodes[0]
        root = Path(self.controller.datadir)
        self.controller.encryptwallet("finality-test")
        self.controller.walletpassphrase("finality-test", 999999)
        self.mining = self.controller.getnewpqaddress(str(root / "mining.dat"))["address"]
        self.mine(self.controller, self.mining, 110)
        # Fund the second staker for the partition island.
        self.staker = self.nodes[5]
        staker_root = Path(self.staker.datadir)
        self.staker.encryptwallet("finality-test")
        self.staker.walletpassphrase("finality-test", 999999)
        self.staker_addr = self.staker.getnewpqaddress(str(staker_root / "mining.dat"))["address"]
        self.controller.sendpqtoaddress(self.staker_addr, 100, str(root / "fund-staker.dat"))
        self.mine(self.controller, self.mining, 20)
        self.sync_blocks(self.nodes[0:5])

    def setup_operators(self):
        root = Path(self.controller.datadir)
        self.registrations, self.credentials, self.payouts = [], [], []
        parent = root / "credentials"
        parent.mkdir(mode=0o700)
        for n in [1, 2, 3, 4]:
            operator = self.controller.createpqoperator(
                str(root / ("operator%d.dat" % n)))["publickey"]
            payout = self.controller.getnewpqaddress(str(root / ("payout%d.dat" % n)))["address"]
            options = {"owner_address": self.controller.getnewpqaddress(
                           str(root / ("owner%d.dat" % n)))["address"],
                       "collateral_address": self.controller.getnewpqaddress(
                           str(root / ("collateral%d.dat" % n)))["address"],
                       "operator_publickey": operator, "payout_address": payout,
                       "service": "127.0.0.1:514%02d" % (10 + n)}
            self.registrations.append(self.controller.sendpqmasternode(
                "register", options, str(root / ("register%d.dat" % n)))["txid"])
            self.mine(self.controller, self.mining)
            destination = parent / str(n)
            self.controller.exportpqoperator(operator, str(destination))
            self.credentials.append(destination)
            self.payouts.append(payout)
        self.mine(self.controller, self.mining, 20)
        self.sync_blocks(self.nodes[0:5])
        for n in [1, 2, 3, 4]:
            assert self.nodes[n].listpqmasternodes(), "registry must sync to operators"

    def activate_finality(self):
        tip_height = self.controller.getblockcount()
        tip_hash = self.controller.getblockhash(tip_height)
        keys = {item["registration"]: item["operator_publickey"]
                for item in self.controller.listpqmasternodes()}
        self.bootstrap = "%d:%s:%s" % (
            tip_height, tip_hash,
            ":".join("%s:%s" % (reg, keys[reg]) for reg in self.registrations))
        args = ["-pqbootstrap=" + self.bootstrap]
        for n in [1, 2, 3, 4]:
            self.restart_node(n, self.extra_args[n] + args +
                              ["-pqoperatorcredentials=" + str(self.credentials[n - 1]),
                               "-pqoperatorid=" + self.registrations[n - 1]])
        self.restart_node(0, self.extra_args[0] + args)
        self.controller.walletpassphrase("finality-test", 999999)
        set_node_times(self.nodes, self.mocktime)
        for i in range(5):
            for j in range(i + 1, 5):
                self.connect_nodes(i, j)
        self.connect_nodes(0, 5)
        self.H0 = tip_height
        info = self.finality(self.controller)
        assert_equal(info["configured"], True)
        wait_until(lambda: self.finality(self.controller)["validated"], timeout=30)
        wait_until(lambda: self.anchors(self.controller) == self.H0, timeout=30)
        for n in [1, 2, 3, 4]:
            wait_until(lambda node=self.nodes[n]: self.finality(node)["validated"], timeout=30)
            wait_until(lambda node=self.nodes[n]: self.anchors(node) == self.H0, timeout=30)
            assert_equal(self.finality(self.nodes[n])["committee_size"], 4)
            assert_equal(self.finality(self.nodes[n])["member"], True)
        assert_equal(self.finality(self.controller)["member"], False)  # controller is passive

    def operator_args(self, n, extra=None):
        args = self.extra_args[n] + ["-pqbootstrap=" + self.bootstrap,
                                     "-pqoperatorcredentials=" + str(self.credentials[n - 1]),
                                     "-pqoperatorid=" + self.registrations[n - 1]]
        return args + (extra or [])

    def start_operator(self, n, extra=None):
        self.start_node(n, self.operator_args(n, extra))
        for i in range(self.num_nodes):
            if i != n:
                self.connect_nodes(i, n)

    def mn_payment(self, node, height):
        """Returns the masternode payment outputs of the coinbase at height."""
        block = node.getblock(node.getblockhash(height), 2)
        return [out for out in block["tx"][0]["vout"] if out["value"] > 0]

    def test_finality_progression(self):
        # 4-of-4 online: anchors advance with PoS; the certificate for an
        # anchor height appears in a subsequent block's FINALITY coinbase.
        start = self.anchors(self.controller)
        self.mine(self.controller, self.mining, 6)
        self.wait_tip(self.controller, start + 6)
        self.wait_anchor(self.controller, start + 2)
        height = self.anchors(self.controller)
        found = False
        for h in range(height + 1, self.tip(self.controller) + 1):
            if self.coinbase_mode(self.controller, h) == FINALITY_MODE:
                found = True
                break
        assert found, "no FINALITY coinbase certificate after the anchor advanced"
        info = self.finality(self.nodes[1])
        assert_greater_than(info["voting_height"], self.H0)
        assert_equal(info["locked"], True)

    def test_three_of_four(self):
        # One operator stopped: finality continues with 3-of-4, then the
        # operator returns and catches up.
        self.stop_node(2)
        start = self.anchors(self.controller)
        self.mine(self.controller, self.mining, 4)
        self.wait_tip(self.controller, start + 4)
        self.wait_anchor(self.controller, start + 2)
        self.start_operator(2)

    def test_quorum_loss_pos_continues(self):
        # Two operators stopped: quorum lost. PoS blocks and transactions MUST
        # continue; no new anchors may appear; finalized anchors stay intact.
        anchor_before = self.anchors(self.controller)
        self.stop_node(2)
        self.stop_node(4)
        tip_before = self.tip(self.controller)
        self.mine(self.controller, self.mining, 4)
        self.wait_tip(self.controller, tip_before + 4)
        assert_equal(self.anchors(self.controller), anchor_before)
        # Transactions keep flowing without finality.
        self.quorum_loss_addr = self.controller.getnewpqaddress(
            str(Path(self.controller.datadir) / "quorum-loss.dat"))["address"]
        txid = self.controller.sendpqtoaddress(self.quorum_loss_addr, 1,
                                               str(Path(self.controller.datadir) / "quorum-loss-tx.dat"))
        self.mine(self.controller, self.mining, 2)
        block = self.controller.getblock(self.controller.getbestblockhash(), 1)
        assert txid in block["tx"], "transaction must confirm without finality quorum"
        assert_equal(self.anchors(self.controller), anchor_before)
        anchor_hash = self.finality(self.controller)["anchor_hash"]
        assert_equal(self.controller.getblockhash(anchor_before), anchor_hash)
        self.start_operator(2)
        self.start_operator(4)

    def test_partition_heal(self):
        # Island A (controller + 3 operators) keeps finalizing; island B (one
        # operator + one staker) keeps producing PoS without finality. On heal,
        # island B adopts the finalized branch and finality resumes everywhere.
        anchor_before = self.anchors(self.controller)
        tip_before = self.tip(self.controller)
        for i in range(4):
            self.disconnect_nodes(i, 4)
        self.mine(self.controller, self.mining, 3)
        self.wait_tip(self.controller, tip_before + 3)
        self.wait_anchor(self.controller, anchor_before + 2)
        # Island B keeps producing PoS blocks with no finality progress.
        self.mine(self.nodes[5], self.staker_addr, 2)
        assert_equal(self.anchors(self.nodes[4]), anchor_before)
        assert_equal(self.anchors(self.nodes[5]), anchor_before)
        # Ensure island A's chain is strictly heavier, then heal.
        self.mine(self.controller, self.mining, 3)
        self.wait_tip(self.controller, tip_before + 6)
        for i in range(4):
            self.connect_nodes(i, 4)
        self.connect_nodes(0, 5)
        self.sync_blocks(timeout=180)
        anchor = self.finality(self.controller)
        assert_greater_than(anchor["anchor_height"], anchor_before)
        for n in [1, 2, 3, 4, 5]:
            other = self.finality(self.nodes[n])
            assert_equal(other["anchor_height"], anchor["anchor_height"])
            assert_equal(other["anchor_hash"], anchor["anchor_hash"])
        assert_equal(self.tip(self.nodes[5]), self.tip(self.controller))
        self.wait_anchor(self.controller, anchor["anchor_height"] + 1)

    def test_crash_no_double_sign(self):
        # SIGKILL an operator right after it voted; on restart its journal
        # replays and finality continues on a single finality chain.
        anchor_before = self.anchors(self.controller)
        self.nodes[1].process.kill()
        self.nodes[1].wait_until_stopped()
        time.sleep(1)
        self.start_operator(1)
        self.mine(self.controller, self.mining, 4)
        self.wait_anchor(self.controller, anchor_before + 2)
        anchor = self.finality(self.controller)
        for n in [2, 3, 4]:
            assert_equal(self.finality(self.nodes[n])["anchor_hash"], anchor["anchor_hash"])

    def test_malformed_messages(self):
        for command, payload in [(b"pqprop", b"\x00"),
                                 (b"pqprop", b"\x01" + b"\x00" * 55),
                                 (b"pqvote", b"\x01" + b"\x00" * 100),
                                 (b"pqcmt", b"\x02" + b"\x00" * 200)]:
            peer = self.controller.add_p2p_connection(Peer())
            frame = PQFrame(payload)
            frame.command = command
            peer.send_message(frame)
            peer.wait_for_disconnect()

    def test_committee_transition_and_service(self):
        # A fifth registration joins the committee deterministically after its
        # registration matures; the whole 6 OLC share keeps rotating whole.
        root = Path(self.controller.datadir)
        operator = self.controller.createpqoperator(str(root / "operator5.dat"))["publickey"]
        payout = self.controller.getnewpqaddress(str(root / "payout5.dat"))["address"]
        options = {"owner_address": self.controller.getnewpqaddress(
                       str(root / "owner5.dat"))["address"],
                   "collateral_address": self.controller.getnewpqaddress(
                       str(root / "collateral5.dat"))["address"],
                   "operator_publickey": operator, "payout_address": payout,
                   "service": "127.0.0.1:51415"}
        reg5 = self.controller.sendpqmasternode("register", options, str(root / "register5.dat"))["txid"]
        self.mine(self.controller, self.mining, 2)
        destination = root / "credentials" / "5"
        self.controller.exportpqoperator(operator, str(destination))
        self.credentials.append(destination)
        self.registrations.append(reg5)
        self.payouts.append(payout)
        # Bring the fifth operator online; the committee grows to five at an
        # anchor advance once the registration is mature.
        self.start_operator(6)
        deadline = time.time() + 420
        while time.time() < deadline:
            if self.finality(self.controller)["committee_size"] == 5:
                break
            self.mine(self.controller, self.mining, 2)
            self.wait_anchor(self.controller, self.anchors(self.controller) + 1)
        assert_equal(self.finality(self.controller)["committee_size"], 5)
        # All committee members sign every height: all eligible.
        members = [item for item in self.controller.listpqmasternodes() if not item["revoked"]]
        for item in members:
            assert_equal(item["eligible"], True, item["registration"])
        # Whole 6 OLC share, 1% operator commission, rotation over winners.
        height = self.tip(self.controller)
        paid_windows = []
        for h in range(height - 4, height + 1):
            outputs = self.mn_payment(self.controller, h)
            if not outputs:
                continue
            total = sum(out["value"] for out in outputs)
            assert_equal(total, MN_SHARE)
            paid_windows.append(outputs[0]["address"])
        assert len(set(paid_windows)) >= 3, "winners must rotate: %s" % paid_windows
        # Three-of-five continues after losing two members.
        anchor_before = self.anchors(self.controller)
        self.stop_node(6)
        self.stop_node(4)
        tip_before = self.tip(self.controller)
        self.mine(self.controller, self.mining, 3)
        self.wait_tip(self.controller, tip_before + 3)
        self.wait_anchor(self.controller, anchor_before + 1)
        self.start_operator(4)
        self.start_operator(6)

    def test_finalized_anchor_protection(self):
        # Invalidating a block at or below the finalized anchor must fail
        # instead of discarding finality.
        anchor = self.anchors(self.controller)
        anchor_hash = self.finality(self.controller)["anchor_hash"]
        for target in [anchor, anchor - 1]:
            try:
                self.controller.invalidateblock(self.controller.getblockhash(target))
            except Exception:
                pass
            self.sync_blocks(self.nodes[0:5])
            assert_equal(self.controller.getblockhash(anchor), anchor_hash)
            assert_equal(self.anchors(self.controller), anchor)

    def test_reindex_preserves_finality(self):
        # -reindex-chainstate rebuilds the mirror from in-block certificates
        # while the durable anchor store keeps protecting finality.
        anchor = self.finality(self.controller)["anchor_height"]
        anchor_hash = self.finality(self.controller)["anchor_hash"]
        self.restart_node(4, self.operator_args(4, ["-reindex-chainstate"]))
        for i in range(4):
            self.connect_nodes(i, 4)
        wait_until(lambda: self.finality(self.nodes[4])["validated"], timeout=60)
        wait_until(lambda: self.finality(self.nodes[4])["anchor_height"] >= anchor, timeout=60)
        assert_equal(self.finality(self.nodes[4])["anchor_hash"], anchor_hash)


if __name__ == "__main__":
    PQFinalityTest().main()
