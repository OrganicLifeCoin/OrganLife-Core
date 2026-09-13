// Copyright (c) 2019-2022 The PIVX Core developers
// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "mnmodel.h"

#include "chainparams.h"
#include "evo/deterministicmns.h"
#include "net.h" // for validateMasternodeIP
#include "netbase.h"
#include "optional.h"
#include "qt/walletmodel.h"
#include "tiertwo/tiertwo_sync_state.h"
#include <validation.h>
#include <utilstrencodings.h>

#include <QHostAddress>
#include <QSettings>

MNModel::MNModel(QObject *parent) : QAbstractTableModel(parent) {}

// Returns the DMN for the given conf entry at the chain tip, or null if the
// entry is not (yet) registered.
static CDeterministicMNCPtr findDMN(const CMasternodeConfig::CMasternodeEntry& mne)
{
    if (!deterministicMNManager) return nullptr;
    CService service;
    if (!Lookup(mne.getIp(), service, Params().GetDefaultPort(), false)) return nullptr;
    return deterministicMNManager->GetListAtChainTip().GetMNByService(service);
}

// Returns the conf entry for the given alias, if found.
static Optional<CMasternodeConfig::CMasternodeEntry> getMasternodeEntry(const QString& mnAlias)
{
    const std::string alias = mnAlias.toStdString();
    for (const auto& mne : masternodeConfig.getEntries()) {
        if (mne.getAlias() == alias) return mne;
    }
    return nullopt;
}

void MNModel::init()
{
    updateMNList();
}

void MNModel::updateMNList()
{
    beginResetModel();
    nodes.clear();
    collateralTxAccepted.clear();
    pqNodes.clear();
    pqError.clear();
    if (Params().SupportsPQ()) {
        LOCK(cs_main);
        pqHeight = chainActive.Height();
        try {
            if (!pq::MasternodesActive(Params(), pqHeight + 1) || !evoDb)
                throw std::runtime_error("inactive registry");
            pqmn::Index registry(*evoDb, Params());
            if (!registry.MatchesChainTip(chainActive.Tip())) throw std::runtime_error("stale registry");
            pqNodes = registry.List();
        } catch (const std::exception&) {
            pqError = tr("PQ registry inactive, unavailable or syncing. No controller actions are available.");
        }
        endResetModel();
        return;
    }
    for (const CMasternodeConfig::CMasternodeEntry& mne : masternodeConfig.getEntries()) {
        CDeterministicMNCPtr dmn = nullptr;
        CService service;
        if (deterministicMNManager &&
            Lookup(mne.getIp(), service, Params().GetDefaultPort(), false)) {
            dmn = deterministicMNManager->GetListAtChainTip().GetMNByService(service);
        }
        nodes.insert(QString::fromStdString(mne.getAlias()), std::make_pair(QString::fromStdString(mne.getIp()), dmn));
        collateralTxAccepted.insert(mne.getAlias(), dmn != nullptr);
    }
    endResetModel();
}

int MNModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return Params().SupportsPQ() ? pqNodes.size() : nodes.size();
}

int MNModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return WAS_COLLATERAL_ACCEPTED + 1;
}

Optional<std::pair<uint256, pqmn::Record>> MNModel::pqRecord(const QModelIndex& index) const
{
    if (!index.isValid() || index.model() != this || index.row() < 0 ||
        static_cast<size_t>(index.row()) >= pqNodes.size()) return nullopt;
    return pqNodes[index.row()];
}


