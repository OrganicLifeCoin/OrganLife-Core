// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.

#include <wallet/wallet.h>
#include <coincontrol.h>
#include <evo/governancevoteindex.h>
#include <evo/pqmasternode.h>
#include <key_io.h>
#include <policy/policy.h>
#include <pqtransaction.h>
#include <random.h>
#include <script/sign.h>
#include <validation.h>
#include <wallet/fees.h>
#include <algorithm>

bool CWallet::PreparePQOperator(const fs::path& backup, mldsa44::PublicKey& public_key, std::string& reason)
{
    public_key = {}; reason.clear();
    LOCK2(cs_wallet, cs_KeyStore);
    const auto fail = [&](const char* message) { reason = message; return false; };
    const auto& params = Params();
    const int first = params.GetConsensus().vUpgrades[Consensus::UPGRADE_PQ_MASTERNODES].nActivationHeight;
    if (!pq::MasternodesActive(params, first)) return fail("PQ operator recovery requires opt-in regtest masternodes");
    if (!IsCrypted() || IsLocked() || fWalletUnlockStaking)
        return fail("PQ operator recovery requires an encrypted, fully unlocked wallet");
    const auto& genesis = params.GetConsensus().hashGenesisBlock;
    auto pending = std::find_if(m_pq_operator_recovery.begin(), m_pq_operator_recovery.end(),
                               [](const auto& entry) { return !entry.second.backed; });
    if (pending == m_pq_operator_recovery.end()) {
        pqwallet::SecureBytes seed(mldsa44::SEED_SIZE);
        GetStrongRandBytes(seed.data(), seed.size());
        pqwallet::OperatorRecovery recovery;
        if (!pqwallet::EncryptOperatorRecovery(seed, vMasterKey, params.NetworkIDString(), genesis, recovery.record))
            return fail("Could not encrypt PQ operator recovery key");
        const auto id = pq::GetID(recovery.record.public_key, params.NetworkIDString());
        if (!id || m_pq_keys.count(*id) || m_pq_operator_recovery.count(*id) ||
            !WalletBatch(*database).WritePQOperatorRecovery(genesis, *id, recovery))
            return fail("Could not persist pending PQ operator recovery key");
        pending = m_pq_operator_recovery.emplace(*id, recovery).first;
    }
    // Preserve the pending encrypted seed on failure/crash. No listing or return
    // path publishes it; the next request retries it before generating another.
    if (!BackupWallet(backup.string(), true)) return fail("PQ operator recovery backup failed; retry with a new backup file");
    auto backed = pending->second;
    backed.backed = 1;
    if (!WalletBatch(*database).WritePQOperatorRecovery(genesis, pending->first, backed, true))
        return fail("Could not persist PQ operator recovery backup state; retry with a new backup file");
    pending->second = backed;
    public_key = backed.record.public_key;
    return true;
}

std::vector<mldsa44::PublicKey> CWallet::GetPQOperators() const
{
    LOCK(cs_KeyStore);
    std::vector<mldsa44::PublicKey> result;
    if (!Params().IsRegTestNet()) return result;
    for (const auto& entry : m_pq_operator_recovery)
        if (entry.second.backed) result.push_back(entry.second.record.public_key);
    return result;
}

bool CWallet::LoadPQOperatorRecovery(const uint256& genesis, const pq::KeyID& id,
                                    const pqwallet::OperatorRecovery& recovery)
{
    LOCK(cs_KeyStore);
    const auto expected = pq::GetID(recovery.record.public_key, Params().NetworkIDString());
    if (!Params().IsRegTestNet() || genesis != Params().GetConsensus().hashGenesisBlock ||
        recovery.record.version != 3 || recovery.backed > 1 || !expected || *expected != id ||
        m_pq_keys.count(id) || m_pq_operator_recovery.count(id) || !SetCrypted()) return false;
    m_pq_operator_recovery.emplace(id, recovery);
    return true;
}

