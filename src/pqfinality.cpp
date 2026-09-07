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
constexpr size_t PROPOSAL_HEADER_SIZE = 1 + 2 + 4 + 4 + 4 + 32 + 2;
constexpr size_t MAX_PROPOSAL_SIZE =
    PROPOSAL_HEADER_SIZE + pqquorum::MAX_CERTIFICATE_SIZE + mldsa44::SIGNATURE_SIZE;

std::vector<unsigned char> EncodeProposal(const Proposal& proposal)
{
    CDataStream stream(SER_NETWORK, 0);
    stream << PROPOSAL_VERSION << proposal.member << proposal.round << proposal.polcRound <<
        proposal.blockHash;
    const auto polc = pqquorum::Encode(proposal.polc);
    stream << static_cast<uint16_t>(polc.size());
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
        uint16_t polcSize;
        stream >> version >> proposal.member >> proposal.round >> proposal.polcRound >>
            proposal.blockHash >> polcSize;
        if (version != PROPOSAL_VERSION || polcSize > uint16_t(pqquorum::MAX_CERTIFICATE_SIZE)) return false;
        if (polcSize) {
            std::vector<unsigned char> polc(polcSize);
            stream.read(reinterpret_cast<char*>(polc.data()), polc.size());
            if (!pqquorum::Decode(polc, proposal.polc)) return false;
        }
        stream >> proposal.signature;
        return stream.empty();
    } catch (const std::ios_base::failure&) {
        return false;
    }
}

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
    committeeSize = 0;
    votingHeight = 0;
    votingRound = 0;
    hasLock = false;
    isMember = false;
    LOCK(cs);
    if (!started || !validated) return;
    validatedOut = true;
    {
        LOCK(cs_main);
        if (auto* store = GetPQAnchorStore()) {
            pqanchor::Record durable;
            if (store->Tip(durable)) {
                anchorHeight = durable.height;
                anchorHash = durable.blockHash;
            }
        }
        if (!anchorHeight) {
            pqanchor::ChainState state(*evoDb, Params());
            pqanchor::Record anchor;
            if (state.TipAnchor(anchor)) {
                anchorHeight = anchor.height;
                anchorHash = anchor.blockHash;
            }
        }
        if (anchorHeight) {
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
    votingHeight = anchorHeight + 1;
    uint32_t finalized{0};
    uint256 value, anchorID;
    if (journal && journal->GetFinalized(finalized, value, anchorID)) {
        votingHeight = std::max(votingHeight, finalized + 1);
        uint32_t round{0};
        if (journal->HighestVoted(votingHeight, pqquorum::Purpose::PREVOTE, round)) votingRound = round;
        if (journal->GetLock(votingHeight, lockedValue, round)) hasLock = true;
    }
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
    inbox.clear();
    pending.clear();
    relayed.clear();
    started = false;
}

void Manager::OnProposal(CNode& peer, Span<const unsigned char> bytes)
{
    Proposal proposal;
    if (!DecodeProposal(bytes, proposal) || proposal.round >= MAX_ROUND || proposal.blockHash.IsNull()) {
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
            certificate.statement.height > uint32_t(tip) + 1 || certificate.statement.round >= MAX_ROUND)
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
    const uint32_t mirror = state.TipHeight();
    LOCK(cs);
    while (!pending.empty() && pending.front().first <= mirror) pending.pop_front();
    if (pending.empty() || pending.front().first != mirror + 1) return false;
    out = pending.front().second;
    return true;
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
    pqquorum::Statement expected = certificate.statement;
    expected.anchor = height.anchor;
    expected.committee = pqquorum::Commitment(height.committee);
    return pqquorum::Verify(certificate, expected, height.committee, reason);
}

bool Manager::VerifyProposal(const Proposal& proposal, const Height& height, std::string& reason) const
{
    reason.clear();
    if (proposal.member >= height.committee.size() || proposal.round >= MAX_ROUND) {
        reason = "bad-pq-proposal-member";
        return false;
    }
    if ((height.height + proposal.round) % height.committee.size() != proposal.member) {
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
    return true;
}

bool Manager::BuildCertificate(const Height& height, uint32_t round, pqquorum::Purpose step,
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
        if (out.signatures.size() >= threshold) break;
        pqquorum::Signature signature;
        signature.member = std::get<2>(entry.first);
        signature.bytes = entry.second.second;
        out.signatures.push_back(signature);
    }
    // The map iterates ascending over (round, step, member), so the selected
    // signatures are already in canonical member order.
    return out.signatures.size() >= threshold;
}

bool Manager::JournalAndBroadcastVote(Height& height, const pqquorum::Statement& statement,
                                      std::string& reason)
{
    if (height.selfMember < 0 || !journal || !localOperator) {
        reason = "pq-finality-not-a-voter";
        return false;
    }
    pqjournal::Journal::Signature signature;
    const auto signer = [&](const pqquorum::Statement& voteStatement,
                            pqjournal::Journal::Signature& out) {
        return localOperator->SignVote(voteStatement, out, reason);
    };
    // Durable before broadcast; the journaled signature is reused on retries.
    if (!journal->GetOrSignVote(statement, signer, signature, reason)) return false;
    height.votes[std::make_tuple(statement.round, statement.purpose, uint16_t(height.selfMember))] =
        std::make_pair(statement.value, signature);
    pqquorum::Certificate vote;
    vote.statement = statement;
    pqquorum::Signature single;
    single.member = uint16_t(height.selfMember);
    single.bytes = signature;
    vote.signatures.push_back(single);
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
    const uint256 anchorID = pqanchor::ID(height.height, height.localBlock, parentCommittee);
    if (journal && !journal->RecordFinalized(height.height, height.localBlock, anchorID, reason)) {
        // Already finalized (duplicate/replay) is success for this height.
        uint32_t finalizedHeight{0};
        uint256 value, recordedAnchor;
        if (!(journal->GetFinalized(finalizedHeight, value, recordedAnchor) &&
              finalizedHeight >= height.height && value == height.localBlock))
            return false;
        reason.clear();
    }
    if (auto* store = GetPQAnchorStore()) {
        pqanchor::Record durable;
        durable.height = height.height;
        durable.blockHash = height.localBlock;
        durable.committee = parentCommittee;
        std::vector<uint256> signers;
        for (const auto& signature : cert.signatures)
            if (signature.member < height.committee.size())
                signers.push_back(height.committee[signature.member].registration);
        durable.signers = std::move(signers);
        if (!store->Write(durable, cert, reason)) return false;
    }
    {
        LOCK(cs);
        pending.emplace_back(height.height, cert);
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
        if (interrupt.sleep_for(std::chrono::milliseconds(std::max<int64_t>(sleepMs, 10)))) break;
    }
}

int64_t Manager::TimeoutFor(uint32_t round, int64_t base) const
{
    const int64_t scale = std::max<int64_t>(gArgs.GetArg("-pqfinalitytimeoutscale", 1000), 1);
    return base * (int64_t(round) + 1) * scale / 1000;
}

void Manager::AdvanceRound(Height& height, uint32_t round, int64_t now)
{
    if (!height.round || round <= height.round->currentRound() || round >= MAX_ROUND) return;
    height.round->Advance(round);
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
    if ((height.height + round) % height.committee.size() != uint32_t(height.selfMember)) return;
    Proposal proposal;
    proposal.round = round;
    proposal.member = uint16_t(height.selfMember);
    proposal.blockHash = height.localBlock;
    // A lock on a different value must be released by a newer PoLC before this
    // node may propose its own value; otherwise propose the locked value.
    const uint256 locked = height.round->lockedValue();
    const pqquorum::Certificate* polc = nullptr;
    if (!locked.IsNull() && locked != height.localBlock) {
        // A collected prevote quorum for our value in a later round releases
        // the lock; one for the locked value never does.
        for (uint32_t candidate = height.round->lockedRound() + 1; candidate < round; ++candidate) {
            pqquorum::Certificate evidence;
            if (BuildCertificate(height, candidate, pqquorum::Purpose::PREVOTE, height.localBlock, evidence)) {
                proposal.polc = evidence;
                proposal.polcRound = candidate;
                polc = &proposal.polc;
                break;
            }
        }
        if (!polc) proposal.blockHash = locked;
    }
    pqquorum::Statement statement;
    statement.purpose = pqquorum::Purpose::PREVOTE;
    statement.genesis = Params().GetConsensus().hashGenesisBlock;
    statement.anchor = height.anchor;
    statement.committee = pqquorum::Commitment(height.committee);
    statement.height = height.height;
    statement.round = round;
    statement.value = proposal.blockHash;
    // The proposal authorization is the proposer's journaled prevote.
    if (!JournalAndBroadcastVote(height, statement, reason)) {
        LogPrint(BCLog::PQ, "pqfinality: proposal vote failed: %s\n", reason);
        return;
    }
    pqquorum::Statement decision;
    height.round->Prevote(proposal.blockHash, polc, decision, reason);
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
    const auto prevoteValue = [&](const uint256& value) {
        pqquorum::Statement vote;
        vote.purpose = pqquorum::Purpose::PREVOTE;
        vote.genesis = Params().GetConsensus().hashGenesisBlock;
        vote.anchor = height.anchor;
        vote.committee = pqquorum::Commitment(height.committee);
        vote.height = height.height;
        vote.round = round;
        vote.value = value;
        return vote;
    };

    // Prevote: on a verified proposal (value constrained by local validation
    // and lock rules) or nil on timeout.
    const auto proposalIt = height.proposals.find(round);
    if (!height.round->hasPrevoted() && !height.round->hasPrecommitted()) {
        if (proposalIt != height.proposals.end()) {
            const Proposal& proposal = proposalIt->second;
            pqquorum::Statement decision;
            const pqquorum::Certificate* polc = proposal.polc.signatures.empty() ? nullptr : &proposal.polc;
            // Only a proposal equal to the locally validated block (or to our
            // existing lock) can be prevoted directly; anything else prevotes
            // locked-or-nil per the reviewed lock rules.
            if (proposal.blockHash == height.localBlock || proposal.blockHash == height.round->lockedValue()) {
                height.round->Prevote(proposal.blockHash, polc, decision, reason);
            } else {
                height.round->Prevote(uint256(), nullptr, decision, reason);
            }
            if (!JournalAndBroadcastVote(height, prevoteValue(decision.value), reason)) {
                LogPrint(BCLog::PQ, "pqfinality: prevote failed: %s\n", reason);
            }
        } else if (now >= height.deadlinePropose) {
            pqquorum::Statement decision;
            height.round->Prevote(uint256(), nullptr, decision, reason); // locked-or-nil
            if (!JournalAndBroadcastVote(height, prevoteValue(decision.value), reason)) {
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
            pqquorum::Statement decision;
            if (!height.round->Precommit(&cert, value, decision, reason)) {
                LogPrint(BCLog::PQ, "pqfinality: precommit refused: %s\n", reason);
                continue;
            }
            // Lock changes are journaled before the precommit that relies on
            // them; a nil precommit changes no lock.
            if (!decision.value.IsNull() &&
                !journal->RecordLock(height.height, round, decision.value, reason)) {
                LogPrintf("pqfinality: lock journal failed at %u: %s\n", height.height, reason);
                return;
            }
            if (!JournalAndBroadcastVote(height, decision, reason)) {
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
        pqquorum::Statement decision;
        if (height.round->Precommit(nullptr, uint256(), decision, reason) &&
            !JournalAndBroadcastVote(height, decision, reason)) {
            LogPrint(BCLog::PQ, "pqfinality: nil precommit failed: %s\n", reason);
        }
    }
    if (now >= height.deadlinePrecommit && !height.committed) {
        AdvanceRound(height, round + 1, now);
    }
}

int64_t Manager::Process()
{
    const int64_t now = GetTimeMillis();
    Height next;
    bool active = false;

    {
        LOCK(cs_main);
        std::string reason;
        pqanchor::ChainState chainState(*evoDb, Params());
        // 1. Lazy bootstrap validation; failure keeps finality inactive.
        const bool ok = pqanchor::ValidateBootstrap(chainState, pqmn::Index(*evoDb, Params()),
                                                    chainActive.Tip(), reason);
        // 2. Seed the durable store with the pinned bootstrap anchor a0 once.
        auto* store = GetPQAnchorStore();
        if (ok && store && store->TipHeight() == 0) {
            const auto* bootstrap = pqanchor::GetBootstrap();
            pqanchor::Record a0;
            a0.height = bootstrap->height;
            a0.blockHash = bootstrap->blockHash;
            std::vector<pqquorum::Member> pinned;
            if (chainState.CommitteeAt(bootstrap->height, pinned)) a0.committee = pqquorum::Commitment(pinned);
            if (a0.committee.IsNull() || !store->Write(a0, pqquorum::Certificate{}, reason))
                LogPrintf("pqfinality: bootstrap anchor seeding failed: %s\n", reason);
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
            uint32_t prevoteRound = 0, precommitRound = 0;
            const bool hasPrevote =
                journal && journal->HighestVoted(working.height, pqquorum::Purpose::PREVOTE, prevoteRound);
            const bool hasPrecommit =
                journal && journal->HighestVoted(working.height, pqquorum::Purpose::PRECOMMIT, precommitRound);
            const uint32_t round = std::max(hasPrevote ? prevoteRound : 0, hasPrecommit ? precommitRound : 0);
            uint256 locked;
            uint32_t lockRound = 0;
            const bool hasLock = journal && journal->GetLock(working.height, locked, lockRound);
            uint256 prevote, precommit;
            pqjournal::Journal::Signature prevoteSig, precommitSig;
            if (hasPrevote)
                journal->GetVote(working.height, round, pqquorum::Purpose::PREVOTE, prevote, prevoteSig);
            if (hasPrecommit)
                journal->GetVote(working.height, round, pqquorum::Purpose::PRECOMMIT, precommit, precommitSig);
            if (round > 0 || hasLock || hasPrevote || hasPrecommit) {
                working.round = pqquorum::RoundState::Restore(
                    Params().GetConsensus().hashGenesisBlock, working.anchor, working.height,
                    working.committee, round, locked, lockRound, prevote, precommit, restoreReason);
                if (!working.round) {
                    LogPrintf("pqfinality: journal restore failed for height %u: %s\n", working.height,
                              restoreReason);
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
        }
    }

    {
        LOCK(cs_main);
        LOCK(cs);
        std::string reason;
        // 4. Drain the bounded inbox; all verification happens here under
        // cs_main. The inbox size bounds verification work per pass.
        std::deque<InboxItem> items;
        items.swap(inbox);
        for (auto& item : items) {
            if (working.committed) continue;
            if (item.proposal) {
                const uint32_t current = CurrentRound(working);
                if (item.round < current) continue; // stale proposal
                if (!VerifyProposal(*item.proposal, working, reason)) {
                    LogPrint(BCLog::PQ, "pqfinality: bad proposal: %s\n", reason);
                    continue;
                }
                if (item.round > current) AdvanceRound(working, item.round, now);
                working.proposals[item.round] = *item.proposal;
            } else if (item.certificate.signatures.size() > 1 &&
                       item.step == pqquorum::Purpose::PRECOMMIT) {
                AcceptCommitment(working, item.certificate, now, reason);
            } else {
                const uint32_t current = CurrentRound(working);
                if (item.round < current) continue; // stale vote
                if (!VerifyVote(item.certificate, working, reason)) {
                    LogPrint(BCLog::PQ, "pqfinality: bad vote: %s\n", reason);
                    continue;
                }
                if (item.round > current) AdvanceRound(working, item.round, now);
                const auto key = std::make_tuple(item.round, item.step, item.member);
                const auto existing = working.votes.find(key);
                if (existing == working.votes.end()) {
                    working.votes[key] = std::make_pair(item.value, item.certificate.signatures[0].bytes);
                } else if (existing->second.first != item.value) {
                    // Equivocation cannot break safety below quorum: first seen
                    // wins, conflicting votes are dropped and logged.
                    LogPrintf("pqfinality: conflicting vote from member %u at height %u round %u\n",
                              item.member, working.height, item.round);
                }
            }
        }
        // 5. Round decisions and timeouts for the current round.
        Drive(working, now, reason);
    }

    {
        LOCK(cs);
        if (working.committed) return 50; // advance promptly on the next pass
        const int64_t nextDeadline = std::min(std::min(working.deadlinePropose, working.deadlinePrevote),
                                              working.deadlinePrecommit);
        return std::max<int64_t>(10, std::min<int64_t>(nextDeadline - now, 500));
    }
}
} // namespace pqfinality