QVariant MNModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
            return QVariant();

    int row = index.row();
    if (Params().SupportsPQ() && role == Qt::ToolTipRole) {
        const auto entry = pqRecord(index);
        return entry ? QVariant(QString::fromStdString(entry->first.GetHex())) : QVariant();
    }
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        if (Params().SupportsPQ()) {
            const auto entry = pqRecord(index);
            if (!entry) return {};
            const auto& record = entry->second;
            switch (index.column()) {
            case ALIAS: {
                const auto id = QString::fromStdString(entry->first.GetHex());
                if (role == Qt::EditRole) return id;
                return QSettings().value("pqMasternodeNames/" + QString::fromStdString(Params().GetConsensus().hashGenesisBlock.GetHex()) + "/" + id,
                                         id.left(12) + "…" + id.right(8));
            }
            case ADDRESS: return record.service == CService() ? tr("Endpoint not set") : QString::fromStdString(record.service.ToString());
            case STATUS: return record.revoked ? tr("REVOKED") :
                (record.MatureAt(std::max(0, pqHeight), Params().GetConsensus().MasternodeCollateralMinConf()) ?
                    tr("REGISTERED") : tr("IMMATURE"));
            case PUB_KEY: return QString::fromStdString(HexStr(record.operatorKey));
            case COLLATERAL_ID: return QString::fromStdString(record.collateral.hash.GetHex());
            case COLLATERAL_OUT_INDEX: return QString::number(record.collateral.n);
            case WAS_COLLATERAL_ACCEPTED: return true;
            default: return {}; // Never return operator secrets or legacy authority fields.
            }
        }
        if (row < 0 || row >= nodes.size())
            return QVariant();
        const QString mnAlias = nodes.keys().value(row);
        const std::pair<QString, CDeterministicMNCPtr>& pair = nodes.values().value(row);
        const CDeterministicMNCPtr& dmn = pair.second;
        switch (index.column()) {
            case ALIAS:
                return mnAlias;
            case ADDRESS:
                return pair.first;
            case PUB_KEY:
                return (dmn) ? QString::fromStdString(dmn->pdmnState->pubKeyOperator.Get().GetHash().GetHex()) : "Not available";
            case COLLATERAL_ID:
                return (dmn) ? QString::fromStdString(dmn->collateralOutpoint.hash.GetHex()) : "Not available";
            case COLLATERAL_OUT_INDEX:
                return (dmn) ? QString::number(dmn->collateralOutpoint.n) : "Not available";
            case STATUS: {
                if (!dmn) return tr("MISSING");
                return dmn->IsPoSeBanned() ? tr("POSE_BANNED") : tr("ENABLED");
            }
            case PRIV_KEY: {
                const Optional<CMasternodeConfig::CMasternodeEntry>& mne = getMasternodeEntry(mnAlias);
                return (mne) ? QString::fromStdString(mne->getPrivKey()) : "Not available";
            }
            case WAS_COLLATERAL_ACCEPTED:{
                return collateralTxAccepted.value(mnAlias.toStdString());
            }
        }
    }
    return QVariant();
}

QModelIndex MNModel::index(int row, int column, const QModelIndex& parent) const
{
    if (parent.isValid() || row < 0 || row >= rowCount() || column < 0 || column >= columnCount())
        return QModelIndex();
    return createIndex(row, column, nullptr);
}


bool MNModel::removeMn(const QModelIndex& modelIndex)
{
    QString alias = modelIndex.data(Qt::DisplayRole).toString();
    int idx = modelIndex.row();
    beginRemoveRows(QModelIndex(), idx, idx);
    nodes.take(alias);
    endRemoveRows();
    Q_EMIT dataChanged(index(idx, 0, QModelIndex()), index(idx, 5, QModelIndex()) );
    return true;
}

bool MNModel::addMn(CMasternodeConfig::CMasternodeEntry* mne)
{
    if (!mne) return false;

    beginInsertRows(QModelIndex(), nodes.size(), nodes.size());
    nodes.insert(QString::fromStdString(mne->getAlias()),
                 std::make_pair(QString::fromStdString(mne->getIp()), nullptr));
    endInsertRows();
    return true;
}

bool MNModel::isMNInactive(const QString& mnAlias)
{
    return !isMNActive(mnAlias);
}

bool MNModel::isMNActive(const QString& mnAlias)
{
    const Optional<CMasternodeConfig::CMasternodeEntry>& mne = getMasternodeEntry(mnAlias);
    if (!mne) return false;
    return findDMN(*mne) != nullptr;
}

bool MNModel::isMNCollateralMature(const QString& mnAlias)
{
    // The collateral is created inside the registration tx, always mature.
    return true;
}

bool MNModel::isMNsNetworkSynced()
{
    return g_tiertwo_sync_state.IsSynced();
}

bool MNModel::validateMNIP(const QString& addrStr)
{
    return validateMasternodeIP(addrStr.toStdString());
}

CAmount MNModel::getMNCollateralRequiredAmount()
{
    return Params().GetConsensus().nMNCollateralAmt;
}

bool MNModel::startDMN(const CMasternodeConfig::CMasternodeEntry& mne, std::string& strError)
{
    Q_UNUSED(mne);
    strError = tr("Masternodes are unavailable in PQ-only mode").toStdString();
    return false;
}

