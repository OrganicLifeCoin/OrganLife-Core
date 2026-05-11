// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/walletutil.h"

#include "../init.h"
#include "util/system.h"
#include "validationinterface.h"
#include "wallet/wallet.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <vector>

namespace {
constexpr const char* MANAGED_WALLET_DIRNAME = "wallets";
constexpr const char* MANAGED_PRIMARY_WALLET_FILENAME = "wallet.dat";
constexpr const char* LEGACY_WALLET_LAYOUT_BACKUP_PREFIX = ".legacy-wallet-layout-backup";
constexpr const char* MIGRATION_TEMP_SUFFIX = ".migration.tmp";

std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool HasDatExtension(const fs::path& walletPath)
{
    return walletPath.has_extension() && walletPath.extension() == ".dat";
}

bool IsReservedManagedWalletName(const std::string& walletName)
{
    return ToLowerAscii(walletName) == "wallet";
}

std::string ManagedWalletDataFilename(const std::string& walletName)
{
    return walletName + ".dat";
}

bool IsSimpleWalletName(const std::string& walletName)
{
    if (walletName.empty()) {
        return false;
    }

    const fs::path walletPath(walletName);
    return !walletPath.is_absolute() &&
           !walletPath.has_parent_path() &&
           walletPath.filename() == walletPath &&
           walletName != "." &&
           walletName != "..";
}

fs::path WalletPathForName(const std::string& walletName)
{
    if (walletName.empty()) {
        return GetWalletDir();
    }
    return GetWalletDir() / fs::path(ManagedWalletDataFilename(walletName));
}

OperationResult VerifyManagedWalletName(const std::string& walletName)
{
    if (!IsSimpleWalletName(walletName)) {
        return {false, _("Wallet name must stay inside the wallets directory")};
    }
    if (IsReservedManagedWalletName(walletName)) {
        return {false, _("Wallet name is reserved for the primary wallet")};
    }
    if (HasDatExtension(fs::path(walletName))) {
        return {false, _("Wallet name must not include the .dat extension")};
    }

    return VerifyWalletPath(ManagedWalletDataFilename(walletName));
}

bool IsWalletLoadedInternal(const std::string& walletName)
{
    return std::any_of(vpwallets.begin(), vpwallets.end(), [&walletName](const CWalletRef wallet) {
        return wallet && wallet->GetName() == walletName;
    });
}

OperationResult LoadWalletInternal(const std::string& walletName, const bool createIfMissing, CWallet*& walletOut)
{
    walletOut = nullptr;

    const auto verifyNameResult = VerifyManagedWalletName(walletName);
    if (!verifyNameResult) {
        return verifyNameResult;
    }

    const fs::path walletDir = GetWalletDir();
    if (walletDir.empty()) {
        return {false, _("Wallet directory is not available")};
    }
    if (createIfMissing && !fs::exists(walletDir)) {
        fs::create_directories(walletDir);
    }

    if (IsWalletLoadedInternal(walletName)) {
        return {false, _("Wallet is already loaded")};
    }

    const fs::path walletPath = WalletPathForName(walletName);
    if (!createIfMissing && !fs::exists(walletPath)) {
        return {false, _("Wallet does not exist in the wallets directory")};
    }

    walletOut = CWallet::CreateWalletFromFile(walletName, walletPath);
    if (!walletOut) {
        return {false, _("Unable to load wallet")};
    }

    vpwallets.emplace_back(walletOut);
    walletOut->postInitProcess(GetNodeScheduler());
    StartWalletStakingThread(walletOut);
    return {true};
}

bool IsLegacyBackupDirectoryName(const std::string& name)
{
    return name.find(LEGACY_WALLET_LAYOUT_BACKUP_PREFIX) == 0;
}

bool IsLegacyPrimaryWalletArtifact(const fs::path& path)
{
    if (!fs::exists(path)) {
        return false;
    }

    const std::string name = path.filename().string();
    return name == MANAGED_PRIMARY_WALLET_FILENAME ||
           name == ".walletlock" ||
           name == "db.log" ||
           name == "database" ||
	           name.find("__db.") == 0;
}

bool FilesHaveEqualContents(const fs::path& lhs, const fs::path& rhs)
{
    if (!fs::is_regular_file(lhs) || !fs::is_regular_file(rhs)) {
        return false;
    }
    if (fs::file_size(lhs) != fs::file_size(rhs)) {
        return false;
    }

    std::ifstream lhsStream(lhs.string(), std::ios::binary);
    std::ifstream rhsStream(rhs.string(), std::ios::binary);
    if (!lhsStream.is_open() || !rhsStream.is_open()) {
        return false;
    }

    char lhsBuffer[4096];
    char rhsBuffer[4096];
    while (lhsStream.good() || rhsStream.good()) {
        lhsStream.read(lhsBuffer, sizeof(lhsBuffer));
        rhsStream.read(rhsBuffer, sizeof(rhsBuffer));
        const std::streamsize lhsCount = lhsStream.gcount();
        const std::streamsize rhsCount = rhsStream.gcount();
        if (lhsCount != rhsCount) {
            return false;
        }
        if (!std::equal(lhsBuffer, lhsBuffer + lhsCount, rhsBuffer)) {
            return false;
        }
        if (lhsCount == 0) {
            break;
        }
    }

    return true;
}

fs::path MigrationTempPathForTarget(const fs::path& targetPath)
{
    const fs::path parent = targetPath.parent_path();
    const std::string baseName = targetPath.filename().string();
    for (int suffix = 0; suffix < 100; ++suffix) {
        const std::string tempName = suffix == 0
                ? baseName + MIGRATION_TEMP_SUFFIX
                : strprintf("%s.%d%s", baseName, suffix, MIGRATION_TEMP_SUFFIX);
        const fs::path candidate = parent / tempName;
        if (!fs::exists(candidate)) {
            return candidate;
        }
    }
    return fs::path();
}
} // namespace

