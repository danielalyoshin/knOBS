// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/state.h"

#include <algorithm>
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

std::string DescribeOtherObs(const std::vector<OtherObs>& others) {
  // Accounts whose names can't be read can't be told apart.
  std::vector<std::string_view> named;
  size_t unnamed = 0;
  size_t yours = 0;
  for (const OtherObs& other : others) {
    if (other.yours) {
      ++yours;
    } else if (other.account.empty()) {
      ++unnamed;
    } else if (std::find(named.begin(), named.end(), other.account) == named.end()) {
      named.push_back(other.account);
    }
  }
  const size_t accounts = named.size() + unnamed;
  if (yours > 0) {
    if (accounts > 0) return std::format("{} other Windows sessions", others.size());
    return yours == 1 ? "your other session" : "your other sessions";
  }
  if (accounts != 1) return std::format("{} other Windows accounts", accounts);
  return named.empty() ? "another Windows account" : std::format("{}'s account", named.front());
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
