// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_WALLET_PQCREDENTIALS_WIN_H
#define ORGANICLIFE_WALLET_PQCREDENTIALS_WIN_H
#include <span.h>
#include <string>

namespace pqwallet {
// Read-only Windows delivery. Caller owns/cleanses both buffers and trusts the
// directory ancestors and service identity. Failure clears both output spans.
bool ReadWindowsOperatorCredentials(const std::wstring& directory,
    Span<unsigned char> record, Span<unsigned char> wrappingKey);
bool IsPrivateWindowsConfigFile(const std::wstring& path, const std::string& expected);
// Exclusive same-parent publication after private creation, flush and readback.
// Caller validates the encrypted record and supplies 16 fresh random bytes.
bool WriteWindowsOperatorCredentials(const std::wstring& directory,
    Span<const unsigned char> record, Span<const unsigned char> wrappingKey,
    Span<const unsigned char> random);
}
#endif
