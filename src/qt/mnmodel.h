// Copyright (c) 2019-2022 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_MNMODEL_H
#define PIVX_QT_MNMODEL_H

#include <QAbstractTableModel>
#include "masternodeconfig.h"
#include "qt/walletmodel.h"

#include <memory>

class CDeterministicMN;
typedef std::shared_ptr<const CDeterministicMN> CDeterministicMNCPtr;

class MNModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit MNModel(QObject *parent);
    ~MNModel() override {
        nodes.clear();
        collateralTxAccepted.clear();
    }
    void init();
    void setWalletModel(WalletModel* _model) { walletModel = _model; };

    enum ColumnIndex {
        ALIAS = 0,  /**< User specified MN alias */
        ADDRESS = 1, /**< Node address */
        PROTO_VERSION = 2, /**< Node protocol version */
        STATUS = 3, /**< Node status */
        ACTIVE_TIMESTAMP = 4, /**<  */
        PUB_KEY = 5,
        COLLATERAL_ID = 6,
        COLLATERAL_OUT_INDEX = 7,
        PRIV_KEY = 8,
        WAS_COLLATERAL_ACCEPTED = 9
    };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QModelIndex index(int row, int column, const QModelIndex& parent) const override;
    bool removeMn(const QModelIndex& index);
    bool addMn(CMasternodeConfig::CMasternodeEntry* entry);
    void updateMNList();


    bool isMNsNetworkSynced();
    // Checks if the masternode is inactive
    bool isMNInactive(const QString& mnAlias);
    // Masternode is active if it's registered in the DMN list at the chain tip
    bool isMNActive(const QString& mnAlias);
    // Deterministic masternodes embed and lock the collateral in the registration
    // transaction: always mature.
    bool isMNCollateralMature(const QString& mnAlias);
    // Validate string representing a masternode IP address
    static bool validateMNIP(const QString& addrStr);

    // Return the specific chain amount value for the MN collateral output.
    CAmount getMNCollateralRequiredAmount();

    // Starts a deterministic masternode (simple DMN flow): the 4000 OLC collateral
    // is embedded and locked in the registration transaction.
    bool startDMN(const CMasternodeConfig::CMasternodeEntry& mne, std::string& strError);
    // Starts all configured masternodes (deterministic flow only).
    void startAllMNs(bool onlyMissing, int& amountOfMnFailed, int& amountOfMnStarted,
                     std::string* aliasFilter = nullptr, std::string* error_ret = nullptr);

    // Appends a 3-field deterministic masternode entry (alias IP:port BLS-operator-key)
    // to masternode.conf. Does not create or lock any collateral: the registration
    // transaction embeds it.
    CMasternodeConfig::CMasternodeEntry* createDMNEntry(const std::string& alias,
                                                        const std::string& serviceAddr,
                                                        const std::string& port,
                                                        const std::string& operatorKeyString,
                                                        QString& ret_error);

    void setCoinControl(CCoinControl* coinControl);
    void resetCoinControl();

private:
    WalletModel* walletModel;
    CCoinControl* coinControl;
    // alias mn node ---> pair <ip, dmn (null if not registered)>
    QMap<QString, std::pair<QString, CDeterministicMNCPtr>> nodes;
    QMap<std::string, bool> collateralTxAccepted;
};

#endif // PIVX_QT_MNMODEL_H
