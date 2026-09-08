#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying file COPYING.
"""Controller registration/lifecycle transactions reach a separate walletless node."""
from pathlib import Path
import json

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class PQMasternodeWalletTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        args = ["-connect=0", "-dnsseed=0", "-discover=0", "-staking=0",
                "-createwalletbackups=0", "-nuparams=pq_masternodes:1"]
        self.extra_args = [args, args + ["-disablewallet"]]

    def run_test(self):
        controller, observer = self.nodes
        root = Path(controller.datadir)
        controller.encryptwallet("controller-test")
        controller.walletpassphrase("controller-test", 600)
        addresses = {role: controller.getnewpqaddress(str(root / (role + ".dat")))["address"]
                     for role in ["mining", "collateral", "owner", "payout"]}
        operator = controller.createpqoperator(str(root / "operator.dat"))["publickey"]
        controller.generatetoaddress(110, addresses["mining"])
        self.sync_all()
        options = {"collateral_address": addresses["collateral"],
                   "owner_address": addresses["owner"], "operator_publickey": operator,
                   "payout_address": addresses["payout"], "service": "[::1]:19999",
                   "operator_reward": 1000, "operator_payout_address": addresses["mining"]}
        controller.walletlock()
        assert_raises_rpc_error(-13, "walletpassphrase", controller.sendpqmasternode,
                                "register", options, str(root / "register.dat"))
        controller.walletpassphrase("controller-test", 600, True)
        assert_raises_rpc_error(-13, "walletpassphrase", controller.sendpqmasternode,
                                "register", options, str(root / "register.dat"))
        controller.walletpassphrase("controller-test", 600)
        assert_raises_rpc_error(-8, "Unknown", controller.sendpqmasternode,
                                "register", dict(options, typo=True), str(root / "register.dat"))
        for bad in [{}, [], "1", 1.5, -1, 10001]:
            assert_raises_rpc_error(-8, "operator reward", controller.sendpqmasternode,
                                    "register", dict(options, operator_reward=bad), str(root / "bad.dat"))
        for bad in [{}, [], "1", 1.5, -1, 4294967296]:
            assert_raises_rpc_error(-8, "collateral index", controller.sendpqmasternode,
                                    "register", dict(options, collateral_txid="12" * 32, collateral_vout=bad),
                                    str(root / "bad.dat"))
        assert_raises_rpc_error(-4, "operator identity", controller.sendpqmasternode,
                                "register", dict(options, operator_publickey="00" * 1312), str(root / "bad.dat"))
        assert_raises_rpc_error(-4, "backup", controller.sendpqmasternode,
                                "register", options, str(root / "missing" / "backup.dat"))
        assert_equal(controller.getrawmempool(), [])
        assert_equal(observer.listpqmasternodes(), [])
        existing_backup = (root / "operator.dat").read_bytes()
        assert_raises_rpc_error(-4, "backup", controller.sendpqmasternode,
                                "register", options, str(root / "operator.dat"))
        assert_equal((root / "operator.dat").read_bytes(), existing_backup)
        assert_equal(controller.getrawmempool(), [])
        sent = controller.sendpqmasternode("register", options, str(root / "register.dat"))
        registration = sent["txid"]
        assert sent["fee"] > 0
        assert_equal(controller.gettransaction(registration)["confirmations"], 0)
        self.sync_mempools()
        assert registration in observer.getrawmempool()
        assert_equal(observer.listpqmasternodes(), [])
        controller.generatetoaddress(1, addresses["mining"])
        self.sync_all()
        record = observer.listpqmasternodes()[0]
        assert_equal(record["registration"], registration)
        assert_equal(record["operator_publickey"], operator)
        assert_equal(record["payout_address"], addresses["payout"])
        assert_equal(record["sequence"], "0")
        assert_equal(record["revoked"], False)
        assert_equal(record["operator_reward"], 1000)
        assert_equal(record["operator_payout_address"], addresses["mining"])
        assert_equal(record["collateral_txid"], registration)
        assert_equal(record["collateral_vout"], 0)
        # Repeated registration cannot create a second bond for the same identity.
        assert_raises_rpc_error(-4, "bad-pqmn-duplicate-key", controller.sendpqmasternode,
                                "register", options, str(root / "duplicate.dat"))
        assert_equal(controller.getrawmempool(), [])
        controller.walletlock()
        assert_equal(controller.listpqmasternodes(), observer.listpqmasternodes())
        controller.walletpassphrase("controller-test", 600)
        for bad in ["", "0", "01", "-1", "1.0", "18446744073709551616", " 1", 1]:
            assert_raises_rpc_error(-8, "", controller.sendpqmasternode,
                                    "revoke", {"registration": registration, "sequence": bad}, str(root / "bad.dat"))

        def submit(action, fields, sequence):
            fields = dict(fields, registration=registration, sequence=str(sequence))
            result = controller.sendpqmasternode(action, fields, str(root / (action + str(sequence) + ".dat")))
            self.sync_mempools()
            assert result["txid"] in observer.getrawmempool()
            block = controller.generatetoaddress(1, addresses["mining"])[0]
            self.sync_all()
            assert_equal(observer.listpqmasternodes()[0]["sequence"], str(sequence))
            assert_raises_rpc_error(-4, "bad-pqmn-sequence", controller.sendpqmasternode,
                                    action, fields, str(root / "retry.dat"))
            return block

        submit("update", {"payout_address": addresses["owner"], "operator_publickey": operator}, 1)
        assert_equal(observer.listpqmasternodes()[0]["payout_address"], addresses["owner"])
        submit("service", {"service": "[::1]:20000"}, 2)
        assert_equal(observer.listpqmasternodes()[0]["service"], "[::1]:20000")
        assert_equal(observer.listpqmasternodes()[0]["operator_payout_address"], addresses["mining"])
        revoked_block = submit("revoke", {}, 3)
        assert_equal(observer.listpqmasternodes()[0]["revoked"], True)
        assert_equal(observer.listpqmasternodes()[0]["eligible"], False)
        # Reorg and restart must expose confirmed state, not a wallet-side cache.
        observer.invalidateblock(revoked_block)
        assert_equal(observer.listpqmasternodes()[0]["revoked"], False)
        observer.reconsiderblock(revoked_block)
        self.sync_blocks()
        self.restart_node(1)
        assert_equal(observer.listpqmasternodes()[0]["revoked"], True)
        self.connect_nodes(0, 1)
        replacement = controller.createpqoperator(str(root / "replacement.dat"))["publickey"]
        submit("update", {"payout_address": addresses["payout"], "operator_publickey": replacement}, 4)
        record = observer.listpqmasternodes()[0]
        assert_equal(record["revoked"], False)
        assert_equal(record["operator_publickey"], replacement)
        assert_equal(record["service"], "")
        assert_equal(record["eligible"], False)
        submit("service", {"service": "[::1]:20001", "operator_payout_address": addresses["mining"]}, 5)
        # Existing confirmed collateral is registered without being spent for fees.
        collateral2 = controller.getnewpqaddress(str(root / "collateral2.dat"))["address"]
        owner2 = controller.getnewpqaddress(str(root / "owner2.dat"))["address"]
        operator2 = controller.createpqoperator(str(root / "operator2.dat"))["publickey"]
        funded = controller.sendpqtoaddress(collateral2, 100, str(root / "bond-funding.dat"))["txid"]
        controller.generatetoaddress(1, addresses["mining"])
        self.sync_all()
        bond = next(coin for coin in controller.listpqunspent()
                    if coin["txid"] == funded and coin["address"] == collateral2)
        external = {"collateral_address": collateral2, "owner_address": owner2,
                    "operator_publickey": operator2, "payout_address": addresses["payout"],
                    "collateral_txid": funded, "collateral_vout": bond["vout"]}
        # Exercise the real command-line object's JSON conversion as well as RPC.
        registered2 = controller.cli.sendpqmasternode("register", json.dumps(external),
                                                     str(root / "external.dat"))["txid"]
        self.sync_mempools()
        controller.generatetoaddress(1, addresses["mining"])
        self.sync_all()
        records = observer.listpqmasternodes()
        assert_equal(len(records), 2)
        external_record = next(item for item in records if item["registration"] == registered2)
        assert_equal(external_record["collateral_txid"], funded)
        assert_equal(external_record["collateral_vout"], bond["vout"])
        assert_equal(external_record["operator_reward"], 0)
        assert_equal(external_record["operator_payout_address"], "")
        assert controller.gettxout(funded, bond["vout"]) is not None
        self.restart_node(0)
        assert_equal(controller.listpqmasternodes(), records)
        assert controller.gettransaction(registration)["confirmations"] > 0
        assert controller.gettransaction(registered2)["confirmations"] > 0
        assert controller.verifychain(0)
        assert observer.verifychain(0)

        # Explicit coin selection can retire a masternode by spending its bond.
        # Automatic selection above must not spend either registered collateral.
        self.connect_nodes(0, 1)
        controller.walletpassphrase("controller-test", 600)
        selected = [{"txid": funded, "vout": bond["vout"]}]
        for bad in [None, {}, [], "[]", selected * 2, selected * 3, [None], [{}],
                    [{"txid": funded}], [dict(selected[0], extra=True)],
                    [{"txid": "00" * 32, "vout": 0}], [{"txid": "bad", "vout": 0}]] + [
                    [{"txid": funded, "vout": value}] for value in [None, True, "0", -1, 1.5, 4294967296]]:
            assert_raises_rpc_error(-8, "", controller.sendpqtoaddress, addresses["mining"], 1,
                                    str(root / "invalid-selection.dat"), bad)
        assert_raises_rpc_error(-4, "selected coin", controller.sendpqtoaddress, addresses["mining"], 1,
                                str(root / "unavailable-selection.dat"), [{"txid": "12" * 32, "vout": 0}])
        assert_raises_rpc_error(-4, "Insufficient", controller.sendpqtoaddress, addresses["mining"], 101,
                                str(root / "insufficient-selection.dat"), selected)
        addresses_before = controller.listpqaddresses()
        assert_raises_rpc_error(-4, "backup", controller.sendpqtoaddress, addresses["mining"], 1,
                                str(root / "missing" / "spend-bond.dat"), selected)
        assert_equal(controller.listpqaddresses(), addresses_before)
        assert_equal(controller.getrawmempool(), [])
        assert controller.gettxout(funded, bond["vout"]) is not None
        spent = controller.cli.sendpqtoaddress(addresses["mining"], 1,
                    str(root / "spend-bond.dat"), json.dumps(selected))["txid"]
        assert (root / "spend-bond.dat").is_file()
        raw = controller.gettransaction(spent)["hex"]
        decoded = controller.decoderawtransaction(raw)
        assert_equal([(item["txid"], item["vout"]) for item in decoded["vin"]],
                     [(funded, bond["vout"])])
        self.sync_mempools()
        controller.generatetoaddress(1, addresses["mining"])
        self.sync_all()
        assert_equal([item["registration"] for item in observer.listpqmasternodes()], [registration])
        assert_equal(observer.gettxout(funded, bond["vout"]), None)
        both = [{"txid": spent, "vout": index} for index in range(2)]
        combined = controller.sendpqtoaddress(addresses["mining"], 1,
                    str(root / "two-selected.dat"), both)["txid"]
        inputs = controller.decoderawtransaction(controller.gettransaction(combined)["hex"])["vin"]
        assert_equal({(item["txid"], item["vout"]) for item in inputs}, {(spent, 0), (spent, 1)})
        assert_equal(len(inputs), 2)
        self.sync_mempools()
        controller.generatetoaddress(1, addresses["mining"])
        self.sync_all()
        self.restart_node(1)
        assert_equal(observer.listpqmasternodes(), controller.listpqmasternodes())
        assert observer.verifychain(0)


if __name__ == "__main__":
    PQMasternodeWalletTest().main()
