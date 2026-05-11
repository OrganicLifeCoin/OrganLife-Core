#!/usr/bin/env python3
# Copyright (c) 2026 The PIVX Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""End-to-end RPC flow for governance coin-lock voting."""

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
import time


class GovernanceCoinLockTest(PivxTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-nuparams=v6_evo:250", "-nuparams=v6_coinlock_governance:251"]]

    def run_test(self):
        node = self.nodes[0]

        node.generate(400)
        proposal_hash = "01" * 32
        unlock_height = node.getblockcount() + 100

        # Unknown proposal hashes should be rejected.
        status_unknown = node.getgovvotestatus(proposal_hash)
        assert_equal(status_unknown["coin_yes"], 0)
        assert_equal(status_unknown["coin_no"], 0)
        assert_equal(status_unknown["combined_score"], 0)
        assert_raises_rpc_error(
            -8,
            "unknown proposal hash",
            node.creategovvotelock,
            proposal_hash,
            50,
            unlock_height,
        )
        assert_raises_rpc_error(
            -8,
            "unknown proposal hash",
            node.castgovvote,
            proposal_hash,
            "yes",
            [f"{'0' * 64}:0"],
        )

        # End-to-end with a real budget proposal: voting after proposal cutoff must be rejected.
        proposal_name = "coinlockexp01"
        proposal_url = "https://freedom.buzz/p/coinlockexp01"
        proposal_cycles = 1
        proposal_amount = 100
        proposal_start = node.getnextsuperblock()
        proposal_payee = node.getnewaddress()

        proposal_fee_txid = node.preparebudget(
            proposal_name,
            proposal_url,
            proposal_cycles,
            proposal_start,
            proposal_payee,
            proposal_amount,
        )
        node.generate(7)
        time.sleep(1)
        proposal_hash_real = node.submitbudget(
            proposal_name,
            proposal_url,
            proposal_cycles,
            proposal_start,
            proposal_payee,
            proposal_amount,
            proposal_fee_txid,
        )
        proposal_info = node.getbudgetinfo(proposal_name)[0]
        proposal_end = proposal_info["BlockEnd"]

        # Backward-compatible response: txid string
        pre_cutoff_lock_txid = node.creategovvotelock(proposal_hash_real, 5, proposal_end + 10)
        node.generate(1)
        pre_cutoff_lock = next(
            lock for lock in node.listgovlocks(proposal_hash_real) if lock["txid"] == pre_cutoff_lock_txid
        )

        # Verbose response: lock reference object for direct cast usage
        pre_cutoff_unused_lock = node.creategovvotelock(proposal_hash_real, 5, proposal_end + 10, True)
        pre_cutoff_cast = node.castgovvote(proposal_hash_real, "yes", [pre_cutoff_lock["outpoint"]])
        assert pre_cutoff_cast
        node.generate(1)
        pre_cutoff_status = node.getgovvotestatus(proposal_hash_real)
        assert_equal(pre_cutoff_status["coin_yes"], 5)

        current_height = node.getblockcount()
        if current_height <= proposal_end:
            node.generate((proposal_end - current_height) + 1)

        assert_raises_rpc_error(
            -8,
            "proposal voting window closed",
            node.creategovvotelock,
            proposal_hash_real,
            5,
            node.getblockcount() + 20,
            True,
        )
        assert_raises_rpc_error(
            -8,
            "proposal voting window closed",
            node.castgovvote,
            proposal_hash_real,
            "yes",
            [pre_cutoff_unused_lock["outpoint"]],
        )


if __name__ == '__main__':
    GovernanceCoinLockTest().main()
