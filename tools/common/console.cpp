// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/console.h"

#include <cmath>
#include <cstdio>
#include <format>

#include "runtime/obs_host.h"
#include "util/win_strings.h"

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
    const char* note = !filter.known   ? " (unknown to libobs: passes audio through)"
                       : !filter.audio ? " (not an audio filter: passes audio through)"
                                       : "";
    // Indented to line up with the detail column above.
    Print(std::format("{:26}{}. {} \"{}\"{}{}\n", "", i + 1, filter.type, filter.name,
                      filter.enabled ? "" : " (disabled)", note));
  }
}

std::string FormatPeak(float peak) {
  return peak > 0 ? std::format("{:.1f} dBFS", 20 * std::log10(peak)) : std::string("silence");
}

int FinishRun(runtime::ObsHost& host) {
  const long leaks = host.Shutdown();
  Check(leaks == 0, "shutdown",
        leaks == 0 ? "0 leaked allocations" : std::format("{} libobs allocations leaked", leaks));
  Report(Outcome::kNote, "libobs log",
         std::format("{} ({} warnings/errors)", ToUtf8(host.log().path()), host.log().problem_count()));
  for (const std::string& line : host.log().RecentProblems()) Print(std::format("{:26}{}\n", "", line));
  Print(AnyFailed() ? "FAIL\n" : "PASS\n");
  return AnyFailed() ? kExitFail : kExitPass;
}

}  // namespace knobs::tools
