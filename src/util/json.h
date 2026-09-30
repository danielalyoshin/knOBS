// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace knobs {

// `text` (UTF-8) as a JSON string literal, quotes included.
std::string JsonQuote(std::string_view text);

}  // namespace knobs
