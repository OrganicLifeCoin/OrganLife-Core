// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_WALLET_WALLETUTIL_H
#define PIVX_WALLET_WALLETUTIL_H

#include "fs.h"
#include "operationresult.h"

#include <string>
#include <vector>

class CWallet;

//! Get the path of the wallet directory.
fs::path GetWalletDir();
//! Verify the wallet db's path
OperationResult VerifyWalletPath(const std::string& walletFile);
//! Migrate legacy root and directory-backed wallets into the managed wallets layout.
OperationResult MigrateLegacyManagedWalletLayout(std::vector<std::string>& warnings);
//! Enumerate managed wallet data files inside GetWalletDir().
std::vector<std::string> ListWalletDir();
//! Runtime wallet lifecycle helpers restricted to GetWalletDir() names.
OperationResult CreateWalletInDir(const std::string& walletName, CWallet*& walletOut);
OperationResult LoadWalletInDir(const std::string& walletName, CWallet*& walletOut);
OperationResult UnloadWalletByName(const std::string& walletName);

#endif // PIVX_WALLET_WALLETUTIL_H
