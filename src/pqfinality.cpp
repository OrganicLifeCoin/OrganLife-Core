// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqfinality.h>
#include <chain.h>
#include <clientversion.h>
#include <evo/evodb.h>
#include <init.h>
#include <logging.h>
#include <netmessagemaker.h>
#include <protocol.h>
#include <streams.h>
#include <util/system.h>
#include <validation.h>
#include <algorithm>
#include <tuple>

namespace pqfinality {
namespace {
constexpr uint8_t PROPOSAL_VERSION = 1;
constexpr size_t PROPOSAL_HEADER_SIZE = 1 + 2 + 4 + 4 + 32 + 4;
constexpr size_t MAX_PROPOSAL_SIZE =
    PROPOSAL_HEADER_SIZE + pqquorum::MAX_CERTIFICATE_SIZE + mldsa44::SIGNATURE_SIZE;
} // namespace

std::vector<unsigned char> EncodeProposal(const Proposal& proposal)
{
    CDataStream stream(SER_NETWORK, 0);
    stream << PROPOSAL_VERSION << proposal.member << proposal.round << proposal.polcRound <<
        proposal.blockHash;
    const auto polc = pqquorum::Encode(proposal.polc);
    if (!proposal.polc.signatures.empty() && polc.empty()) return {};
    stream << static_cast<uint32_t>(polc.size());
    if (!polc.empty()) stream.write(reinterpret_cast<const char*>(polc.data()), polc.size());
    stream << proposal.signature;
    return {stream.begin(), stream.end()};
}

bool DecodeProposal(Span<const unsigned char> bytes, Proposal& proposal)
{
    proposal = {};
    if (bytes.size() < PROPOSAL_HEADER_SIZE + mldsa44::SIGNATURE_SIZE ||
        bytes.size() > MAX_PROPOSAL_SIZE || !bytes.data())
        return false;
    try {
        const char* begin = reinterpret_cast<const char*>(bytes.data());
        CDataStream stream(begin, begin + bytes.size(), SER_NETWORK, 0);
        uint8_t version;
        uint32_t polcSize;
        Proposal decoded;
        stream >> version >> decoded.member >> decoded.round >> decoded.polcRound >>
            decoded.blockHash >> polcSize;
        if (version != PROPOSAL_VERSION || polcSize > pqquorum::MAX_CERTIFICATE_SIZE ||
            stream.size() != size_t(polcSize) + mldsa44::SIGNATURE_SIZE) return false;
        if (polcSize) {
            std::vector<unsigned char> polc(polcSize);
            stream.read(reinterpret_cast<char*>(polc.data()), polc.size());
            if (!pqquorum::Decode(polc, decoded.polc)) return false;
        }
        stream >> decoded.signature;
        if (!stream.empty()) return false;
        proposal = std::move(decoded);
        return true;
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

namespace {
uint256 MessageKey(const std::string& command, const std::vector<unsigned char>& bytes)
{
    CHashWriter hash(SER_GETHASH, 0);
    hash << command << bytes;
    return hash.GetHash();
}
} // namespace

Manager& Manager::Get()
{
    static Manager manager;
    return manager;
}

void Manager::GetStatus(bool& configured, bool& validatedOut, uint32_t& anchorHeight, uint256& anchorHash,
                        uint32_t& committeeSize, uint32_t& votingHeight, uint32_t& votingRound,
                        bool& hasLock, uint256& lockedValue, bool& isMember) const
{
    configured = pqanchor::GetBootstrap() != nullptr;
    validatedOut = false;
    anchorHeight = 0;
    anchorHash.SetNull();
    committeeSize = 0;
    votingHeight = 0;
    votingRound = 0;
    hasLock = false;
    lockedValue.SetNull();
    isMember = false;
    LOCK2(cs_main, cs);
    validatedOut = started && validated;
    {
        LOCK(cs_main);
        if (auto* store = GetPQAnchorStore()) {
            pqanchor::Record durable;
            if (store->Tip(durable)) {
                anchorHeight = durable.height;
                anchorHash = durable.blockHash;
            }
        }
        if (!anchorHeight && evoDb) {
            pqanchor::ChainState state(*evoDb, Params());
            pqanchor::Record anchor;
            if (state.TipAnchor(anchor)) {
                anchorHeight = anchor.height;
                anchorHash = anchor.blockHash;
            }
        }
        if (anchorHeight && evoDb) {
            pqanchor::ChainState state(*evoDb, Params());
            std::vector<pqquorum::Member> committee;
            if (state.CommitteeAt(anchorHeight, committee)) {
                committeeSize = committee.size();
                if (localOperator) {
                    for (size_t i = 0; i < committee.size(); ++i)
                        if (committee[i].operator_key == localOperator->PublicKey()) isMember = true;
                }
            }
        }
    }
    if (!validatedOut) return;
    votingHeight = working.height ? working.height : anchorHeight + 1;
    votingRound = CurrentRound(working);
    if (journal) {
        uint32_t lockRound{0};
        hasLock = journal->GetLock(votingHeight, lockedValue, lockRound) && !lockedValue.IsNull();
    }
}

bool Manager::GetJournalProgress(uint32_t& finalized, uint32_t& votedHeight, uint32_t& votedRound) const
{
    LOCK(cs);
    finalized = votedHeight = votedRound = 0;
    return journal && journal->GetProgress(finalized, votedHeight, votedRound);
}

bool Manager::Start(std::string& reason)
{
    reason.clear();
    if (started) return true;
    if (!pqanchor::GetBootstrap()) return false; // finality inactive: not an error
    localOperator = GetPQOperator();
    if (localOperator) {
        journal = pqjournal::Journal::Load(GetDataDir(), Params().GetConsensus().hashGenesisBlock,
                                           localOperator->Registration(), localOperator->PublicKey(), reason);
        if (!journal) {
            reason = "pq-finality-journal: " + reason;
            return false;
        }
    }
    interrupt.reset();
    started = true;
    driver = std::thread(&TraceThread<std::function<void()>>, "pqfinality",
                         std::function<void()>(std::bind(&Manager::Run, this)));
    LogPrintf("pqfinality: runtime started%s\n", localOperator ? " (voting member)" : " (passive)");
    return true;
}

void Manager::Stop()
{
    if (!started) return;
    interrupt();
    wake.notify_all();
    if (driver.joinable()) driver.join();
    LOCK(cs);
    journal.reset();
    localOperator = nullptr;
    validated = false;
    nextCommitmentRelay = 0;
    inbox.clear();
    relayed.clear();
    started = false;
}

void Manager::OnProposal(CNode& peer, Span<const unsigned char> bytes)
{
    Proposal proposal;
    if (!DecodeProposal(bytes, proposal) || proposal.blockHash.IsNull()) {
        peer.fDisconnect = true;
        return;
    }
    {
        LOCK(cs_main);
        if (!chainActive.Tip()) return;
    }
    {
        LOCK(cs);
        if (inbox.size() >= MAX_INBOX) inbox.pop_front();
        InboxItem item;
        item.peer = &peer;
        item.member = proposal.member;
        item.round = proposal.round;
        item.step = pqquorum::Purpose::PREVOTE;
        item.proposal = std::make_unique<Proposal>(proposal);
        item.value = proposal.blockHash;
        inbox.push_back(std::move(item));
    }
    wake.notify_all();
}

void Manager::OnVote(CNode& peer, Span<const unsigned char> bytes)
{
    pqquorum::Certificate certificate;
    if (!pqquorum::Decode(bytes, certificate) || certificate.signatures.size() != 1) {
        peer.fDisconnect = true;
        return;
    }
    {
        LOCK(cs_main);
        const int tip = chainActive.Height();
        if (!chainActive.Tip() || certificate.statement.height == 0 ||
            certificate.statement.height > uint32_t(tip) + 1)
            return;
    }
    {
        LOCK(cs);
        if (inbox.size() >= MAX_INBOX) inbox.pop_front();
        InboxItem item;
        item.peer = &peer;
        item.member = certificate.signatures[0].member;
        item.round = certificate.statement.round;
        item.step = certificate.statement.purpose;
        item.value = certificate.statement.value;
        item.certificate = std::move(certificate);
        inbox.push_back(std::move(item));
    }
    wake.notify_all();
}

void Manager::OnCommitment(CNode& peer, Span<const unsigned char> bytes)
{
    pqquorum::Certificate certificate;
    if (!pqquorum::Decode(bytes, certificate)) {
        peer.fDisconnect = true;
        return;
    }
    {
        LOCK(cs_main);
        if (!chainActive.Tip()) return;
    }
    {
        LOCK(cs);
        if (inbox.size() >= MAX_INBOX) inbox.pop_front();
        InboxItem item;
        item.peer = &peer;
        item.member = certificate.signatures.empty() ? 0 : certificate.signatures[0].member;
        item.round = certificate.statement.round;
        item.step = pqquorum::Purpose::PRECOMMIT;
        item.value = certificate.statement.value;
        item.certificate = std::move(certificate);
        inbox.push_back(std::move(item));
    }
    wake.notify_all();
}

bool Manager::PendingCertificate(pqquorum::Certificate& out)
{
    AssertLockHeld(cs_main);
    if (!started) return false;
    pqanchor::ChainState state(*evoDb, Params());
    const auto* bootstrap = pqanchor::GetBootstrap();
    if (!bootstrap) return false;
    const uint32_t mirror = std::max(state.TipHeight(), bootstrap->height);
    // The durable store is also the publication queue. Reopening a node must
    // not lose a committed certificate that has not reached a carrier block.
    auto* store = GetPQAnchorStore();
    return store && store->ReadCertificate(mirror + 1, out);
}

void Manager::RelayToPeers(const std::string& command, const std::vector<unsigned char>& bytes) const
{
    if (!g_connman) return;
    g_connman->ForEachNode([&](CNode* peer) {
        if (!peer->fDisconnect) {
            CSerializedNetMsg message;
            message.command = command;
            message.data = bytes;
            g_connman->PushMessage(peer, std::move(message));
        }
    });
}

void Manager::BroadcastNovel(const std::string& command, const std::vector<unsigned char>& bytes,
                             const uint256& key)
{
    {
        LOCK(cs);
        if (relayed.count(key)) return;
        if (relayed.size() >= MAX_RELAYED_SET) relayed.clear();
        relayed.insert(key);
    }
    RelayToPeers(command, bytes);
}

bool Manager::VerifyVote(const pqquorum::Certificate& certificate, const Height& height,
                         std::string& reason) const
{
    pqquorum::Statement context;
    context.genesis = Params().GetConsensus().hashGenesisBlock;
    context.anchor = height.anchor;
    context.committee = pqquorum::Commitment(height.committee);
    context.height = height.height;
    return pqfinality::VerifyVote(certificate, context, height.committee, reason);
}

bool VerifyVote(const pqquorum::Certificate& certificate, const pqquorum::Statement& context,
                const std::vector<pqquorum::Member>& committee, std::string& reason)
{
    reason.clear();
    if (certificate.signatures.size() != 1) {
        reason = "bad-pq-vote-single-signature";
        return false;
    }
    const auto& statement = certificate.statement;
    if (statement.genesis != context.genesis || statement.anchor != context.anchor ||
        statement.committee != context.committee || statement.height != context.height ||
        (statement.purpose != pqquorum::Purpose::PREVOTE &&
         statement.purpose != pqquorum::Purpose::PRECOMMIT)) {
        reason = "bad-pq-vote-context";
        return false;
    }
    return pqquorum::VerifySignature(statement, certificate.signatures[0].member,
                                     certificate.signatures[0].bytes, committee, reason);
}

bool Manager::VerifyProposal(const Proposal& proposal, const Height& height, std::string& reason) const
{
    reason.clear();
    if (proposal.member >= height.committee.size()) {
        reason = "bad-pq-proposal-member";
        return false;
    }
    if ((uint64_t(height.height) + proposal.round) % height.committee.size() != proposal.member) {
        reason = "bad-pq-proposer-rotation";
        return false;
    }
    pqquorum::Statement expected;
    expected.purpose = pqquorum::Purpose::PREVOTE;
    expected.genesis = Params().GetConsensus().hashGenesisBlock;
    expected.anchor = height.anchor;
    expected.committee = pqquorum::Commitment(height.committee);
    expected.height = height.height;
    expected.round = proposal.round;
    expected.value = proposal.blockHash;
    const auto message = pqquorum::Message(expected);
    if (message.empty()) {
        reason = "bad-pq-proposal-statement";
        return false;
    }
    if (!mldsa44::Verify(height.committee[proposal.member].operator_key, message, pqquorum::Context(),
                         proposal.signature)) {
        reason = "bad-pq-proposal-signature";
        return false;
    }
    if (!proposal.polc.signatures.empty()) {
        if (proposal.polcRound != proposal.polc.statement.round || proposal.polcRound >= proposal.round) {
            reason = "bad-pq-proposal-polc-round";
            return false;
        }
        expected.round = proposal.polcRound;
        expected.value = proposal.polc.statement.value;
        if (!pqquorum::Verify(proposal.polc, expected, height.committee, reason)) return false;
    }
    return true;
}

bool Manager::AcceptProposal(Height& height, const Proposal& proposal, std::string& reason)
{
    const auto current = CurrentRound(height);
    if (proposal.round < current || !VerifyProposal(proposal, height, reason)) return false;
    if (proposal.round > current) {
        if (height.futureProposal && height.futureProposal->round <= proposal.round) return false;
        height.futureProposal = proposal;
        return true;
    }
    if (!proposal.polc.signatures.empty() && !RememberProof(height, proposal.polc, reason)) return false;
    return height.proposals.emplace(proposal.round, proposal).second;
}

bool Manager::RememberProof(Height& height, const pqquorum::Certificate& proof, std::string& reason) const
{
    // Called only after proposal verification or assembly of verified votes.
    const auto& old = height.bestPolc;
    if (!old.signatures.empty() && (proof.statement.round < old.statement.round ||
        (proof.statement.round == old.statement.round &&
         (proof.statement.value != old.statement.value || proof.signatures.size() <= old.signatures.size()))))
        return true;
    if (journal && !journal->RecordProof(proof, reason)) {
        LogPrintf("pqfinality: could not persist quorum proof: %s\n", reason);
        return false;
    }
    height.bestPolc = proof;
    return true;
}

bool Manager::RestoreProof(Height& height, std::string& reason) const
{
    pqquorum::Certificate proof;
    if (!journal || !journal->GetProof(height.height, proof)) return true;
    pqquorum::Statement expected;
    expected.purpose = pqquorum::Purpose::PREVOTE;
    expected.genesis = Params().GetConsensus().hashGenesisBlock;
    expected.anchor = height.anchor;
    expected.committee = pqquorum::Commitment(height.committee);
    expected.height = height.height;
    expected.round = proof.statement.round;
    expected.value = proof.statement.value;
    if (!pqquorum::Verify(proof, expected, height.committee, reason)) return false;
    height.bestPolc = std::move(proof);
    return true;
}

bool Manager::BuildCertificate(Height& height, uint32_t round, pqquorum::Purpose step,
                               const uint256& value, pqquorum::Certificate& out) const
{
    out = {};
    const size_t threshold = pqquorum::Threshold(height.committee.size());
    if (!threshold) return false;
    out.statement.purpose = step;
    out.statement.genesis = Params().GetConsensus().hashGenesisBlock;
    out.statement.anchor = height.anchor;
    out.statement.committee = pqquorum::Commitment(height.committee);
    out.statement.height = height.height;
    out.statement.round = round;
    out.statement.value = value;
    for (const auto& entry : height.votes) {
        if (std::get<0>(entry.first) != round || std::get<1>(entry.first) != step) continue;
        if (entry.second.first != value) continue;
        pqquorum::Signature signature;
        signature.member = std::get<2>(entry.first);
        signature.bytes = entry.second.second;
        out.signatures.push_back(signature);
    }
    // The map iterates ascending over (round, step, member), so the selected
    // signatures are already in canonical member order.
    const bool complete = out.signatures.size() >= threshold;
    std::string reason;
    if (complete && step == pqquorum::Purpose::PREVOTE && !RememberProof(height, out, reason)) return false;
    return complete;
}

bool Manager::StoreVote(Height& height, const pqquorum::Certificate& vote, bool persistProof)
{
    const auto& statement = vote.statement;
    const auto& signature = vote.signatures[0];
    const auto key = std::make_tuple(statement.round, statement.purpose, signature.member);
    const auto inserted = height.votes.emplace(key, std::make_pair(statement.value, signature.bytes));
    if (!inserted.second) return false; // first-seen vote wins, including equivocation
    if (persistProof && statement.purpose == pqquorum::Purpose::PREVOTE) {
        pqquorum::Certificate proof;
        BuildCertificate(height, statement.round, statement.purpose, statement.value, proof);
    }
    return true;
}

bool Manager::AcceptVote(Height& height, const pqquorum::Certificate& vote, int64_t now, std::string& reason,
                         bool persistProof)
{
    const uint32_t current = CurrentRound(height);
    const uint32_t round = vote.statement.round;
    if (round < current && uint64_t(current) - round >= ROUND_HISTORY) return false;
    if (!VerifyVote(vote, height, reason)) return false;
    if (round <= current) return StoreVote(height, vote, persistProof);

    const uint16_t member = vote.signatures[0].member;
    const auto previous = height.futureVotes.find(member);
    if (previous != height.futureVotes.end() && previous->second.statement.round >= round) return false;
    height.futureVotes[member] = vote;
    const size_t threshold = pqquorum::Threshold(height.committee.size());
    if (!threshold || height.futureVotes.size() < threshold) return true;
    std::vector<uint32_t> reports;
    for (const auto& entry : height.futureVotes) reports.push_back(entry.second.statement.round);
    std::sort(reports.begin(), reports.end(), std::greater<uint32_t>());
    // At least the normal quorum is at or beyond this round; one outlier
    // cannot choose it, and duplicate identities/steps never add support.
    AdvanceRound(height, reports[threshold - 1], now);
    return true;
}

bool JournalVote(pqquorum::RoundState& state, pqjournal::Journal& journal,
                 pqquorum::Purpose step, const uint256& value, const pqquorum::Certificate* proof,
                 const pqjournal::Journal::Signer& signer, pqquorum::Statement& decision,
                 pqjournal::Journal::Signature& signature, std::string& reason)
{
    decision = {}; signature = {};
    const uint256 oldLock = state.lockedValue();
    const uint32_t oldRound = state.lockedRound();
    pqquorum::Statement vote;
    if (step == pqquorum::Purpose::PREVOTE) {
        if (!state.Prevote(value, proof, vote, reason)) return false;
    } else if (step == pqquorum::Purpose::PRECOMMIT) {
        if (!state.Precommit(proof, value, vote, reason)) return false;
    } else {
        reason = "pq-finality-invalid-vote-step";
        return false;
    }
    // Detect a prior incomplete transition before using ephemeral state again.
    uint256 durableLock;
    uint32_t durableRound = 0;
    const bool hasDurableLock = journal.GetLock(vote.height, durableLock, durableRound);
    if ((hasDurableLock && (durableLock != oldLock || durableRound != oldRound)) ||
        (!hasDurableLock && (!oldLock.IsNull() || oldRound != 0))) {
        reason = "pq-finality-journal-lock-mismatch";
        return false;
    }
    if ((state.lockedValue() != oldLock || state.lockedRound() != oldRound) &&
        !journal.RecordLock(vote.height, state.lockedRound(), state.lockedValue(), reason)) return false;
    if (!journal.GetOrSignVote(vote, signer, signature, reason)) return false;
    decision = vote;
    return true;
}

bool Manager::JournalAndBroadcastVote(Height& height, pqquorum::Purpose step, const uint256& value,
                                      const pqquorum::Certificate* proof, std::string& reason)
{
    if (height.selfMember < 0 || !journal || !localOperator) {
        reason = "pq-finality-not-a-voter";
        return false;
    }
    pqjournal::Journal::Signature signature;
    pqquorum::Statement statement;
    const auto signer = [&](const pqquorum::Statement& voteStatement,
                            pqjournal::Journal::Signature& out) {
        return localOperator->SignVote(voteStatement, out, reason);
    };
    // Durable before broadcast; the journaled signature is reused on retries.
    if (!JournalVote(*height.round, *journal, step, value, proof, signer, statement, signature, reason)) return false;
    pqquorum::Certificate vote;
    vote.statement = statement;
    pqquorum::Signature single;
    single.member = uint16_t(height.selfMember);
    single.bytes = signature;
    vote.signatures.push_back(single);
    StoreVote(height, vote);
    const auto bytes = pqquorum::Encode(vote);
    if (bytes.empty()) {
        reason = "pq-finality-vote-encode-failed";
        return false;
    }
    BroadcastNovel(NetMsgType::PQVOTE, bytes, MessageKey(NetMsgType::PQVOTE, bytes));
    return true;
}

bool Manager::RecordCommitment(Height& height, const pqquorum::Certificate& cert, std::string& reason)
{
    AssertLockHeld(cs_main);
    // The mirror parent for this anchor is the current mirror tip, or the
    // pinned bootstrap before any certificate-bearing block has connected.
    pqanchor::ChainState state(*evoDb, Params());
    pqanchor::Record parent;
    uint256 parentCommittee;
    uint32_t parentHeight = 0;
    uint256 parentHash;
    if (state.TipAnchor(parent)) {
        // One finalized block delay: a commitment is only connectable when the
        // mirror parent is exactly one below; the committee handoff anchors in
        // finalized state.
        if (parent.height + 1 != height.height) {
            reason = "pq-finality-commit-mirror-lagging";
            return false;
        }
        parentHeight = parent.height;
        parentHash = parent.blockHash;
        parentCommittee = parent.committee;
    } else {
        const auto* bootstrap = pqanchor::GetBootstrap();
        if (!bootstrap || height.height != bootstrap->height + 1) {
            reason = "pq-finality-commit-no-parent";
            return false;
        }
        parentHeight = bootstrap->height;
        parentHash = bootstrap->blockHash;
        parentCommittee = pqquorum::Commitment(height.committee);
    }
    // The committed value must be the local active-chain block and the
    // statement must name exactly the mirror parent anchor.
    pqquorum::Statement expected;
    expected.purpose = pqquorum::Purpose::PRECOMMIT;
    expected.genesis = Params().GetConsensus().hashGenesisBlock;
    expected.anchor = pqanchor::ID(parentHeight, parentHash, parentCommittee);
    expected.committee = parentCommittee;
    expected.height = height.height;
    expected.round = cert.statement.round;
    expected.value = height.localBlock;
    if (!pqquorum::Verify(cert, expected, height.committee, reason)) return false;
    std::vector<pqquorum::Member> nextCommittee;
    if (!state.CommitteeAt(height.height, nextCommittee)) {
        reason = "pq-finality-commit-next-committee-unavailable";
        return false;
    }
    const uint256 nextCommitment = pqquorum::Commitment(nextCommittee);
    const uint256 anchorID = pqanchor::ID(height.height, height.localBlock, nextCommitment);
    // The referenced block must survive a crash before either signing history
    // or the irreversible store can rely on it. cs_main keeps ancestry stable.
    if (!FlushStateToDisk()) {
        reason = "pq-finality-chainstate-flush-failed";
        return false;
    }
    if (journal && !journal->RecordFinalized(height.height, height.localBlock, anchorID, reason)) {
        // Already finalized (duplicate/replay) is success for this height.
        uint32_t finalizedHeight{0};
        uint256 value, recordedAnchor;
        if (!(journal->GetFinalized(finalizedHeight, value, recordedAnchor) &&
              finalizedHeight == height.height && value == height.localBlock && recordedAnchor == anchorID))
            return false;
        reason.clear();
    }
    if (auto* store = GetPQAnchorStore()) {
        pqanchor::Record durable;
        durable.height = height.height;
        durable.blockHash = height.localBlock;
        durable.committee = nextCommitment;
        std::vector<uint256> signers;
        for (const auto& signature : cert.signatures)
            if (signature.member < height.committee.size())
                signers.push_back(height.committee[signature.member].registration);
        durable.signers = std::move(signers);
        if (!store->Write(durable, cert, reason)) return false;
    }
    const auto bytes = pqquorum::Encode(cert);
    if (!bytes.empty()) RelayToPeers(NetMsgType::PQCMT, bytes);
    height.committed = true;
    height.commitValue = height.localBlock;
    return true;
}

void Manager::Run()
{
    while (!interrupt) {
        int64_t sleepMs = 250;
        try {
            sleepMs = Process();
        } catch (const std::exception& e) {
            LogPrintf("pqfinality: driver error: %s\n", e.what());
        }
        if (!interrupt.sleep_for(std::chrono::milliseconds(std::max<int64_t>(sleepMs, 10)))) break;
    }
}

int64_t Manager::TimeoutFor(uint32_t round, int64_t base) const
{
    // base is in seconds; -pqfinalitytimeoutscale rescales the unit (1000 = 1s).
    const int64_t scale = std::clamp<int64_t>(gArgs.GetArg("-pqfinalitytimeoutscale", 1000), 1, 1000);
    return base * (int64_t(round) + 1) * scale;
}

void Manager::AdvanceRound(Height& height, uint32_t round, int64_t now)
{
    if (!height.round || !height.round->Advance(round)) return;
    const uint32_t first = round >= ROUND_HISTORY ? round - ROUND_HISTORY + 1 : 0;
    height.votes.erase(height.votes.begin(), height.votes.lower_bound({first, pqquorum::Purpose::PREVOTE, 0}));
    height.proposals.clear();
    if (height.futureProposal && height.futureProposal->round <= round) {
        auto proposal = std::move(*height.futureProposal);
        height.futureProposal.reset();
        std::string reason;
        AcceptProposal(height, proposal, reason); // Recheck and promote only on actual round entry.
    }
    for (auto it = height.futureVotes.begin(); it != height.futureVotes.end();) {
        if (it->second.statement.round > round) { ++it; continue; }
        if (it->second.statement.round >= first) StoreVote(height, it->second);
        it = height.futureVotes.erase(it);
    }
    height.proposed = false;
    height.deadlinePropose = now + TimeoutFor(round, PROPOSE_TIMEOUT_SECONDS);
    height.deadlinePrevote = height.deadlinePropose + TimeoutFor(round, VOTE_TIMEOUT_SECONDS);
    height.deadlinePrecommit = height.deadlinePrevote + TimeoutFor(round, VOTE_TIMEOUT_SECONDS);
    LogPrint(BCLog::PQ, "pqfinality: advanced to round %u at height %u\n", round, height.height);
}

void Manager::AcceptCommitment(Height& height, const pqquorum::Certificate& cert, int64_t /*now*/,
                               std::string& reason)
{
    AssertLockHeld(cs_main);
    // Only a gossiped commitment for the exact height this node is voting on,
    // finalizing the local block, is accepted. Other heights reach finality
    // through in-block certificates during ordinary sync. Conflicting values
    // are rejected and logged; the existing anchor is never overwritten.
    if (cert.statement.height != height.height || height.committed) return;
    if (cert.statement.value != height.localBlock) {
        LogPrintf("pqfinality: conflicting gossiped commitment at height %u\n", cert.statement.height);
        return;
    }
    const auto bytes = pqquorum::Encode(cert);
    const uint256 key = MessageKey(NetMsgType::PQCMT, bytes);
    {
        LOCK(cs);
        if (relayed.count(key)) return; // already processed and relayed
    }
    if (!RecordCommitment(height, cert, reason)) {
        LogPrint(BCLog::PQ, "pqfinality: gossiped commitment rejected at %u: %s\n",
                 cert.statement.height, reason);
        return;
    }
    BroadcastNovel(NetMsgType::PQCMT, bytes, key);
    LogPrint(BCLog::PQ, "pqfinality: accepted gossiped commitment for height %u\n", cert.statement.height);
}

void Manager::Propose(Height& height, std::string& reason)
{
    AssertLockHeld(cs_main);
    if (height.proposed) return;
    if (height.selfMember < 0 || !journal || !localOperator) return;
    const uint32_t round = CurrentRound(height);
    if ((uint64_t(height.height) + round) % height.committee.size() != uint32_t(height.selfMember)) return;
    Proposal proposal;
    proposal.round = round;
    proposal.member = uint16_t(height.selfMember);
    proposal.blockHash = height.localBlock;
    // A lock on a different value must be released by a newer PoLC before this
    // node may propose its own value; otherwise propose the locked value.
    const uint256 locked = height.round->lockedValue();
    const pqquorum::Certificate* polc = nullptr;
    if (!height.bestPolc.signatures.empty() && height.bestPolc.statement.round < round) {
        proposal.polc = height.bestPolc;
        proposal.polcRound = proposal.polc.statement.round;
        polc = &proposal.polc;
    }
    if (!locked.IsNull() && (!polc || polc->statement.round <= height.round->lockedRound() ||
                             polc->statement.value == locked)) proposal.blockHash = locked;
    // The proposal authorization is the proposer's journaled prevote.
    if (!JournalAndBroadcastVote(height, pqquorum::Purpose::PREVOTE, proposal.blockHash, polc, reason)) {
        LogPrint(BCLog::PQ, "pqfinality: proposal vote failed: %s\n", reason);
        return;
    }
    pqjournal::Journal::Signature signature;
    uint256 value;
    if (!journal->GetVote(height.height, round, pqquorum::Purpose::PREVOTE, value, signature)) {
        reason = "pq-finality-missing-journaled-proposal";
        return;
    }
    proposal.signature = signature;
    const auto bytes = EncodeProposal(proposal);
    if (bytes.empty()) {
        reason = "pq-finality-proposal-encode-failed";
        return;
    }
    RelayToPeers(NetMsgType::PQPROP, bytes);
    height.proposed = true;
    LogPrint(BCLog::PQ, "pqfinality: proposed %s at height %u round %u\n",
             proposal.blockHash.ToString(), height.height, round);
}

void Manager::Drive(Height& height, int64_t now, std::string& reason)
{
    AssertLockHeld(cs_main);
    if (height.committed || height.selfMember < 0) {
        // Passive nodes still drain commitments (above) but never vote.
        return;
    }
    const uint32_t round = CurrentRound(height);

    // Prevote: on a verified proposal (value constrained by local validation
    // and lock rules) or nil on timeout.
    const auto proposalIt = height.proposals.find(round);
    if (!height.round->hasPrevoted() && !height.round->hasPrecommitted()) {
        if (proposalIt != height.proposals.end()) {
            const Proposal& proposal = proposalIt->second;
            const pqquorum::Certificate* polc = proposal.polc.signatures.empty() ? nullptr : &proposal.polc;
            // Only a proposal equal to the locally validated block (or to our
            // existing lock) can be prevoted directly; anything else prevotes
            // locked-or-nil per the reviewed lock rules.
            const bool known = proposal.blockHash == height.localBlock || proposal.blockHash == height.round->lockedValue();
            if (!JournalAndBroadcastVote(height, pqquorum::Purpose::PREVOTE,
                    known ? proposal.blockHash : uint256(), known ? polc : nullptr, reason)) {
                LogPrint(BCLog::PQ, "pqfinality: prevote failed: %s\n", reason);
            }
        } else if (now >= height.deadlinePropose) {
            if (!JournalAndBroadcastVote(height, pqquorum::Purpose::PREVOTE, {}, nullptr, reason)) {
                LogPrint(BCLog::PQ, "pqfinality: nil prevote failed: %s\n", reason);
            }
        } else {
            Propose(height, reason);
        }
    }

    // Prevote quorum -> precommit. Only locally validated values precommit.
    if (height.round->hasPrevoted() && !height.round->hasPrecommitted()) {
        for (const uint256 value : {height.localBlock, uint256()}) {
            pqquorum::Certificate cert;
            if (!BuildCertificate(height, round, pqquorum::Purpose::PREVOTE, value, cert)) continue;
            if (!JournalAndBroadcastVote(height, pqquorum::Purpose::PRECOMMIT, value, &cert, reason)) {
                LogPrint(BCLog::PQ, "pqfinality: precommit broadcast failed: %s\n", reason);
            }
            break;
        }
    }

    // Precommit quorum -> commit (only for the locally validated block).
    if (height.round->hasPrecommitted() && !height.committed) {
        pqquorum::Certificate cert;
        if (BuildCertificate(height, round, pqquorum::Purpose::PRECOMMIT, height.localBlock, cert)) {
            uint256 finalized;
            if (height.round->Commit(cert, height.localBlock, finalized, reason) &&
                RecordCommitment(height, cert, reason)) {
                LogPrintf("pqfinality: finalized height %u value %s (round %u)\n",
                          height.height, height.localBlock.ToString(), round);
            } else {
                LogPrint(BCLog::PQ, "pqfinality: commit deferred: %s\n", reason);
            }
        }
    }

    // Timeouts: nil precommit, then round advance.
    if (now >= height.deadlinePrevote && !height.round->hasPrecommitted()) {
        if (!JournalAndBroadcastVote(height, pqquorum::Purpose::PRECOMMIT, {}, nullptr, reason)) {
            LogPrint(BCLog::PQ, "pqfinality: nil precommit failed: %s\n", reason);
        }
    }
    if (now >= height.deadlinePrecommit && !height.committed && round < UINT32_MAX) {
        AdvanceRound(height, round + 1, now);
    }
}

void Manager::DrainInbox(Height& height, int64_t now)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(cs);
    std::string reason;
    size_t signatures = 0;
    std::set<std::pair<uint32_t, uint256>> changedPrevotes;
    const auto persistPrevotes = [&] {
        for (const auto& entry : changedPrevotes) {
            pqquorum::Certificate proof;
            BuildCertificate(height, entry.first, pqquorum::Purpose::PREVOTE, entry.second, proof);
        }
        changedPrevotes.clear();
    };
    // ponytail: declared signature work, not wall time; benchmark Drive and
    // storage separately before moving verification off the chain lock.
    for (size_t count = 0; count < MAX_INBOX_MESSAGES_PER_PASS && !inbox.empty() && !height.committed; ++count) {
        const auto& next = inbox.front();
        const size_t cost = next.proposal ? 1 + next.proposal->polc.signatures.size() :
                            std::max<size_t>(1, next.certificate.signatures.size());
        if (cost > MAX_INBOX_SIGNATURES_PER_PASS - signatures) break;
        signatures += cost;
        auto item = std::move(inbox.front());
        inbox.pop_front();
        // A future vote quorum can advance/prune the retained window. Save
        // any collected proof before that transition discards its votes.
        if (!item.proposal && item.certificate.statement.round > CurrentRound(height))
            persistPrevotes();
        if (item.proposal) {
            AcceptProposal(height, *item.proposal, reason);
        } else if (item.certificate.signatures.size() > 1 &&
                   item.step == pqquorum::Purpose::PRECOMMIT) {
            AcceptCommitment(height, item.certificate, now, reason);
        } else if (AcceptVote(height, item.certificate, now, reason, false)) {
            const auto& statement = item.certificate.statement;
            if (statement.purpose == pqquorum::Purpose::PREVOTE)
                changedPrevotes.emplace(statement.round, statement.value);
            const auto bytes = pqquorum::Encode(item.certificate);
            BroadcastNovel(NetMsgType::PQVOTE, bytes, MessageKey(NetMsgType::PQVOTE, bytes));
        }
    }
    // Persist the complete proof once per touched statement, not once per
    // extra signer. No local voting decision runs before this batch returns.
    if (!height.committed) persistPrevotes();
}

int64_t Manager::Process()
{
    // Keep candidate selection and all decisions on one active-chain snapshot.
    // A reorg between selection and signing must not finalize the old branch.
    LOCK(cs_main);
    const int64_t now = GetTimeMillis();
    Height next;
    bool active = false;

    {
        std::string reason;
        pqanchor::ChainState chainState(*evoDb, Params());
        // 1. Lazy bootstrap validation; failure keeps finality inactive.
        const bool ok = pqanchor::ValidateBootstrap(chainState, chainActive.Tip(), reason);
        // 2. Seed the durable store with the pinned bootstrap anchor a0 once.
        auto* store = GetPQAnchorStore();
        if (ok && store && store->TipHeight() == 0) {
            const auto* bootstrap = pqanchor::GetBootstrap();
            pqanchor::Record a0;
            a0.height = bootstrap->height;
            a0.blockHash = bootstrap->blockHash;
            std::vector<pqquorum::Member> pinned;
            if (chainState.CommitteeAt(bootstrap->height, pinned)) a0.committee = pqquorum::Commitment(pinned);
            if (a0.committee.IsNull() || !FlushStateToDisk() || !store->Write(a0, pqquorum::Certificate{}, reason)) {
                LogPrintf("pqfinality: bootstrap anchor seeding failed: %s\n", reason);
                return 5000; // Never start voting from an unpersisted checkpoint.
            }
        }
        // 3. Commitment tip: the store carries locally committed and accepted
        // foreign anchors; the mirror may lag until a block publishes the cert.
        pqanchor::Record tipAnchor;
        bool hasAnchor = false;
        if (store && store->Tip(tipAnchor)) hasAnchor = true;
        else if (chainState.TipAnchor(tipAnchor)) hasAnchor = true;
        if (ok && hasAnchor && chainActive.Tip()) {
            const uint32_t votingHeight = tipAnchor.height + 1;
            const CBlockIndex* ancestor = chainActive.Tip()->GetAncestor(int(tipAnchor.height));
            if (!ancestor || ancestor->GetBlockHash() != tipAnchor.blockHash) {
                LogPrintf("pqfinality: active chain diverged from finalized anchor at %d\n", tipAnchor.height);
                LOCK(cs);
                validated = false; // stop voting; anchors remain protected
                return 5000;
            }
            std::vector<pqquorum::Member> committee;
            if (votingHeight <= uint32_t(chainActive.Height()) &&
                chainState.CommitteeAt(tipAnchor.height, committee)) {
                next.height = votingHeight;
                next.localBlock = chainActive[votingHeight]->GetBlockHash();
                next.committee = committee;
                next.anchorHeight = tipAnchor.height;
                next.anchorHash = tipAnchor.blockHash;
                next.anchor = pqanchor::ID(tipAnchor.height, tipAnchor.blockHash, tipAnchor.committee);
                if (localOperator) {
                    for (size_t i = 0; i < committee.size(); ++i)
                        if (committee[i].operator_key == localOperator->PublicKey())
                            next.selfMember = int16_t(i);
                }
                active = true;
            }
        }
        {
            LOCK(cs);
            if (ok != validated) {
                LogPrintf("pqfinality: bootstrap %s\n",
                          ok ? "validated; finality active" : "invalid; finality inactive");
                validated = ok;
            }
        }
    }

    // A peer can miss the original announcement while disconnected or while
    // catching up its chain. Reannounce only the next unpublished certificate,
    // recovered from disk, until a carrier advances the mirror. Never re-sign.
    if (validated && now >= nextCommitmentRelay) {
        nextCommitmentRelay = now + 15000;
        pqquorum::Certificate pending;
        if (PendingCertificate(pending))
            RelayToPeers(NetMsgType::PQCMT, pqquorum::Encode(pending));
    }

    if (!active) {
        LOCK(cs);
        working = Height{};
        return 1000;
    }

    {
        LOCK(cs);
        // Committee changes are detected by commitment (bounded hash compare);
        // vectors of Member are not directly comparable.
        const bool heightChanged = working.height != next.height || working.anchor != next.anchor ||
                                   pqquorum::Commitment(working.committee) != pqquorum::Commitment(next.committee);
        if (heightChanged) {
            working = std::move(next);
            // Rebuild the round state from journal-replayed decisions only; a
            // used key is never given fresh state.
            std::string restoreReason;
            if (!RestoreProof(working, restoreReason)) {
                LogPrintf("pqfinality: quorum proof restore failed: %s\n", restoreReason);
                working = Height{}; // Retry full restoration; never drive partially restored state.
                return 5000;
            }
            uint32_t prevoteRound = 0, precommitRound = 0;
            const bool hasPrevote =
                journal && journal->HighestVoted(working.height, pqquorum::Purpose::PREVOTE, prevoteRound);
            const bool hasPrecommit =
                journal && journal->HighestVoted(working.height, pqquorum::Purpose::PRECOMMIT, precommitRound);
            uint256 locked;
            uint32_t lockRound = 0;
            const bool hasLock = journal && journal->GetLock(working.height, locked, lockRound);
            const uint32_t round = std::max({hasPrevote ? prevoteRound : 0,
                hasPrecommit ? precommitRound : 0, hasLock ? lockRound : 0,
                working.bestPolc.signatures.empty() ? 0 : working.bestPolc.statement.round});
            std::optional<uint256> prevote, precommit;
            uint256 value;
            pqjournal::Journal::Signature signature;
            if (journal && journal->GetVote(working.height, round, pqquorum::Purpose::PREVOTE, value, signature)) {
                prevote = value;
                if (working.selfMember >= 0)
                    working.votes[{round, pqquorum::Purpose::PREVOTE, uint16_t(working.selfMember)}] = {value, signature};
            }
            if (journal && journal->GetVote(working.height, round, pqquorum::Purpose::PRECOMMIT, value, signature)) {
                precommit = value;
                if (working.selfMember >= 0)
                    working.votes[{round, pqquorum::Purpose::PRECOMMIT, uint16_t(working.selfMember)}] = {value, signature};
            }
            if (round > 0 || hasLock || hasPrevote || hasPrecommit) {
                working.round = pqquorum::RoundState::Restore(
                    Params().GetConsensus().hashGenesisBlock, working.anchor, working.height,
                    working.committee, round, locked, lockRound, prevote, precommit, restoreReason);
                if (!working.round) {
                    LogPrintf("pqfinality: journal restore failed for height %u: %s\n", working.height,
                              restoreReason);
                    working = Height{};
                    return 5000; // fail closed: never sign without restored state
                }
            } else {
                working.round = std::make_unique<pqquorum::RoundState>(
                    Params().GetConsensus().hashGenesisBlock, working.anchor, working.height,
                    working.committee);
            }
            working.proposed = false;
            working.committed = false;
            working.deadlinePropose = now + TimeoutFor(round, PROPOSE_TIMEOUT_SECONDS);
            working.deadlinePrevote = working.deadlinePropose + TimeoutFor(round, VOTE_TIMEOUT_SECONDS);
            working.deadlinePrecommit = working.deadlinePrevote + TimeoutFor(round, VOTE_TIMEOUT_SECONDS);
        } else {
            // Same immutable voting context, possibly a different unfinalized
            // branch. Refresh only the candidate: never clear votes or locks.
            working.localBlock = next.localBlock;
        }
    }

    {
        LOCK(cs);
        std::string reason;
        // 4. Verify queued peer messages on the same active-chain snapshot.
        DrainInbox(working, now);
        // 5. Round decisions and timeouts for the current round.
        Drive(working, now, reason);
    }

    {
        LOCK(cs);
        return NextWakeDelay(now);
    }
}

int64_t Manager::NextWakeDelay(int64_t now) const
{
    AssertLockHeld(cs);
    if (working.committed) return 50; // advance promptly on the next pass
    if (!inbox.empty()) return 10; // release cs_main before the next bounded batch
    if (working.selfMember < 0 || !working.round) return 500;
    // Completed steps must not keep waking the driver on expired deadlines.
    const int64_t nextDeadline = working.round->hasPrecommitted() ? working.deadlinePrecommit :
                                 working.round->hasPrevoted() ? working.deadlinePrevote : working.deadlinePropose;
    return std::max<int64_t>(10, std::min<int64_t>(nextDeadline - now, 500));
}
} // namespace pqfinality
