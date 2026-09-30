// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdint>
#include <format>

namespace knobs::tools {

std::string Sha256Hex(const void* data, size_t size) {
  BCRYPT_HASH_HANDLE hash = nullptr;
  // CNG's pseudo-handle needs no open or close (Windows 10+).
  if (!BCRYPT_SUCCESS(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &hash, nullptr, 0, nullptr, 0, 0))) {
    return {};
  }
  bool ok = true;
  const auto* bytes = static_cast<const uint8_t*>(data);
  while (ok && size > 0) {
    const ULONG chunk = static_cast<ULONG>(std::min<size_t>(size, size_t{1} << 30));
    ok = BCRYPT_SUCCESS(BCryptHashData(hash, const_cast<PUCHAR>(bytes), chunk, 0));
    bytes += chunk;
    size -= chunk;
  }
  uint8_t digest[32] = {};
  ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
  BCryptDestroyHash(hash);
  if (!ok) return {};

  std::string hex;
  for (const uint8_t byte : digest) hex += std::format("{:02x}", byte);
  return hex;
}

}  // namespace knobs::tools
