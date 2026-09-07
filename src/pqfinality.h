// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQFINALITY_H
#define ORGANICLIFE_PQFINALITY_H

#include <chainparams.h>
#include <crypto/mldsa44.h>
#include <evo/pqmnauth.h>
#include <net.h>
#include <pqanchors.h>
#include <pqjournal.h>
#include <pqquorum.h>
#include <sync.h>
#include <threadsafety.h>
#include <threadinterrupt.h>
#include <uint256.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <set>
#include <thread>

// Finality runtime: connects the RoundState decision logic and the durable
// journal to the daemon lifecycle and bounded P2P transport. The driver votes
// only for height h = anchor.height + 1 with the value of h in its own active
// chain, never waits on votes for PoS production, and advances on a verified
// precommit quorum. P2P handlers only bound-check, dedup and enqueue under the
// driver lock; all registry/chain reads and verification happen in the driver
// under cs_main. The journal's anti-equivocation gate is the only path to
// operator vote signing.
namespace pqfinality {
// Step timeouts grow linearly per round (default 15s/10s/10s, rescaled on
// regtest via -pqfinalitytimeoutscale=<ms per unit>).
constexpr int64_t PROPOSE_TIMEOUT_SECONDS = 15;
constexpr int64_t VOTE_TIMEOUT_SECONDS = 10;
constexpr uint32_t MAX_ROUND = 100; // rounds retained per height

// Bounded inbox per message class; drop-oldest on exhaustion (never unbounded).
// This is the global verification budget: at most the inbox is verified per
// driver pass.
constexpr size_t MAX_INBOX = 64;
constexpr size_t MAX_RELAYED_SET = 4096;

struct Proposal {
    uint256 blockHash;
    uint32_t round{0};
    uint32_t polcRound{0};
    pqquorum::Certificate polc; // optional unlock proof; empty means none
    uint16_t member{0};         // proposer index inside the voting committee
    std::array<unsigned char, mldsa44::SIGNATURE_SIZE> signature{};
};

struct InboxItem {
    CNode* peer; // for per-peer disconnect on late protocol violations
    uint16_t member;
    uint32_t round;
    pqquorum::Purpose step;
    uint256 value; // empty for proposals (carried in Proposal)
    std::unique_ptr<Proposal> proposal;
    pqquorum::Certificate certificate; // vote or commitment payload
};

class Manager {
public:
    static Manager& Get();
    // Starts the driver when finality is configured (bootstrap present). A
    // corrupt journal fails closed with a reason; false never blocks startup
    // of ordinary PoS operation. Caller holds no locks; requires cs_main free.
    bool Start(std::string& reason);
    // Interrupts and joins the driver before credential/EvoDB teardown.
    void Stop();
    bool Started() const { return started; }

    // Status snapshot for RPC reporting (thread safe).
    void GetStatus(bool& configured, bool& validated, uint32_t& anchorHeight, uint256& anchorHash,
                   uint32_t& committeeSize, uint32_t& votingHeight, uint32_t& votingRound,
                   bool& hasLock, uint256& lockedValue, bool& isMember) const;

    // P2P entry points. Caller holds cs_main; malformed input disconnects.
    void OnProposal(CNode& peer, Span<const unsigned char> bytes);
    void OnVote(CNode& peer, Span<const unsigned char> bytes);
    void OnCommitment(CNode& peer, Span<const unsigned char> bytes);

    // Certificate the next block should carry (finalizes mirrorTip + 1); false
    // when none is pending. Caller holds cs_main. Non-const: prunes published
    // certificates.
    bool PendingCertificate(pqquorum::Certificate& out);

private:
    Manager() = default;
    void Run();
    // One driver step. Returns the suggested sleep in milliseconds.
    int64_t Process();

    // Driver-side working state for the single voting height.
    struct Height {
        uint32_t height{0};                 // voting height (anchor + 1)
        uint256 localBlock;                 // our active-chain block at height
        std::vector<pqquorum::Member> committee;
        uint256 anchor, anchorHash;
        uint32_t anchorHeight{0};
        int16_t selfMember{-1};             // our index in the committee; -1 = passive
        // Received, verified votes: (round, step, member) -> value + raw
        // signature bytes (the member index is the map key).
        std::map<std::tuple<uint32_t, pqquorum::Purpose, uint16_t>,
                 std::pair<uint256, pqjournal::Journal::Signature>>
            votes;
        std::map<uint32_t, Proposal> proposals; // verified proposals by round
        std::unique_ptr<pqquorum::RoundState> round;
        // Timing: per-phase deadlines (ms ticks) for the current round.
        int64_t deadlinePropose{0};
        int64_t deadlinePrevote{0};
        int64_t deadlinePrecommit{0};
        bool proposed{false};
        bool committed{false}; // decided for this height
        // Local commit: certificate to publish and the finalized value.
        pqquorum::Certificate commitCert;
        uint256 commitValue;
        uint256 commitAnchor;
    };

    // Verify a received vote against the committee; caller holds cs_main.
    bool VerifyVote(const pqquorum::Certificate& certificate, const Height& height,
                    std::string& reason) const;
    bool VerifyProposal(const Proposal& proposal, const Height& height, std::string& reason) const;
    // Assembles a canonical quorum certificate from collected votes + own vote.
    bool BuildCertificate(const Height& height, uint32_t round, pqquorum::Purpose step,
                          const uint256& value, pqquorum::Certificate& out) const;
    // Signs and journals a vote through the journal gate, then broadcasts.
    bool JournalAndBroadcastVote(Height& height, const pqquorum::Statement& statement,
                                 std::string& reason);
    void BroadcastNovel(const std::string& command, const std::vector<unsigned char>& bytes,
                        const uint256& key);
    void RelayToPeers(const std::string& command, const std::vector<unsigned char>& bytes) const;
    // Records a locally validated commitment (store + journal + pending cert).
    bool RecordCommitment(Height& height, const pqquorum::Certificate& cert, std::string& reason);
    // Driver helpers (caller holds cs and cs_main). rCurrent = round state.
    static uint32_t CurrentRound(const Height& height) { return height.round ? height.round->currentRound() : 0; }
    void AdvanceRound(Height& height, uint32_t round, int64_t now);
    void AcceptCommitment(Height& height, const pqquorum::Certificate& cert, int64_t now, std::string& reason);
    // Round decisions and timeouts for one driver pass.
    void Drive(Height& height, int64_t now, std::string& reason);
    // Builds and broadcasts the local proposal for (height, round).
    void Propose(Height& height, std::string& reason);
    int64_t TimeoutFor(uint32_t round, int64_t base) const;

    mutable RecursiveMutex cs;
    CThreadInterrupt interrupt;
    std::thread driver;
    std::condition_variable wake;
    std::atomic<bool> started{false};
    bool validated{false}; // pinned bootstrap currently valid (rechecked lazily)
    std::unique_ptr<pqjournal::Journal> journal;
    const pqmnauth::LocalOperator* localOperator{nullptr};
    std::deque<InboxItem> inbox;
    Height working; // driver's single-height working state under cs
    // Pending published commitments in height order; the assembler carries the
    // one matching the mirror tip + 1 (at most one certificate per block).
    std::deque<std::pair<uint32_t, pqquorum::Certificate>> pending;
    // Recently broadcast keys (bounded) to suppress redundant relay.
    std::set<uint256> relayed;
};
} // namespace pqfinality
#endif
