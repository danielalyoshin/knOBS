// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/text_file.h"

#include <format>
#include <fstream>
#include <sstream>

#include "util/win_strings.h"

namespace knobs::tools {

Result<std::string> ReadText(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return Error{std::format("Couldn't open {}.", ToUtf8(file))};
  std::ostringstream text;
  text << in.rdbuf();
  return text.str();
}

}  // namespace knobs::tools