bool CWallet::GeneratePQAddress(std::string& address)
{
    address.clear();
    LOCK2(cs_wallet, cs_KeyStore);
    if (!Params().IsTestChain() || !IsCrypted() || IsLocked() || fWalletUnlockStaking) return false;
    pqwallet::SecureBytes seed(mldsa44::SEED_SIZE);
    GetStrongRandBytes(seed.data(), seed.size());
    pqwallet::Record record;
    if (!pqwallet::EncryptSeed(seed, vMasterKey, Params().NetworkIDString(), record)) return false;
    const auto id = pq::GetID(record.public_key, Params().NetworkIDString());
    if (!id || m_pq_keys.count(*id) || m_pq_operator_recovery.count(*id) || !WalletBatch(*database).WritePQKey(*id, record)) return false;
    m_pq_keys.emplace(*id, record);
    address = pq::EncodeAddress(*id, Params().NetworkIDString());
    return true;
}

bool CWallet::ErasePQAddress(const std::string& address)
{
    LOCK2(cs_wallet, cs_KeyStore);
    pq::KeyID id;
    if (!pq::DecodeAddress(address, Params().NetworkIDString(), id) || !m_pq_keys.count(id) ||
        !WalletBatch(*database).ErasePQKey(id)) return false;
    m_pq_keys.erase(id);
    return true;
}

std::vector<std::string> CWallet::GetPQAddresses() const
{
    LOCK(cs_KeyStore);
    std::vector<std::string> addresses;
    if (!Params().IsTestChain()) return addresses;
    for (const auto& entry : m_pq_keys) addresses.push_back(pq::EncodeAddress(entry.first, Params().NetworkIDString()));
    return addresses;
}

bool CWallet::GetPQKey(const std::string& address, mldsa44::Key& key) const
{
    key.Clear();
    pq::KeyID id;
    if (!pq::DecodeAddress(address, Params().NetworkIDString(), id)) return false;
    return GetPQKey(id, key, false);
}

bool CWallet::GetPQKey(const pq::KeyID& id, mldsa44::Key& key, bool staking) const
{
    key.Clear();
    LOCK2(cs_wallet, cs_KeyStore);
    if (!Params().IsTestChain() || !IsCrypted() || IsLocked() || (!staking && fWalletUnlockStaking)) return false;
    const auto entry = m_pq_keys.find(id);
    return entry != m_pq_keys.end() && pqwallet::DecryptKey(vMasterKey, entry->second, Params().NetworkIDString(), key);
}

bool CWallet::LoadPQKey(const pq::KeyID& id, const pqwallet::Record& record)
{
    LOCK(cs_KeyStore);
    const auto expected_id = pq::GetID(record.public_key, Params().NetworkIDString());
    if (!Params().IsTestChain() || record.version != 1 || !expected_id || id != *expected_id ||
        m_pq_keys.count(id) || m_pq_operator_recovery.count(id) || !SetCrypted()) return false;
    m_pq_keys.emplace(id, record);
    return true;
}

bool CWallet::PQPaymentsActive()
{
    LOCK(cs_main);
    return pq::PaymentsActive(Params(), chainActive.Height() + 1);
}

bool CWallet::IsPQMine(const CTxOut& output) const
{
    LOCK(cs_KeyStore);
    pq::KeyID id;
    return Params().IsTestChain() && pq::ExtractID(output.scriptPubKey, id) && m_pq_keys.count(id);
}

bool CWallet::InvolvesPQ(const CTransaction& tx) const
{
    LOCK(cs_wallet);
    for (const auto& output : tx.vout) if (IsPQMine(output)) return true;
    for (const auto& input : tx.vin) {
        const auto previous = mapWallet.find(input.prevout.hash);
        if (previous != mapWallet.end() && input.prevout.n < previous->second.tx->vout.size() &&
            IsPQMine(previous->second.tx->vout[input.prevout.n])) return true;
    }
    return false;
}

bool CWallet::IsPQCollateral(const COutPoint& outpoint) const
{
    AssertLockHeld(cs_main);
    if (!pq::MasternodesActive(Params(), chainActive.Height() + 1)) return false;
    if (mempool.IsPQMNCollateral(outpoint)) return true;
    if (!evoDb) throw std::runtime_error("PQ masternode registry is unavailable");
    uint256 registration;
    return pqmn::Index(*evoDb, Params()).FindCollateral(outpoint, registration);
}

