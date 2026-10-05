// SPDX-License-Identifier: GPL-2.0-or-later
#include "import/obs_ini.h"

#include "util/text_file.h"

namespace knobs::import {
namespace {

bool IsNewline(char c) { return c == '\r' || c == '\n'; }
bool IsWhitespace(char c) { return c == ' ' || c == '\t' || IsNewline(c); }

// config-file.c, unescape: only \\, \r and \n are escapes. A backslash before
// anything else stays.
std::string Unescape(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '\\' && i + 1 < in.size()) {
      const char next = in[i + 1];
      if (next == '\\') {
        ++i;
      } else if (next == 'r') {
        c = '\r';
        ++i;
      } else if (next == 'n') {
        c = '\n';
        ++i;
      }
    }
    out += c;
  }
  return out;
}

}  // namespace

ObsIni ObsIni::Parse(std::string_view text) {
  // libobs reads the file as a C string after skipping the BOM.
  if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
  if (const size_t nul = text.find('\0'); nul != std::string_view::npos) text = text.substr(0, nul);

  ObsIni ini;
  Section* section = nullptr;
  const size_t n = text.size();
  size_t pos = 0;
  const auto skip_line = [&] {
    while (pos < n && !IsNewline(text[pos])) ++pos;
    if (pos < n) ++pos;
  };
  while (pos < n) {
    const char c = text[pos];
    if (IsWhitespace(c)) {
      ++pos;
      continue;
    }
    if (c == '[') {
      // The name runs to ']' or the end of the line, and parsing goes on
      // right after it.
      const size_t start = ++pos;
      while (pos < n && text[pos] != ']' && !IsNewline(text[pos])) ++pos;
      const std::string name(text.substr(start, pos - start));
      if (pos < n) ++pos;
      if (name.empty()) break;  // libobs stops reading the file here.
      section = &ini.sections_[name];
      section->clear();
      continue;
    }
    // Before the first section only headers count.
    if (!section || c == '#') {
      skip_line();
      continue;
    }
    // The key's first character is never taken for the '='.
    const size_t key_start = pos++;
    while (pos < n && text[pos] != '=' && !IsNewline(text[pos])) ++pos;
    if (pos < n && IsNewline(text[pos])) {
      ++pos;  // No '=' on the line: not an item.
      continue;
    }
    std::string key(text.substr(key_start, pos - key_start));
    if (pos < n) ++pos;
    const size_t value_start = pos;
    while (pos < n && !IsNewline(text[pos])) ++pos;
    (*section)[std::move(key)] = Unescape(text.substr(value_start, pos - value_start));
  }
  return ini;
}

Result<ObsIni> ObsIni::Read(const std::filesystem::path& file) {
  auto text = ReadText(file);
  if (!text) return Error{text.error()};
  return Parse(*text);
}

std::optional<std::string> ObsIni::Get(std::string_view section, std::string_view key) const {
  const auto s = sections_.find(section);
  if (s == sections_.end()) return std::nullopt;
  const auto item = s->second.find(key);
  if (item == s->second.end()) return std::nullopt;
  return item->second;
}

}  // namespace knobs::import
