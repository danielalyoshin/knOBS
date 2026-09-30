// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/console.h"

#include <cstdio>
#include <format>

namespace knobs::tools {
namespace {

bool g_failed = false;

}  // namespace

void Print(std::string_view text) {
  fwrite(text.data(), 1, text.size(), stdout);
  fflush(stdout);
}

void Report(Outcome outcome, std::string_view step, std::string_view detail) {
  const char* tag = outcome == Outcome::kOk ? "ok" : outcome == Outcome::kFail ? "FAIL" : "--";
  if (outcome == Outcome::kFail) g_failed = true;
  Print(std::format("[{:<4}] {:<18} {}\n", tag, step, detail));
}

void Check(bool ok, std::string_view step, std::string_view detail) {
  Report(ok ? Outcome::kOk : Outcome::kFail, step, detail);
}

bool AnyFailed() { return g_failed; }

void ReportChain(const audio::ChainInfo& chain) {
  Report(Outcome::kNote, "chain",
         std::format("{}: balance {:.2f}, Mono {}, then {} filter(s), then volume {:.2f}", chain.type,
                     chain.balance, chain.mono ? "on" : "off", chain.filters.size(), chain.volume));
  for (size_t i = 0; i < chain.filters.size(); ++i) {
    const auto& filter = chain.filters[i];
    // Indented to line up with the detail column above.
    Print(std::format("{:26}{}. {} \"{}\"{}{}\n", "", i + 1, filter.type, filter.name,
                      filter.enabled ? "" : " (disabled)",
                      filter.known ? "" : " (unknown to libobs: passes audio through)"));
  }
}

}  // namespace knobs::tools