void MNModel::startAllMNs(bool onlyMissing, int& amountOfMnFailed, int& amountOfMnStarted,
                          std::string* aliasFilter, std::string* error_ret)
{
    for (const auto& mne : masternodeConfig.getEntries()) {
        if (!aliasFilter) {
            // Check for missing only
            QString mnAlias = QString::fromStdString(mne.getAlias());
            if (onlyMissing && !isMNInactive(mnAlias)) {
                if (!isMNActive(mnAlias))
                    amountOfMnFailed++;
                continue;
            }
        } else if (*aliasFilter != mne.getAlias()){
            continue;
        }

        std::string ret_str;
        if (!startDMN(mne, ret_str)) {
            amountOfMnFailed++;
            if (error_ret) *error_ret = ret_str;
        } else {
            amountOfMnStarted++;
        }
    }
}

CMasternodeConfig::CMasternodeEntry* MNModel::createDMNEntry(const std::string& alias,
                             const std::string& serviceAddr,
                             const std::string& port,
                             const std::string& operatorKeyString,
                             QString& ret_error)
{
    // Update the conf file
    QString strConfFileQt(PIVX_MASTERNODE_CONF_FILENAME);
    std::string strConfFile = strConfFileQt.toStdString();
    std::string strDataDir = GetDataDir().string();
    fs::path conf_file_path(strConfFile);
    if (strConfFile != conf_file_path.filename().string()) {
        throw std::runtime_error(strprintf(_("%s %s resides outside data directory %s"), strConfFile, strConfFile, strDataDir));
    }

    fs::path pathBootstrap = GetDataDir() / strConfFile;
    if (!fs::exists(pathBootstrap)) {
        ret_error = tr("%1 file doesn't exists").arg(strConfFileQt);
        return nullptr;
    }

    fs::path pathMasternodeConfigFile = GetMasternodeConfigFile();
    fsbridge::ifstream streamConfig(pathMasternodeConfigFile);

    if (!streamConfig.good()) {
        ret_error = tr("Invalid %1 file").arg(strConfFileQt);
        return nullptr;
    }

    int linenumber = 1;
    std::string lineCopy;
    for (std::string line; std::getline(streamConfig, line); linenumber++) {
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string comment, alias, ip, privKey, txHash, outputIndex;

        if (iss >> comment) {
            if (comment.at(0) == '#') continue;
            iss.str(line);
            iss.clear();
        }

        if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex)) {
            iss.str(line);
            iss.clear();
            // Tolerate deterministic (3-field) entries when copying existing lines.
            if (!(iss >> alias >> ip >> privKey)) {
                streamConfig.close();
                ret_error = tr("Error parsing %1 file").arg(strConfFileQt);
                return nullptr;
            }
        }
        lineCopy += line + "\n";
    }

    if (lineCopy.empty()) {
        lineCopy = "# Masternode config file\n"
                   "# Format: alias IP:port masternodeprivkey collateral_output_txid collateral_output_index\n"
                   "# Example: mn1 127.0.0.2:31616 93HaYBVUCYjEMeeH1Y4sBGLALQZE1Yc1K64xiqgX37tGBDQL8Xg 2bcd3c84c84f87eaa86e4e56834c92927a07f9e18718810b92e0d0324456a67c 0"
                   "#";
    }
    lineCopy += "\n";

    streamConfig.close();

    // Check IP address type
    std::string ipAddress = serviceAddr;
    QHostAddress hostAddress(QString::fromStdString(ipAddress));
    QAbstractSocket::NetworkLayerProtocol layerProtocol = hostAddress.protocol();
    if (layerProtocol == QAbstractSocket::IPv6Protocol) {
        ipAddress = "["+ipAddress+"]";
    }

    // Deterministic entries carry only 3 fields: alias IP:port BLS-operator-key.
    // No collateral output is created here: the registration tx embeds and locks it.
    fs::path pathConfigFile = AbsPathForConfigVal(fs::path("masternode_temp.conf"));
    FILE* configFile = fopen(pathConfigFile.string().c_str(), "w");
    lineCopy += alias+" "+ipAddress+":"+port+" "+operatorKeyString+"\n";
    fwrite(lineCopy.c_str(), std::strlen(lineCopy.c_str()), 1, configFile);
    fclose(configFile);

    fs::path pathOldConfFile = AbsPathForConfigVal(fs::path("old_masternode.conf"));
    if (fs::exists(pathOldConfFile)) {
        fs::remove(pathOldConfFile);
    }
    rename(pathMasternodeConfigFile, pathOldConfFile);

    fs::path pathNewConfFile = AbsPathForConfigVal(fs::path(strConfFile));
    rename(pathConfigFile, pathNewConfFile);

    return masternodeConfig.add(alias, ipAddress+":"+port, operatorKeyString, "", "");
}

void MNModel::setCoinControl(CCoinControl* coinControl)
{
    this->coinControl = coinControl;
}

void MNModel::resetCoinControl()
{
    coinControl = nullptr;
}
