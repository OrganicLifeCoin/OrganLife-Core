// Copyright (c) 2026 The OrganicLife Coin developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef ORGANICLIFE_PQADDRESS_H
#define ORGANICLIFE_PQADDRESS_H

#include <crypto/mldsa44.h>
#include <optional.h>
#include <string>

namespace pq {
using KeyID = std::array<unsigned char, 32>;
Optional<KeyID> GetID(const mldsa44::PublicKey& public_key, const std::string& network);
std::string EncodeAddress(const KeyID& id, const std::string& network);
bool DecodeAddress(const std::string& address, const std::string& network, KeyID& id);
} // namespace pq
#endif