fs::path GetWalletDir()
{
    fs::path path;

    if (gArgs.IsArgSet("-walletdir")) {
        path = gArgs.GetArg("-walletdir", "");
        if (!fs::is_directory(path)) {
            // If the path specified doesn't exist, we return the deliberately
            // invalid empty string.
            path = "";
        }
    } else {
        path = GetDataDir() / MANAGED_WALLET_DIRNAME;
    }

    return path;
}

OperationResult VerifyWalletPath(const std::string& walletFile)
{
    // Do some checking on wallet path. It should be either a:
    //
    // 1. Path where a directory can be created.
    // 2. Path to an existing directory.
    // 3. Path to a symlink to a directory.
    // 4. For backwards compatibility, the name of a data file in -walletdir.
    fs::path wallet_path = fs::absolute(walletFile, GetWalletDir());
    fs::file_type path_type = fs::symlink_status(wallet_path).type();
    if (!(path_type == fs::file_not_found || path_type == fs::directory_file ||
          (path_type == fs::symlink_file && fs::is_directory(wallet_path)) ||
          (path_type == fs::regular_file && fs::path(walletFile).filename() == walletFile))) {
        return {false, (strprintf(
                _("Invalid -wallet path '%s'. -wallet path should point to a directory where wallet.dat and "
                  "database/log.?????????? files can be stored, a location where such a directory could be created, "
                  "or a wallet data file in -walletdir (%s)"),
                walletFile, GetWalletDir()))};
    }
    return {true};
}

