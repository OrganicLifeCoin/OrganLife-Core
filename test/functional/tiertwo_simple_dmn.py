#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the simple deterministic masternode flow (legacy-style UX wrapper)."""

import os
from decimal import Decimal

from test_framework.messages import COutPoint
from test_framework.test_framework import PivxTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
    is_coin_locked_by,
    p2p_port,
)

# Regtest BLS secret key bech32 HRP (see CRegTestParams in src/chainparams.cpp)
REGTEST_BLS_SECRET_HRP = "olc-bls-sk-regtest"
# Regtest deterministic MN collateral: consensus.nMNCollateralAmt = 100 * COIN
MN_COLLATERAL = Decimal('100')
# v6_evo (DMN enforcement) activation height, set with -nuparams
V6_EVO_ACTIVATION = 130


class SimpleDMNTest(PivxTestFramework):

    def set_test_params(self):
        # 1 controller wallet (all the MN keys + collateral + registration)
        # v5_shield (SAPLING tx version) must be active from height 1, or the
        # registration tx (version 3) is rejected as non-standard ("version").
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.extra_args = [["-nuparams=v5_shield:1", "-nuparams=v6_evo:%d" % V6_EVO_ACTIVATION]] * self.num_nodes

    def masternode_conf_path(self, node_index):
        return os.path.join(self.options.tmpdir, "node%d" % node_index, "regtest", "masternode.conf")

    def run_test(self):
        self.log.info("Generate coins and activate V6_evo")
        self.nodes[0].generate(V6_EVO_ACTIVATION + 10)
        self.sync_all()
        assert_equal(self.nodes[0].getblockcount(), V6_EVO_ACTIVATION + 10)
        self.log.info("V6_evo active at height %d" % self.nodes[0].getblockcount())

        self.log.info("Create DMN key set")
        keyres = self.nodes[0].createmasternodekey("dmn", "mn1")
        assert_equal(sorted(keyres.keys()),
                     ["confLine", "operatorPrivKey", "ownerAddress", "payoutAddress", "votingAddress"])
        assert_equal(keyres["votingAddress"], keyres["ownerAddress"])
        assert keyres["payoutAddress"] != keyres["ownerAddress"], \
            "payout address must be distinct from the owner address (consensus: bad-protx-payee-reuse)"
        assert keyres["operatorPrivKey"].startswith(REGTEST_BLS_SECRET_HRP + "1"), \
            "operatorPrivKey must be bech32 with the regtest BLS secret HRP '%s' (got %s)" % (
                REGTEST_BLS_SECRET_HRP, keyres["operatorPrivKey"])
        assert_equal(keyres["confLine"], "mn1 YOUR_VPS_IP:PORT %s" % keyres["operatorPrivKey"])
        self.log.info("Key set OK")

        self.log.info("createmasternodekey dmn without alias must fail")
        assert_raises_rpc_error(-8, "alias", self.nodes[0].createmasternodekey, "dmn")

        self.log.info("Write 3-field masternode.conf and restart to verify parse persistence")
        mn_ip = "127.0.0.1:%d" % p2p_port(0)
        conf_line = "mn1 %s %s" % (mn_ip, keyres["operatorPrivKey"])
        conf_path = self.masternode_conf_path(0)
        with open(conf_path, "w", encoding="utf8") as f:
            f.write(conf_line + "\n")
        self.restart_node(0, extra_args=self.extra_args[0])
        self.connect_nodes(0, 1)
        self.connect_nodes(0, 2)
        self.sync_all()
        conf = self.nodes[0].listmasternodeconf()
        assert_equal(len(conf), 1)
        entry = conf[0]
        assert_equal(entry["alias"], "mn1")
        assert_equal(entry["address"], mn_ip)
        assert_equal(entry["privateKey"], keyres["operatorPrivKey"])
        assert_equal(entry["type"], "deterministic")
        assert_equal(entry["status"], "MISSING")   # not registered yet
        self.log.info("3-field conf parsed at daemon start")

        self.log.info("Start the DMN via startmasternode alias (with conf reload)")
        res = self.nodes[0].startmasternode("alias", False, "mn1", True)
        assert_equal(res["alias"], "mn1")
        assert_equal(res["result"], "success")
        assert_equal(res["vpsConfig"], "-mnoperatorprivatekey=%s" % keyres["operatorPrivKey"])
        txid = res["txid"]
        self.log.info("Registration txid: %s" % txid)

        self.log.info("Verify registration tx embeds locked collateral")
        tx = self.nodes[0].getrawtransaction(txid, True)
        assert "payload" in tx, "registration tx must be a ProReg tx with a payload"
        pl = tx["payload"]
        assert_equal(pl["service"], mn_ip)
        assert_equal(pl["ownerAddress"], keyres["ownerAddress"])
        assert_equal(pl["votingAddress"], keyres["ownerAddress"])
        assert_equal(pl["payoutAddress"], keyres["payoutAddress"])
        assert pl["operatorPubKey"].startswith("olc-bls-pk-regtest1")
        assert_equal(pl["collateralHash"], "0" * 64)   # collateral funded within the ProReg tx itself
        coll_vouts = [o for o in tx["vout"] if o["value"] == MN_COLLATERAL]
        assert_equal(len(coll_vouts), 1)
        coll_idx = coll_vouts[0]["n"]
        assert_equal(pl["collateralIndex"], coll_idx)
        # collateral output must be locked by the wallet right after registration
        assert is_coin_locked_by(self.nodes[0], COutPoint(int(txid, 16), coll_idx)), \
            "collateral vout %s:%d must be locked" % (txid, coll_idx)
        locked = self.nodes[0].listlockunspent()["transparent"]
        assert {"txid": txid, "vout": coll_idx} in locked, locked
        self.log.info("Collateral vout %d locked" % coll_idx)

        self.log.info("Mine and verify DMN is active")
        self.nodes[0].generate(3)
        self.sync_all()
        assert_equal(self.nodes[0].getrawtransaction(txid, True)["confirmations"], 3)
        # protx list (non-verbose: list of txids)
        assert txid in self.nodes[0].protx_list(False)
        # protx list (verbose, valid only)
        protx = [p for p in self.nodes[0].protx_list(True, False, True) if p["proTxHash"] == txid]
        assert_equal(len(protx), 1)
        assert_equal(protx[0]["collateralHash"], txid)
        assert_equal(protx[0]["dmnstate"]["service"], mn_ip)
        # masternode count
        count = self.nodes[0].getmasternodecount()
        assert_equal(count["total"], 1)
        assert_equal(count["enabled"], 1)
        # conf entry status
        conf = self.nodes[0].listmasternodeconf()
        entry = [c for c in conf if c["alias"] == "mn1"][0]
        assert_equal(entry["status"], "ENABLED")
        self.log.info("DMN enabled and registered")

        self.log.info("All good.")


if __name__ == '__main__':
    SimpleDMNTest().main()
