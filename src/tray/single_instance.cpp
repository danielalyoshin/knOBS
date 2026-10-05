// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/single_instance.h"

#include <windows.h>

#include <algorithm>
#include <format>

#include "util/win_strings.h"

namespace knobs::tray {

Result<std::unique_ptr<SingleInstance>> SingleInstance::Acquire(const std::wstring& name,
                                                                std::chrono::milliseconds wait) {
  const std::wstring full_name = L"Local\\" + name;
  const HANDLE mutex = CreateMutexW(nullptr, FALSE, full_name.c_str());
  if (!mutex) {
    return Error{std::format("Couldn't check whether another copy is running: {}", DescribeWinError(GetLastError()))};
  }
  const auto milliseconds = std::clamp<long long>(wait.count(), 0, INFINITE - 1);
  const DWORD result = WaitForSingleObject(mutex, static_cast<DWORD>(milliseconds));
  if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
    return std::unique_ptr<SingleInstance>(new SingleInstance(mutex));
  }
  CloseHandle(mutex);
  if (result == WAIT_TIMEOUT) return std::unique_ptr<SingleInstance>();
  return Error{std::format("Couldn't check whether another copy is running: {}", DescribeWinError(GetLastError()))};
}

SingleInstance::~SingleInstance() {
  ReleaseMutex(mutex_);
  CloseHandle(mutex_);
}

}  // namespace knobs::tray
