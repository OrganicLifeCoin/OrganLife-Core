#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Live PQ peer identity uses registered operator keys, never legacy MN authority."""
from pathlib import Path
import time

from test_framework.mininode import MESSAGEMAP, P2PInterface
from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, wait_until


class PQHello:
    command = b"pqhello"
    def __init__(self, payload=b""):
        self.payload = payload
    def serialize(self):
        return self.payload
    def deserialize(self, stream):
        self.payload = stream.read()


class PQAuth(PQHello):
    command = b"pqauth"


class Peer(P2PInterface):
    def peer_connect(self, *args, **kwargs):
        connect = super().peer_connect(*args, **kwargs)
        self.magic_bytes = bytes.fromhex("9bcf211d")
        self.on_connection_send_msg.nVersion = 70929
        return connect
    def on_pqhello(self, message): pass
    def on_pqauth(self, message): pass


MESSAGEMAP.update({b"pqhello": PQHello, b"pqauth": PQAuth})


class PQPeerAuthTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        args = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                "-createwalletbackups=0", "-nuparams=pq_masternodes:1"]
        self.extra_args = [args, args + ["-disablewallet"], args + ["-disablewallet"]]

    def run_test(self):
        controller = self.nodes[0]
        root = Path(controller.datadir)
        controller.encryptwallet("peer-auth-test")
        controller.walletpassphrase("peer-auth-test", 600)
        def address(name):
            return controller.getnewpqaddress(str(root / (name + ".dat")))["address"]
        mining = address("mining")
        controller.generatetoaddress(110, mining)
        registrations, credentials = [], []
        parent = root / "credentials"
        parent.mkdir(mode=0o700)
        for n in [1, 2]:
            operator = controller.createpqoperator(str(root / ("operator%d.dat" % n)))["publickey"]
            options = {"owner_address": address("owner%d" % n),
                       "collateral_address": address("collateral%d" % n),
                       "operator_publickey": operator, "payout_address": mining}
            registrations.append(controller.sendpqmasternode("register", options,
                                 str(root / ("register%d.dat" % n)))["txid"])
            controller.generatetoaddress(1, mining)
            destination = parent / str(n)
            controller.exportpqoperator(operator, str(destination))
            credentials.append(destination)
        controller.generatetoaddress(20, mining)
        self.sync_all()
        for n in [1, 2]:
            self.restart_node(n, self.extra_args[n] + ["-pqoperatorcredentials=" + str(credentials[n-1]),
                                                      "-pqoperatorid=" + registrations[n-1]])
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.connect_nodes(1, 2)
        def identities(node):
            return {peer.get("pq_registration") for peer in node.getpeerinfo()
                    if peer.get("pq_registration")}
        wait_until(lambda: registrations[1] in identities(self.nodes[1]), timeout=15)
        wait_until(lambda: registrations[0] in identities(self.nodes[2]), timeout=15)
        wait_until(lambda: set(registrations) <= identities(controller), timeout=15)
        for node in self.nodes:
            for peer in node.getpeerinfo():
                assert_equal(peer["masternode"], False)
                assert "verif_mn_proreg_tx_hash" not in peer
        controller.sendpqmasternode("revoke", {"registration": registrations[1], "sequence": "1"},
                                    str(root / "revoke.dat"))
        controller.generatetoaddress(1, mining)
        self.sync_blocks()
        wait_until(lambda: registrations[1] not in identities(self.nodes[1]), timeout=15)
        wait_until(lambda: registrations[1] not in identities(controller), timeout=15)
        # Untrusted wire inputs cannot reuse a connection's authentication slot.
        tip = bytes.fromhex(controller.getbestblockhash())[::-1]
        hello = b"\x01" + b"\x23" * 32 + tip
        for malformed in [b"", hello[:-1], hello + b"\x00", b"\x02" + hello[1:],
                          b"\x01" + b"\x00" * 32 + tip]:
            peer = controller.add_p2p_connection(Peer())
            peer.send_message(PQHello(malformed))
            peer.wait_for_disconnect()
        peer = controller.add_p2p_connection(Peer())
        peer.send_message(PQAuth(b"\x00" * 2453))
        peer.wait_for_disconnect()
        peer = controller.add_p2p_connection(Peer())
        peer.send_message(PQHello(hello))
        peer.sync_with_ping()
        assert_equal(len(peer.last_message["pqhello"].payload), 65)
        peer.send_message(PQHello(hello))
        peer.wait_for_disconnect()
        # A real operator signature captured on one connection cannot authenticate
        # a different connection/direction (replay and reflection protection).
        source = self.nodes[1].add_p2p_connection(Peer())
        source.send_message(PQHello(hello))
        source.wait_until(lambda: "pqauth" in source.last_message)
        proof = source.last_message["pqauth"].payload
        assert_equal(len(proof), 2453)
        assert_equal(proof[:33], b"\x01" + bytes.fromhex(registrations[0])[::-1])
        replay = controller.add_p2p_connection(Peer())
        replay.send_message(PQHello(hello))
        replay.sync_with_ping()
        replay.send_message(PQAuth(proof))
        replay.wait_for_disconnect()
        # Wrong-tip and silent peers retain ordinary traffic without identity.
        different = controller.add_p2p_connection(Peer())
        different.send_message(PQHello(b"\x01" + b"\x24" * 32 + b"\x25" * 32))
        different.sync_with_ping()
        silent = controller.add_p2p_connection(Peer())
        silent.send_message(PQHello(hello))
        silent.sync_with_ping()
        time.sleep(31)  # Real monotonic deadline, deliberately unaffected by mocktime.
        silent.sync_with_ping()
        silent.send_message(PQAuth(proof))
        silent.wait_for_disconnect()
        # Rotation invalidates existing identity and prevents stale credentials
        # signing on a new connection. A correctly provisioned replacement works.
        replacement = controller.createpqoperator(str(root / "replacement.dat"))["publickey"]
        controller.sendpqmasternode("update", {"registration": registrations[0], "sequence": "1",
                                    "operator_publickey": replacement, "payout_address": mining},
                                    str(root / "rotation.dat"))
        rotated_block = controller.generatetoaddress(1, mining)[0]
        self.sync_blocks()
        wait_until(lambda: registrations[0] not in identities(controller), timeout=15)
        wait_until(lambda: registrations[0] not in identities(self.nodes[2]), timeout=15)
        stale = self.nodes[1].add_p2p_connection(Peer())
        fresh_tip = bytes.fromhex(controller.getbestblockhash())[::-1]
        stale.send_message(PQHello(b"\x01" + b"\x26" * 32 + fresh_tip))
        stale.sync_with_ping()
        assert "pqauth" not in stale.last_message
        replacement_dir = parent / "replacement"
        controller.exportpqoperator(replacement, str(replacement_dir))
        replacement_args = self.extra_args[1] + ["-pqoperatorcredentials=" + str(replacement_dir),
                                                "-pqoperatorid=" + registrations[0]]
        self.restart_node(1, replacement_args)
        self.connect_nodes(0, 1)
        self.connect_nodes(1, 2)
        wait_until(lambda: registrations[0] in identities(controller), timeout=15)
        wait_until(lambda: registrations[0] in identities(self.nodes[2]), timeout=15)
        for node in self.nodes:
            node.invalidateblock(rotated_block)
        self.sync_blocks()
        assert registrations[0] not in identities(controller)
        assert registrations[0] not in identities(self.nodes[2])
        for node in self.nodes:
            node.reconsiderblock(rotated_block)
        self.sync_blocks()
        # Restoring the same registration/key does not revive an invalidated session.
        assert registrations[0] not in identities(controller)
        assert registrations[0] not in identities(self.nodes[2])
        self.restart_node(1, replacement_args)
        self.connect_nodes(0, 1)
        wait_until(lambda: registrations[0] in identities(controller), timeout=15)


if __name__ == "__main__":
    PQPeerAuthTest().main()
