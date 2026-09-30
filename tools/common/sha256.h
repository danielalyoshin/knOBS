// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace knobs::tools {

// Lowercase hex SHA-256 of `size` bytes at `data`, via Windows CNG. Empty if
// CNG fails.
std::string Sha256Hex(const void* data, size_t size);

}  // namespace knobs::tools
