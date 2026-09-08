// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#include <pqservice.h>
#include <evo/pqmnauth.h>
#include <init.h>
#include <net.h>
#include <validation.h>
#include <utiltime.h>
#include <algorithm>
#include <chrono>
#include <map>

namespace pqservice {
namespace {
// One verified proof per identity, bounded by the supported committee size.
// No worker thread: existing peer send passes refresh and relay activity.
std::map<uint256, Heartbeat> pending;

void Prune(const CBlockIndex* parent)
{
    pqmn::Index index(*evoDb, Params());
    for (auto it = pending.begin(); it != pending.end();) {
        pqmn::Record record; std::string reason;
        if (!index.Get(it->first, record) || !CheckContext(it->second, record, parent, Params(), reason))
            it = pending.erase(it);
        else ++it;
    }
}

void Refresh()
{
    if (!evoDb || !Active(Params(), chainActive.Height() + 1)) { pending.clear(); return; }
    Prune(chainActive.Tip());
    const auto* local = GetPQOperator();
    if (!local || IsInitialBlockDownload()) return;
    pqmn::Index index(*evoDb, Params());
    pqmn::Record record;
    if (!index.Get(local->Registration(), record)) return;
    const uint32_t interval = std::max(1, Params().GetConsensus().nPQServiceWindow / 3);
    auto last = record.lastHeartbeatHeight;
    const auto existing = pending.find(local->Registration());
    if (existing != pending.end()) last = std::max(last, existing->second.height);
    if (last && uint32_t(chainActive.Height()) - last < interval) return;
    if (pending.size() >= pqquorum::MAX_MEMBERS && existing == pending.end()) return;
    Heartbeat heartbeat; std::string reason;
    if (local->SignHeartbeat(heartbeat, reason)) pending[heartbeat.registration] = heartbeat;
}
}

bool Receive(Span<const unsigned char> bytes)
{
    AssertLockHeld(cs_main);
    if (bytes.size() != HEARTBEAT_SIZE + 3) return false;
    Carrier carrier;
    if (!Decode(bytes, carrier) || !carrier.certificate.empty() || carrier.heartbeats.size() != 1) return false;
    if (!evoDb || !Active(Params(), chainActive.Height() + 1) || IsInitialBlockDownload()) return true;
    const auto& heartbeat = carrier.heartbeats[0];
    const auto existing = pending.find(heartbeat.registration);
    if (existing != pending.end() && existing->second.height >= heartbeat.height) return true;
    static auto window = std::chrono::steady_clock::now();
    static size_t used = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - window >= std::chrono::seconds(1)) { window = now; used = 0; }
    if (used >= MAX_HEARTBEATS) return true;
    ++used;
    Prune(chainActive.Tip());
    if (pending.size() >= pqquorum::MAX_MEMBERS && !pending.count(heartbeat.registration)) return true;
    pqmn::Index index(*evoDb, Params()); std::string reason;
    if (index.MatchesChainTip(chainActive.Tip()) && Verify(heartbeat, index, chainActive.Tip(), Params(), reason))
        pending[heartbeat.registration] = heartbeat;
    // In-flight proofs may normally become stale across blocks/reorgs/key updates.
    return true;
}

std::vector<Heartbeat> Pending(const CBlockIndex* parent)
{
    AssertLockHeld(cs_main);
    if (!evoDb || !parent || !Active(Params(), parent->nHeight + 1)) return {};
    Refresh();
    pqmn::Index index(*evoDb, Params());
    std::vector<std::pair<uint32_t, Heartbeat>> candidates;
    for (const auto& item : pending) {
        pqmn::Record record; std::string reason;
        if (index.Get(item.first, record) && CheckContext(item.second, record, parent, Params(), reason))
            candidates.emplace_back(record.lastHeartbeatHeight, item.second);
    }
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return std::make_pair(a.first, a.second.registration) < std::make_pair(b.first, b.second.registration);
    });
    std::vector<Heartbeat> result;
    for (const auto& item : candidates) {
        result.push_back(item.second);
        if (result.size() == MAX_HEARTBEATS) break;
    }
    return result;
}

void Send(CNode& peer, int64_t& nextRelay, uint256& cursor)
{
    AssertLockHeld(cs_main);
    const int64_t now = GetTimeMillis();
    if (now < nextRelay || !g_connman) return;
    nextRelay = now + 1000;
    Refresh();
    auto it = pending.upper_bound(cursor);
    if (it == pending.end()) it = pending.begin();
    for (size_t n = 0; it != pending.end() && n < MAX_HEARTBEATS; ++n, ++it) {
        CSerializedNetMsg message;
        message.command = NetMsgType::PQSERVICE;
        message.data = Encode({{}, {it->second}});
        cursor = it->first;
        g_connman->PushMessage(&peer, std::move(message));
    }
}
}
