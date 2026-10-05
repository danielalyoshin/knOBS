// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "util/result.h"

namespace knobs::import {

// An OBS settings file (global.ini, user.ini, a profile's basic.ini), read
// the way libobs's parser reads it (util/config-file.c): a UTF-8 BOM is
// skipped; "[section]" lines start sections; "key=value" lines inside one set
// values, with nothing trimmed but the key's leading whitespace; lines
// starting with '#' are comments; and \\, \n and \r in values are unescaped.
// A repeated key or section replaces the earlier one, as libobs's hash
// lookup finds the last. Read-only: knOBS never writes OBS's settings.
class ObsIni {
 public:
  static ObsIni Parse(std::string_view text);
  // An error if the file is missing or can't be read.
  static Result<ObsIni> Read(const std::filesystem::path& file);

  // nullopt if the section or key isn't there.
  std::optional<std::string> Get(std::string_view section, std::string_view key) const;

 private:
  using Section = std::map<std::string, std::string, std::less<>>;
  std::map<std::string, Section, std::less<>> sections_;
};

}  // namespace knobs::import
