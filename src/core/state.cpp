// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/state.h"

#include <format>

namespace knobs::core {
namespace {

constexpr std::string_view kSeparator = " › ";  // " › "

}  // namespace

std::string_view StateName(State state) {
  switch (state) {
    case State::kStarting:
      return "starting";
    case State::kRunning:
      return "running";
    case State::kPausedByUser:
      return "paused";
    case State::kPausedForObs:
      return "paused while OBS is open";
    case State::kNeedsSetup:
      return "needs setup";
    case State::kMicMissing:
      return "mic missing";
    case State::kCableMissing:
      return "cable missing";
    case State::kObsMissing:
      return "OBS missing";
    case State::kObsUnsupported:
      return "OBS unsupported";
    case State::kRestartNeeded:
      return "restart needed";
    case State::kFailed:
      return "failed";
  }
  return "unknown";
}

std::string FormatChain(const ChainSummary& chain, bool short_form) {
  std::string text = chain.mic;
  if (short_form) {
    if (!chain.filters.empty()) {
      text += std::format("{}{} filter{}", kSeparator, chain.filters.size(), chain.filters.size() == 1 ? "" : "s");
    }
  } else {
    for (const std::string& filter : chain.filters) text += std::format("{}{}", kSeparator, filter);
  }
  if (!chain.cable.empty()) text += std::format("{}{}", kSeparator, chain.cable);
  return text;
}

std::string DescribeSnapshot(const Snapshot& snapshot) {
  std::string text(StateName(snapshot.state));
  if (!snapshot.detail.empty()) {
    text += ": " + snapshot.detail;
  } else if (snapshot.chain) {
    text += ": " + FormatChain(*snapshot.chain, false);
  }
  return text;
}

}  // namespace knobs::core
