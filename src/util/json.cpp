// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/json.h"

#include <format>

namespace knobs {

std::string JsonQuote(std::string_view text) {
  std::string out = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      out += std::format("\\u{:04x}", static_cast<unsigned>(c));
    } else {
      out += c;
    }
  }
  out += '"';
  return out;
}

}  // namespace knobs
