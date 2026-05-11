// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2015-2021 The PIVX Core developers
// Copyright (c) 2026 The CTEAM Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_NET_PROCESSING_H
#define PIVX_NET_PROCESSING_H

#include "consensus/validation.h"
#include "net.h"
#include "validationinterface.h"

extern RecursiveMutex cs_main; // !TODO: change mutex to cs_orphans

/** Default for -maxorphantx, maximum number of orphan transactions kept in memory */
static const unsigned int DEFAULT_MAX_ORPHAN_TRANSACTIONS = 25;
/** Expiration time for orphan transactions in seconds */
static const int64_t ORPHAN_TX_EXPIRE_TIME = 20 * 60;
/** Minimum time between orphan transactions expire time checks in seconds */
static const int64_t ORPHAN_TX_EXPIRE_INTERVAL = 5 * 60;
/** Default for -blockspamfilter, use header spam filter */
static const bool DEFAULT_BLOCK_SPAM_FILTER = true;
/** Default for -blockspamfiltermaxsize, maximum size of the list of indexes in the block spam filter */
static const unsigned int DEFAULT_BLOCK_SPAM_FILTER_MAX_SIZE = 200;
/** Default for -blockspamfiltermaxavg, maximum average size of an index occurrence in the block spam filter */
static const unsigned int DEFAULT_BLOCK_SPAM_FILTER_MAX_AVG = 50;

/** Average delay between trickled inventory transmissions in seconds.
 *  Blocks and whitelisted receivers bypass this, outbound peers get half this delay. */
static const unsigned int INVENTORY_BROADCAST_INTERVAL = 5;
/** Maximum number of inventory items to send per transmission.
 *  Limits the impact of low-fee transaction floods. */
static const unsigned int INVENTORY_BROADCAST_MAX = 7 * INVENTORY_BROADCAST_INTERVAL;

class PeerLogicValidation : public CValidationInterface, public NetEventsInterface {
private:
    CConnman* connman;

public:
    explicit PeerLogicValidation(CConnman* connman);
    ~PeerLogicValidation() = default;

    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex) override;
    void UpdatedBlockTip(const CBlockIndex *pindexNew, const CBlockIndex *pindexFork, bool fInitialDownload) override;
    void BlockChecked(const CBlock& block, const CValidationState& state) override;


    void InitializeNode(CNode* pnode) override;
    void FinalizeNode(NodeId nodeid, bool& fUpdateConnectionTime) override;
    /** Process protocol messages received from a given node */
    bool ProcessMessages(CNode* pfrom, std::atomic<bool>& interrupt) override;
    /**
    * Send queued protocol messages to be sent to a give node.
    *
    * @param[in]   pto             The node which we are sending messages to.
    * @param[in]   interrupt       Interrupt condition for processing threads
    * @return                      True if there is more work to be done
    */
    bool SendMessages(CNode* pto, std::atomic<bool>& interrupt) override EXCLUSIVE_LOCKS_REQUIRED(pto->cs_sendProcessing);
};

struct CNodeStateStats {
    int nMisbehavior;
    int nSyncHeight;
    int nCommonHeight;
    std::vector<int> vHeightInFlight;
    int nValidBlocksReceived;
    int nMessagesReceived;
    double fReliabilityScore;
    uint64_t m_addr_processed = 0;
    uint64_t m_addr_rate_limited = 0;
};

struct ForkGuardStatus {
    std::string mode;
    int local_height{-1};
    int peer_majority_height{-1};
    int divergence_blocks{0};
    int sampled_peers{0};
    int sustained_samples{0};
    int required_sustained_samples{0};
    bool staking_paused{false};
    int64_t last_mode_change{0};
    int64_t last_recovery_trigger{0};
    std::string last_reason;
};

/** Get statistics from node state */
bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats);
/** Get current ForkGuard status and diagnostics */
bool GetForkGuardStatus(ForkGuardStatus& status);
/** Increase a node's misbehavior score. */
void Misbehaving(NodeId nodeid, int howmuch, const std::string& message="") EXCLUSIVE_LOCKS_REQUIRED(cs_main);
/** Apply peer handling for invalid block results. */
void HandleInvalidBlockFromPeer(NodeId nodeid, const CValidationState& state, int nDoS, CConnman* connman) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
/** Staged warning for minor offenses (warns first, then bans after repeated issues) */
void WarningStaging(NodeId nodeid, int howmuch, const std::string& message="") EXCLUSIVE_LOCKS_REQUIRED(cs_main);
bool IsBanned(NodeId nodeid);

/** Update peer reliability score after receiving valid data */
void UpdatePeerReliability(NodeId nodeid, bool validData = false) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

/** Get peer reliability score (0.0 to 1.0) */
double GetPeerReliabilityScore(NodeId nodeid) EXCLUSIVE_LOCKS_REQUIRED(cs_main);


using SecondsDouble = std::chrono::duration<double, std::chrono::seconds::period>;
/**
 * Helper to count the seconds in any std::chrono::duration type
 */
inline double CountSecondsDouble(SecondsDouble t) { return t.count(); }

#endif // PIVX_NET_PROCESSING_H