OperationResult MigrateLegacyManagedWalletLayout(std::vector<std::string>& warnings)
{
    warnings.clear();

    if (gArgs.IsArgSet("-walletdir")) {
        return {true};
    }

    const fs::path dataDir = GetDataDir();
    if (dataDir.empty()) {
        return {false, _("Data directory is not available")};
    }

    const fs::path walletDir = GetWalletDir();
    if (walletDir.empty()) {
        return {false, _("Wallet directory is not available")};
    }

    if (!fs::is_directory(walletDir) && !TryCreateDirectories(walletDir)) {
        return {false, strprintf(_("Unable to create wallets directory %s"), walletDir.string())};
    }
    if (!fs::is_directory(walletDir)) {
        return {false, strprintf(_("Unable to create wallets directory %s"), walletDir.string())};
    }

    fs::path backupRoot;
    auto addWarning = [&warnings](const std::string& warning) {
        warnings.emplace_back(warning);
        LogPrintf("%s\n", warning);
    };
    auto logMigrationNote = [](const std::string& note) {
        LogPrintf("%s\n", note);
    };
    auto ensureBackupRoot = [&]() -> fs::path {
        if (!backupRoot.empty()) {
            return backupRoot;
        }

        for (int suffix = 0; suffix < 100; ++suffix) {
            const std::string candidateName = suffix == 0
                    ? strprintf("%s-%d", LEGACY_WALLET_LAYOUT_BACKUP_PREFIX, GetTime())
                    : strprintf("%s-%d-%d", LEGACY_WALLET_LAYOUT_BACKUP_PREFIX, GetTime(), suffix);
            const fs::path candidatePath = dataDir / candidateName;
            if (fs::exists(candidatePath)) {
                continue;
            }
            if (TryCreateDirectories(candidatePath) && fs::is_directory(candidatePath)) {
                backupRoot = candidatePath;
                return backupRoot;
            }
        }

        return fs::path();
    };
    auto backupTargetFor = [&](const fs::path& sourcePath) -> fs::path {
        const fs::path root = ensureBackupRoot();
        if (root.empty()) {
            return fs::path();
        }

        fs::path candidate = root / sourcePath.filename();
        for (int suffix = 1; fs::exists(candidate); ++suffix) {
            candidate = root / strprintf("%s-%d", sourcePath.filename().string(), suffix);
        }
        return candidate;
    };
    auto backupLegacyPath = [&](const fs::path& sourcePath) -> bool {
        if (!fs::exists(sourcePath)) {
            return true;
        }

        const fs::path backupPath = backupTargetFor(sourcePath);
        if (backupPath.empty()) {
            addWarning(strprintf("Unable to prepare a backup location for legacy wallet path %s", sourcePath.string()));
            return false;
        }

        try {
            fs::rename(sourcePath, backupPath);
            logMigrationNote(strprintf("Moved legacy wallet path %s to backup location %s",
                                       sourcePath.string(),
                                       backupPath.string()));
            return true;
        } catch (const fs::filesystem_error& e) {
            addWarning(strprintf("Unable to move legacy wallet path %s to backup location: %s",
                                 sourcePath.string(),
                                 e.what()));
            return false;
        }
    };
    auto stageMigratedWalletFile = [&](const fs::path& sourceWalletFile, const fs::path& targetWalletFile) -> OperationResult {
        if (!fs::exists(sourceWalletFile)) {
            return {true};
        }

        if (fs::exists(targetWalletFile)) {
            addWarning(strprintf("Legacy wallet file %s was left in backup because target %s already exists",
                                 sourceWalletFile.string(),
                                 targetWalletFile.string()));
            return {true};
        }

        const fs::path tempTargetPath = MigrationTempPathForTarget(targetWalletFile);
        if (tempTargetPath.empty()) {
            return {false, strprintf(_("Unable to create a temporary migration target for %s"),
                                     targetWalletFile.string())};
        }

        try {
            TryCreateDirectories(targetWalletFile.parent_path());
            fs::copy_file(sourceWalletFile, tempTargetPath);
            if (!FilesHaveEqualContents(sourceWalletFile, tempTargetPath)) {
                fs::remove(tempTargetPath);
                return {false, strprintf(_("Copied wallet file %s did not match the original during migration"),
                                         sourceWalletFile.string())};
            }
            fs::rename(tempTargetPath, targetWalletFile);
            LogPrintf("Migrated legacy wallet file %s to %s\n",
                      sourceWalletFile.string(),
                      targetWalletFile.string());
            return {true};
        } catch (const fs::filesystem_error& e) {
            if (fs::exists(tempTargetPath)) {
                try {
                    fs::remove(tempTargetPath);
                } catch (const fs::filesystem_error&) {
                }
            }
            return {false, strprintf(_("Unable to migrate wallet file %s to %s: %s"),
                                     sourceWalletFile.string(),
                                     targetWalletFile.string(),
                                     e.what())};
        }
    };

    const fs::path legacyPrimaryWalletFile = dataDir / MANAGED_PRIMARY_WALLET_FILENAME;
    const bool legacyPrimaryDetected = fs::exists(legacyPrimaryWalletFile) ||
                                       IsLegacyPrimaryWalletArtifact(dataDir / "database") ||
                                       IsLegacyPrimaryWalletArtifact(dataDir / "db.log") ||
                                       IsLegacyPrimaryWalletArtifact(dataDir / ".walletlock");
    const OperationResult primaryMigrationResult = stageMigratedWalletFile(legacyPrimaryWalletFile,
                                                                           walletDir / MANAGED_PRIMARY_WALLET_FILENAME);
    if (!primaryMigrationResult) {
        return primaryMigrationResult;
    }

    std::vector<fs::path> legacyWalletDirs;
    for (const fs::directory_entry& entry : fs::directory_iterator(dataDir)) {
        if (!entry.is_directory()) {
            continue;
        }

        const std::string walletName = entry.path().filename().string();
        if (walletName == MANAGED_WALLET_DIRNAME || IsLegacyBackupDirectoryName(walletName)) {
            continue;
        }
        if (!IsSimpleWalletName(walletName)) {
            continue;
        }
        if (!fs::is_regular_file(entry.path() / MANAGED_PRIMARY_WALLET_FILENAME)) {
            continue;
        }
        if (IsReservedManagedWalletName(walletName)) {
            return {false, strprintf(_("Legacy wallet directory %s conflicts with the primary wallet file name"),
                                     walletName)};
        }
        legacyWalletDirs.emplace_back(entry.path());
    }

    std::sort(legacyWalletDirs.begin(), legacyWalletDirs.end());

    for (const fs::path& legacyWalletDir : legacyWalletDirs) {
        const std::string walletName = legacyWalletDir.filename().string();
        const OperationResult walletMigrationResult = stageMigratedWalletFile(legacyWalletDir / MANAGED_PRIMARY_WALLET_FILENAME,
                                                                              WalletPathForName(walletName));
        if (!walletMigrationResult) {
            return walletMigrationResult;
        }

        if (fs::exists(legacyWalletDir)) {
            try {
                backupLegacyPath(legacyWalletDir);
            } catch (const fs::filesystem_error& e) {
                addWarning(strprintf("Unable to clean up legacy wallet directory %s: %s",
                                     legacyWalletDir.string(),
                                     e.what()));
            }
        }
    }

    if (legacyPrimaryDetected) {
        for (const fs::directory_entry& entry : fs::directory_iterator(dataDir)) {
            if (entry.path() == walletDir) {
                continue;
            }
            if (IsLegacyBackupDirectoryName(entry.path().filename().string())) {
                continue;
            }
            if (!IsLegacyPrimaryWalletArtifact(entry.path())) {
                continue;
            }
            backupLegacyPath(entry.path());
        }
    }

    return {true};
}