std::vector<COutput> CWallet::GetPQUnspent(bool include_locked, const CCoinControl* coin_control) const
{
    LOCK2(cs_main, cs_wallet);
    std::vector<COutput> coins;
    if (!PQPaymentsActive()) return coins;
    // ponytail: scan the existing wallet map; add an index only if measured wallet size warrants it.
    for (const auto& item : mapWallet) {
        const auto& wtx = item.second;
        const auto* block = LookupBlockIndex(wtx.m_confirm.hashBlock);
        if (!wtx.isConfirmed() || !block || !chainActive.Contains(block) || !CheckFinalTx(wtx.tx)) continue;
        const int depth = wtx.GetDepthInMainChain();
        if (depth < 1 || wtx.GetBlocksToMaturity() > 0) continue;
        for (size_t i = 0; i < wtx.tx->vout.size(); ++i) {
            const auto& output = wtx.tx->vout[i];
            const COutPoint outpoint(item.first, i);
            const auto& chain_coin = pcoinsTip->AccessCoin(outpoint);
            CGovVoteLockRecord governance_lock;
            const bool governance_locked = governanceVoteIndex &&
                    governanceVoteIndex->GetLockRecord(outpoint, governance_lock) &&
                    chainActive.Height() + 1 < static_cast<int>(governance_lock.unlockHeight);
            if (IsPQMine(output) && output.nValue > 0 && Params().GetConsensus().MoneyRange(output.nValue) && !IsSpent(outpoint) &&
                (include_locked || (!IsLockedCoin(outpoint.hash, outpoint.n) &&
                    (!IsPQCollateral(outpoint) || (coin_control && coin_control->IsSelected(outpoint))))) && !governance_locked &&
                !chain_coin.IsSpent() && chain_coin.out == output && !mempool.isSpent(outpoint))
                coins.emplace_back(&wtx, i, depth, false, false, false);
        }
    }
    return coins;
}

bool CWallet::CreatePQTransaction(const std::string& address, CAmount amount,
                                  CTransactionRef& tx, CAmount& fee, std::string& reason,
                                  const CCoinControl* coin_control, bool subtract_fee)
{
    pq::KeyID recipient_id;
    if (!pq::DecodeAddress(address, Params().NetworkIDString(), recipient_id)) {
        tx.reset();
        fee = 0;
        reason = "Invalid PQ address for this network";
        return false;
    }
    return CreatePQTransaction({CTxOut(amount, pq::GetScript(recipient_id))}, pq::TRANSFER, {},
                               tx, fee, reason, coin_control, subtract_fee);
}

bool CWallet::CreatePQMasternodeTransaction(const pqmn::Payload& operation, const mldsa44::Key* operator_key,
                                          CTransactionRef& tx, CAmount& fee, std::string& reason)
{
    tx.reset(); fee = 0; reason.clear();
    LOCK2(cs_main, cs_wallet);
    if (!pq::MasternodesActive(Params(), chainActive.Height() + 1) || !evoDb) {
        reason = "PQ masternodes are not active on this network";
        return false;
    }
    pqmn::Payload op;
    if (!pqmn::Decode(pqmn::Encode(operation), op)) {
        reason = "Invalid PQ masternode operation";
        return false;
    }
    op.ownerSignature = {}; op.operatorSignature = {}; op.collateralSignature = {};
    std::vector<CTxOut> outputs;
    if (op.action == pqmn::Action::REGISTER && op.collateral.hash.IsNull()) {
        const auto id = pq::GetID(op.collateralKey, Params().NetworkIDString());
        if (op.collateral.n != 0 || !id) {
            reason = "Internal PQ collateral must use output zero";
            return false;
        }
        outputs.emplace_back(Params().GetConsensus().nMNCollateralAmt, pq::GetScript(*id));
    }
    try {
        CTransactionRef prepared;
        pqmn::Record checked;
        if (CreatePQTransactionInternal(outputs, pq::MASTERNODE, pqmn::Encode(op), prepared, fee, reason,
                                        nullptr, false, &op, operator_key) &&
            pqmn::Index(*evoDb, Params()).Check(*prepared, *pcoinsTip, chainActive.Height() + 1, checked, reason)) {
            tx = std::move(prepared);
            return true;
        }
    } catch (const std::exception&) {
        reason = "PQ masternode wallet or registry is unavailable";
    }
    fee = 0;
    return false;
}

