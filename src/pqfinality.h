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
#include <optional>
#include <set>
#include <thread>

// Finality runtime: connects the RoundState decision logic and the durable
// journal to the daemon lifecycle and bounded P2P transport. The driver votes
// only for height h = anchor.height + 1 with the value of h in its own active
// chain, never waits on votes for PoS production, and advances on a verified
// precommit quorum. P2P handlers only bound-check and enqueue under the
// driver lock; all registry/chain reads and verification happen in the driver
// under cs_main. The journal's anti-equivocation gate is the only path to
// operator vote signing.
namespace pqfinality {
// Step timeouts grow linearly per round (default 15s/10s/10s, rescaled on
// regtest via -pqfinalitytimeoutscale=<ms per unit>).
constexpr int64_t PROPOSE_TIMEOUT_SECONDS = 15;
constexpr int64_t VOTE_TIMEOUT_SECONDS = 10;
constexpr uint32_t ROUND_HISTORY = 4; // retained rounds, not a round-number limit

// Shared bounded inbox; drop-oldest on exhaustion. Per-pass limits leave
// queued work for another chain-lock acquisition without skipping validation.
// These budget inbox dispatch only, not Drive's proof checks or storage time.
constexpr size_t MAX_INBOX = 64;
constexpr size_t MAX_INBOX_MESSAGES_PER_PASS = 16;
constexpr size_t MAX_INBOX_SIGNATURES_PER_PASS = pqquorum::MAX_MEMBERS + 1;
constexpr size_t MAX_RELAYED_SET = 4096;

struct Proposal {
    uint256 blockHash;
    uint32_t round{0};
    uint32_t polcRound{0};
    pqquorum::Certificate polc; // optional unlock proof; empty means none
    uint16_t member{0};         // proposer index inside the voting committee
    std::array<unsigned char, mldsa44::SIGNATURE_SIZE> signature{};
};

// Canonical, bounded proposal wire encoding.
std::vector<unsigned char> EncodeProposal(const Proposal& proposal);
bool DecodeProposal(Span<const unsigned char> bytes, Proposal& proposal);
// Check a single vote against the locally selected height and committee.
// Only round and value may vary within that immutable context.
bool VerifyVote(const pqquorum::Certificate& certificate, const pqquorum::Statement& context,
                const std::vector<pqquorum::Member>& committee, std::string& reason);
// Shared runtime signing gate: decide first, persist lock/unlock, then journal
// the signature. A failure never yields broadcastable output. Caller serializes
// state access; a refused signer consumes this in-memory step until next round
// or restart, but cannot erase a durable lock.
bool JournalVote(pqquorum::RoundState& state, pqjournal::Journal& journal,
                 pqquorum::Purpose step, const uint256& value, const pqquorum::Certificate* proof,
                 const pqjournal::Journal::Signer& signer, pqquorum::Statement& decision,
                 pqjournal::Journal::Signature& signature, std::string& reason);

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
    bool GetJournalProgress(uint32_t& finalized, uint32_t& votedHeight, uint32_t& votedRound) const;

    // Status snapshot for RPC reporting (thread safe).
    void GetStatus(bool& configured, bool& validated, uint32_t& anchorHeight, uint256& anchorHash,
                   uint32_t& committeeSize, uint32_t& votingHeight, uint32_t& votingRound,
                   bool& hasLock, uint256& lockedValue, bool& isMember) const;

    // P2P entry points. Caller holds cs_main; malformed input disconnects.
    void OnProposal(CNode& peer, Span<const unsigned char> bytes);
    void OnVote(CNode& peer, Span<const unsigned char> bytes);
    void OnCommitment(CNode& peer, Span<const unsigned char> bytes);

    // Certificate the next block should carry (finalizes mirrorTip + 1); false
    // when none is pending. Caller holds cs_main. Recovered from the durable
    // anchor store, including after restart.
    bool PendingCertificate(pqquorum::Certificate& out);

private:
    friend struct ManagerTestAccess;
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
        std::optional<Proposal> futureProposal; // one nearest verified future proposal, never round-change authority
        // One highest authenticated future report per identity, independent
        // of how large or how many round numbers that identity advertises.
        std::map<uint16_t, pqquorum::Certificate> futureVotes;
        pqquorum::Certificate bestPolc; // latest quorum proof survives history pruning
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
    bool AcceptProposal(Height& height, const Proposal& proposal, std::string& reason);
    bool RememberProof(Height& height, const pqquorum::Certificate& proof, std::string& reason) const;
    bool RestoreProof(Height& height, std::string& reason) const;
    // Assembles a canonical quorum certificate from collected votes + own vote.
    bool BuildCertificate(Height& height, uint32_t round, pqquorum::Purpose step,
                          const uint256& value, pqquorum::Certificate& out) const;
    bool AcceptVote(Height& height, const pqquorum::Certificate& vote, int64_t now, std::string& reason,
                    bool persistProof = true);
    // Called only for locally signed or already verified votes in the retained window.
    // Only inbox batching defers proof assembly; it persists before Drive.
    bool StoreVote(Height& height, const pqquorum::Certificate& vote, bool persistProof = true);
    // Signs and journals a vote through the journal gate, then broadcasts.
    bool JournalAndBroadcastVote(Height& height, pqquorum::Purpose step, const uint256& value,
                                 const pqquorum::Certificate* proof, std::string& reason);
    void BroadcastNovel(const std::string& command, const std::vector<unsigned char>& bytes,
                        const uint256& key);
    void RelayToPeers(const std::string& command, const std::vector<unsigned char>& bytes) const;
    // Records a locally validated commitment (store + journal + pending cert).
    bool RecordCommitment(Height& height, const pqquorum::Certificate& cert, std::string& reason);
    // Driver helpers (caller holds cs and cs_main). rCurrent = round state.
    static uint32_t CurrentRound(const Height& height) { return height.round ? height.round->currentRound() : 0; }
    void AdvanceRound(Height& height, uint32_t round, int64_t now);
    void AcceptCommitment(Height& height, const pqquorum::Certificate& cert, int64_t now, std::string& reason);
    void DrainInbox(Height& height, int64_t now);
    // Round decisions and timeouts for one driver pass.
    void Drive(Height& height, int64_t now, std::string& reason);
    int64_t NextWakeDelay(int64_t now) const;
    // Builds and broadcasts the local proposal for (height, round).
    void Propose(Height& height, std::string& reason);
    int64_t TimeoutFor(uint32_t round, int64_t base) const;

    mutable RecursiveMutex cs;
    CThreadInterrupt interrupt;
    std::thread driver;
    std::condition_variable wake;
    std::atomic<bool> started{false};
    bool validated{false}; // pinned bootstrap currently valid (rechecked lazily)
    int64_t nextCommitmentRelay{0}; // bounded retry of the durable, unpublished certificate
    std::unique_ptr<pqjournal::Journal> journal;
    const pqmnauth::LocalOperator* localOperator{nullptr};
    std::deque<InboxItem> inbox;
    Height working; // driver's single-height working state under cs
    // Recently broadcast keys (bounded) to suppress redundant relay.
    std::set<uint256> relayed;
};
} // namespace pqfinality
#endif
