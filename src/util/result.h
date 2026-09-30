// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace knobs {

// A failure described in a complete sentence, ready to show to the user.
struct Error {
  std::string message;
};

// Value or Error. A small stand-in for C++23's std::expected.
template <typename T>
class Result {
 public:
  Result(T value) : state_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : state_(std::in_place_index<1>, std::move(error)) {}

  bool ok() const { return state_.index() == 0; }
  explicit operator bool() const { return ok(); }

  T& value() & { return std::get<0>(state_); }
  const T& value() const& { return std::get<0>(state_); }
  T&& value() && { return std::get<0>(std::move(state_)); }
  T& operator*() & { return value(); }
  const T& operator*() const& { return value(); }
  T* operator->() { return &value(); }
  const T* operator->() const { return &value(); }

  const std::string& error() const { return std::get<1>(state_).message; }

 private:
  std::variant<T, Error> state_;
};

struct Ok {};
using Status = Result<Ok>;

}  // namespace knobs