bool CWallet::CreatePQTransaction(const std::vector<CTxOut>& outputs, uint8_t mode,
                                  const std::vector<unsigned char>& data,
                                  CTransactionRef& tx, CAmount& fee, std::string& reason,
                                  const CCoinControl* coin_control, bool subtract_fee)
{
    return CreatePQTransactionInternal(outputs, mode, data, tx, fee, reason, coin_control, subtract_fee, nullptr, nullptr);
}

bool CWallet::CreatePQTransactionInternal(const std::vector<CTxOut>& outputs, uint8_t mode,
    const std::vector<unsigned char>& data, CTransactionRef& tx, CAmount& fee, std::string& reason,
    const CCoinControl* coin_control, bool subtract_fee,
    const pqmn::Payload* operation, const mldsa44::Key* operator_key)
{
    tx.reset();
    fee = 0;
    reason.clear();
    LOCK2(cs_main, cs_wallet);
    LOCK(cs_KeyStore);
    const auto fail = [&](const char* message) { reason = message; return false; };
    const auto& consensus = Params().GetConsensus();
    if (!PQPaymentsActive()) return fail("PQ payments are not active on this network");
    if (!IsCrypted() || IsLocked() || fWalletUnlockStaking)
        return fail("PQ payments require an encrypted, fully unlocked wallet");
    const bool masternode = mode == pq::MASTERNODE && operation;
    if (masternode ? (data.empty() || outputs.size() > 1) :
        ((mode != pq::TRANSFER && !pq::IsGovernanceMode(mode)) ||
         (pq::IsGovernanceMode(mode) != !data.empty()) || outputs.size() != 1))
        return fail("Invalid PQ transaction mode or outputs");
    if (subtract_fee && mode != pq::TRANSFER) return fail("Fee subtraction is only available for payments");
    CAmount amount = 0;
    for (const CTxOut& output : outputs) {
        pq::KeyID id;
        if (!pq::ExtractID(output.scriptPubKey, id) || output.nValue <= 0 ||
            !consensus.MoneyRange(output.nValue) ||
            output.nValue > consensus.nMaxMoneyOut - amount || IsDust(output, dustRelayFee))
            return fail("Invalid PQ output");
        amount += output.nValue;
    }

    CMutableTransaction tx_new;
    tx_new.nVersion = CTransaction::SAPLING;
    tx_new.nType = CTransaction::PQ;
    tx_new.sapData = nullopt;
    pq::Payload payload;
    payload.mode = mode;
    payload.data = data;
    pq::KeyID governance_change_id{};
    std::vector<COutput> coins = GetPQUnspent(false, mode == pq::TRANSFER ? coin_control : nullptr);
    const auto selected = [&](const COutput& out) {
        return coin_control && coin_control->IsSelected(COutPoint(out.tx->GetHash(), out.i));
    };
    const size_t required_inputs = coin_control ? coin_control->QuantitySelected() : 0;
    if (required_inputs > pq::MAX_INPUTS) return fail("Select at most two coins for this transaction");
    if (static_cast<size_t>(std::count_if(coins.begin(), coins.end(), selected)) != required_inputs)
        return fail("A selected coin is spent, locked, immature or no longer confirmed");
    if (required_inputs && !coin_control->fAllowOtherInputs) {
        coins.erase(std::remove_if(coins.begin(), coins.end(), [&](const COutput& out) { return !selected(out); }), coins.end());
    }
    std::sort(coins.begin(), coins.end(), [&](const COutput& a, const COutput& b) {
        if (selected(a) != selected(b)) return selected(a);
        return a.Value() > b.Value();
    });
    pq::KeyID custom_change_id{};
    const bool custom_change = coin_control && !coin_control->destPQChange.empty();
    if (custom_change && (!pq::DecodeAddress(coin_control->destPQChange, Params().NetworkIDString(), custom_change_id) || mode != pq::TRANSFER))
        return fail("Invalid PQ change address for this network");
    const auto required_fee = [&](unsigned int bytes) {
        CAmount result = GetMinimumFee(bytes, nTxConfirmTarget, mempool);
        if (coin_control) {
            if (coin_control->fOverrideFeeRate) result = std::max(GetRequiredFee(bytes), coin_control->nFeeRate.GetFee(bytes));
            result = std::max(result, coin_control->nMinimumTotalFee);
        }
        return result;
    };
    CAmount selected_value = 0;
    bool built = false;
    reason = "Insufficient eligible confirmed funds";
    for (const auto& candidate : coins) {
        const COutPoint outpoint(candidate.tx->GetHash(), candidate.i);
        // An external registration's bond is not indexed yet, but cannot fund its own fee.
        if (masternode && operation->action == pqmn::Action::REGISTER && outpoint == operation->collateral) continue;
        if (IsLockedCoin(outpoint.hash, outpoint.n) || IsSpent(outpoint) || mempool.isSpent(outpoint)) continue;
        const auto& chain_coin = pcoinsTip->AccessCoin(outpoint);
        if (chain_coin.IsSpent() || chain_coin.out != candidate.tx->tx->vout[candidate.i]) continue;
        if (candidate.Value() <= 0 || !consensus.MoneyRange(candidate.Value()) ||
            candidate.Value() > consensus.nMaxMoneyOut - selected_value) return fail("PQ input amount out of range");
        selected_value += candidate.Value();
        tx_new.vin.emplace_back(outpoint);
        tx_new.vout = outputs;
        pq::KeyID id;
        if (!pq::ExtractID(chain_coin.out.scriptPubKey, id)) return fail("Invalid owned PQ output");
        if (tx_new.vin.size() == 1) governance_change_id = id;
        payload.authorizations.emplace_back();
        payload.authorizations.back().public_key = m_pq_keys.at(id).public_key;
        tx_new.extraPayload = pq::EncodePayload(payload);
        const CAmount minimum_fee = required_fee(GetSerializeSize(tx_new, PROTOCOL_VERSION));
        if (tx_new.vin.size() >= required_inputs && selected_value >= amount &&
            (subtract_fee || selected_value - amount >= minimum_fee)) {
            const auto change_script = pq::GetScript(pq::KeyID{});
            tx_new.vout.emplace_back(0, change_script);
            fee = required_fee(GetSerializeSize(tx_new, PROTOCOL_VERSION));
            if (fee > maxTxFee) return fail("PQ transaction fee is outside wallet policy");
            const CAmount change = selected_value - amount - (subtract_fee ? 0 : fee);
            if (subtract_fee && change > 0 && IsDust(CTxOut(change, change_script), dustRelayFee))
                return fail("Fee subtraction would leave dust change; adjust the amount or selected coins");
            if (change > 0 && !IsDust(CTxOut(change, change_script), dustRelayFee)) {
                pq::KeyID change_id = custom_change ? custom_change_id : governance_change_id;
                if (!custom_change && !pq::IsGovernanceMode(mode) && !masternode) {
                    std::string change_address;
                    if (!GeneratePQAddress(change_address) ||
                        !pq::DecodeAddress(change_address, Params().NetworkIDString(), change_id))
                        return fail("Could not persist PQ change key; transaction was not broadcast");
                }
                tx_new.vout.back() = CTxOut(change, pq::GetScript(change_id));
            } else {
                tx_new.vout.pop_back();
                fee = subtract_fee ? required_fee(GetSerializeSize(tx_new, PROTOCOL_VERSION)) : selected_value - amount;
            }
            if (subtract_fee) {
                if (amount <= fee) return fail("The amount does not cover the transaction fee");
                tx_new.vout.front().nValue = amount - fee;
                if (IsDust(tx_new.vout.front(), dustRelayFee)) return fail("The amount after subtracting the fee is too small");
            }
            built = true;
            break;
        }
        if (tx_new.vin.size() == pq::MAX_INPUTS) break;
    }
    if (!built) return false;

    // Re-read complete spent outputs from the trusted chain view before signing.
    std::vector<CTxOut> prevouts;
    CAmount input_value = 0;
    for (const auto& input : tx_new.vin) {
        const auto& coin = pcoinsTip->AccessCoin(input.prevout);
        if (coin.IsSpent() || IsSpent(input.prevout) || IsLockedCoin(input.prevout.hash, input.prevout.n) ||
            mempool.isSpent(input.prevout) || coin.nHeight > chainActive.Height()) return fail("PQ input is no longer available");
        if (!consensus.MoneyRange(coin.out.nValue) || coin.out.nValue > consensus.nMaxMoneyOut - input_value)
            return fail("PQ input amount out of range");
        input_value += coin.out.nValue;
        prevouts.push_back(coin.out);
    }
    if (masternode) {
        pqmn::Payload op = *operation;
        pqmn::Record original;
        if (op.action != pqmn::Action::REGISTER && !pqmn::Index(*evoDb, Params()).Get(op.registration, original))
            return fail("Unknown PQ masternode registration");
        const auto message = pqmn::SigningMessage(tx_new, prevouts, consensus.hashGenesisBlock);
        const auto sign = [&](const mldsa44::Key& key, pqmn::Role role, pqmn::Signature& output) {
            std::vector<unsigned char> signature;
            if (message.empty() || !key.Sign(message, pqmn::Context(role), signature) || signature.size() != output.size()) return false;
            std::copy(signature.begin(), signature.end(), output.begin());
            return true;
        };
        const auto wallet_sign = [&](const mldsa44::PublicKey& public_key, pqmn::Role role, pqmn::Signature& output) {
            const auto id = pq::GetID(public_key, Params().NetworkIDString());
            mldsa44::Key key;
            return id && GetPQKey(*id, key, false) && key.GetPublicKey() == public_key && sign(key, role, output);
        };
        if ((op.action == pqmn::Action::REGISTER || op.action == pqmn::Action::UPDATE) &&
            !wallet_sign(op.action == pqmn::Action::REGISTER ? op.owner : original.owner, pqmn::Role::OWNER, op.ownerSignature))
            return fail("PQ masternode owner key is unavailable in this wallet");
        if (op.action == pqmn::Action::REGISTER && !wallet_sign(op.collateralKey, pqmn::Role::COLLATERAL, op.collateralSignature))
            return fail("PQ masternode collateral key is unavailable in this wallet");
        if (op.action != pqmn::Action::UPDATE || op.operatorKey != original.operatorKey) {
            const auto& expected = (op.action == pqmn::Action::REGISTER || op.action == pqmn::Action::UPDATE) ? op.operatorKey : original.operatorKey;
            if (!operator_key || operator_key->GetPublicKey() != expected || !sign(*operator_key, pqmn::Role::OPERATOR, op.operatorSignature))
                return fail("PQ masternode operator signer is unavailable or does not match");
        }
        payload.data = pqmn::Encode(op);
        tx_new.extraPayload = pq::EncodePayload(payload);
    }
    const CTransaction signing_tx(tx_new);
    for (size_t i = 0; i < tx_new.vin.size(); ++i) {
        pq::KeyID id;
        mldsa44::Key key;
        if (!pq::ExtractID(prevouts[i].scriptPubKey, id) ||
            !GetPQKey(pq::EncodeAddress(id, Params().NetworkIDString()), key)) return fail("Could not decrypt PQ input key");
        const auto message = pq::SignatureMessage(signing_tx, prevouts, payload, consensus.hashGenesisBlock, i);
        std::vector<unsigned char> signature;
        const auto context = pq::SignatureContext(Params().NetworkIDString());
        if (message.empty() || !context || !key.Sign(message, *context, signature) ||
            signature.size() != payload.authorizations[i].signature.size()) return fail("Signing PQ transaction failed");
        std::copy(signature.begin(), signature.end(), payload.authorizations[i].signature.begin());
    }
    tx_new.extraPayload = pq::EncodePayload(payload);
    const CTransaction finalized(tx_new);
    if (!pq::VerifyInputs(finalized, prevouts, Params(), reason)) return false;
    CAmount output_value = 0;
    for (const auto& output : tx_new.vout) {
        if (!consensus.MoneyRange(output.nValue) || output.nValue > consensus.nMaxMoneyOut - output_value ||
            IsDust(output, dustRelayFee)) return fail("Invalid PQ output amount");
        output_value += output.nValue;
    }
    if (output_value > input_value) return fail("PQ inputs do not cover outputs");
    fee = input_value - output_value;
    const auto size = GetSerializeSize(finalized, PROTOCOL_VERSION);
    if (fee > maxTxFee || fee < required_fee(size) || fee < minRelayTxFee.GetFee(size))
        return fail("PQ transaction fee is outside wallet policy");
    tx = MakeTransactionRef(std::move(tx_new));
    reason.clear();
    return true;
}
