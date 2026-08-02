#!/usr/bin/env python3
# Copyright (c) 2026 The OrganicLife Coin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Exercise ForkGuard divergence/recovery transitions on a live multi-node split."""

from test_framework.test_framework import PivxTestFramework
from test_framework.util import assert_equal, wait_until


class ForkGuardRecoveryTest(PivxTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        common_args = [
            "-whitelist=127.0.0.1",
            "-forkguarddivergenceblocks=1",
            "-forkguardsustainedsamples=1",
            "-forkguardstablesamples=1",
            "-forkguardrecoverytimeoutsamples=50",
            "-forkguardminpeerevidence=1",
        ]
        self.extra_args = [common_args] * self.num_nodes

    def setup_network(self):
        self.setup_nodes()
        self.connect_nodes_clique(self.nodes)
        self.sync_all()

    def _disconnect_node_from_peers(self, node_idx, peers):
        for peer_idx in peers:
            self.disconnect_nodes(node_idx, peer_idx)
            self.disconnect_nodes(peer_idx, node_idx)

    def _connect_node_to_peers(self, node_idx, peers):
        for peer_idx in peers:
            self.connect_nodes(node_idx, peer_idx)

    def run_test(self):
        tested_node = self.nodes[0]
        peer_indices = [1, 2, 3]

        self.log.info("Priming chain state")
        tested_node.generate(10)
        self.sync_blocks()

        self.log.info("Partitioning node0 from the peer majority")
        self._disconnect_node_from_peers(0, peer_indices)
        tested_node.generate(40)

        self.log.info("Reconnecting node0 to peers")
        self._connect_node_to_peers(0, peer_indices)
        # Make peers announce fresh blocks so node0 has live peer height evidence.
        self.nodes[1].generate(1)
        wait_until(lambda: tested_node.getforkguardstatus()["sampled_peers"] >= 1, timeout=30)

        self.log.info("Triggering recovery mode while peers are behind")
        tested_node.generate(1)
        wait_until(
            lambda: (
                tested_node.getforkguardstatus()["mode"] == "RECOVERY"
                and tested_node.getforkguardstatus()["staking_paused"]
            ),
            timeout=30,
        )

        self.log.info("Dropping peer evidence to confirm recovery exits safely")
        self._disconnect_node_from_peers(0, peer_indices)
        tested_node.generate(1)
        wait_until(
            lambda: (
                tested_node.getforkguardstatus()["mode"] == "CAUTION"
                and not tested_node.getforkguardstatus()["staking_paused"]
            ),
            timeout=30,
        )

        self.log.info("Reconnecting and re-synchronizing before final ban checks")
        self._connect_node_to_peers(0, peer_indices)
        self.sync_blocks()

        self.log.info("Verifying no peer bans were introduced by fork-sensitive handling")
        for node in self.nodes:
            assert_equal(len(node.listbanned()), 0)


if __name__ == "__main__":
    ForkGuardRecoveryTest().main()
