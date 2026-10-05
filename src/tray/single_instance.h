// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "util/result.h"

namespace knobs::tray {

// One copy of knobs per Windows session: the running copy owns a named
// mutex. Windows hands an abandoned mutex to the next waiter, so a copy that
// crashed doesn't block the next one.
class SingleInstance {
 public:
  // Takes the mutex `name` (in the session's Local\ namespace), waiting up
  // to `wait` for a copy that's quitting, as on a restart. A null pointer
  // means another copy holds it.
  static Result<std::unique_ptr<SingleInstance>> Acquire(const std::wstring& name, std::chrono::milliseconds wait);
  // Releases the mutex. Destroy it on the thread that acquired it.
  ~SingleInstance();
  SingleInstance(const SingleInstance&) = delete;
  SingleInstance& operator=(const SingleInstance&) = delete;

 private:
  explicit SingleInstance(void* mutex) : mutex_(mutex) {}

  void* mutex_;  // HANDLE
};

}  // namespace knobs::tray
