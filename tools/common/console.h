// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <string_view>

#include "audio/live_chain.h"

namespace knobs::runtime {
class ObsHost;
}

// Console reporting shared by the knOBS tools: one "[tag] step  detail" line
// per check.
namespace knobs::tools {

// Exit codes. 77 is what CTest's SKIP_RETURN_CODE is set to.
inline constexpr int kExitPass = 0;
inline constexpr int kExitFail = 1;
inline constexpr int kExitUsage = 2;
inline constexpr int kExitSkip = 77;

// Writes UTF-8 to stdout and flushes.
void Print(std::string_view text);

// kWarn is for something the user should know that doesn't fail the run.
enum class Outcome { kOk, kFail, kWarn, kNote };

// Prints one result line. kFail also marks the run as failed.
void Report(Outcome outcome, std::string_view step, std::string_view detail);
void Check(bool ok, std::string_view step, std::string_view detail);

// Whether anything was reported as kFail.
bool AnyFailed();

// Reports a loaded chain as a note: its source-level state and filters.
void ReportChain(const audio::ChainInfo& chain);

// A peak sample level as "-6.0 dBFS", or "silence" for 0.
std::string FormatPeak(float peak);

// Ends a tool's run: shuts libobs down, reports leaks and the log's
// warnings, and prints PASS or FAIL. Returns the exit code.
int FinishRun(runtime::ObsHost& host);

}  // namespace knobs::tools