std::vector<std::string> ListWalletDir()
{
    std::vector<std::string> walletNames;
    const fs::path walletDir = GetWalletDir();
    if (walletDir.empty() || !fs::is_directory(walletDir)) {
        return walletNames;
    }

    for (const fs::directory_entry& entry : fs::directory_iterator(walletDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const fs::path filename = entry.path().filename();
        if (!HasDatExtension(filename) || filename == MANAGED_PRIMARY_WALLET_FILENAME) {
            continue;
        }
        walletNames.emplace_back(filename.stem().string());
    }

    std::sort(walletNames.begin(), walletNames.end());
    return walletNames;
}

OperationResult CreateWalletInDir(const std::string& walletName, CWallet*& walletOut)
{
    return LoadWalletInternal(walletName, /*createIfMissing=*/true, walletOut);
}

OperationResult LoadWalletInDir(const std::string& walletName, CWallet*& walletOut)
{
    return LoadWalletInternal(walletName, /*createIfMissing=*/false, walletOut);
}

OperationResult UnloadWalletByName(const std::string& walletName)
{
    const auto verifyNameResult = VerifyManagedWalletName(walletName);
    if (!verifyNameResult) {
        return verifyNameResult;
    }

    const auto it = std::find_if(vpwallets.begin(), vpwallets.end(), [&walletName](const CWalletRef wallet) {
        return wallet && wallet->GetName() == walletName;
    });
    if (it == vpwallets.end()) {
        return {false, _("Wallet is not loaded")};
    }

    CWallet* wallet = *it;
    StopWalletStakingThread(wallet);
    uiInterface.UnloadWallet(wallet);
    UnregisterValidationInterface(wallet);
    GetMainSignals().FlushBackgroundCallbacks();
    wallet->Flush(false);
    wallet->Flush(true);
    vpwallets.erase(it);
    delete wallet;
    return {true};
}
