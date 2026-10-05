// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/obs_process.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string_view>

#include "runtime/obs_layout.h"

namespace knobs::core {
namespace {

// The mutex an installed OBS creates. A portable one adds its settings
// folder to "OBSStudioPortable" instead.
constexpr wchar_t kObsMutex[] = L"OBSStudioCore";
constexpr std::chrono::seconds kScanInterval{2};

using QuerySystemInformation = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
constexpr NTSTATUS kInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004);

// The process list as the kernel reports it, the call CreateToolhelp32Snapshot
// makes, without the snapshot's copy of it: about a millisecond for 250
// processes, a third of a snapshot read in full. Empty if it fails.
std::vector<DWORD> ProcessesNamed(std::wstring_view exe, std::vector<std::byte>& buffer) {
  static const auto query = reinterpret_cast<QuerySystemInformation>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  std::vector<DWORD> found;
  if (!query) return found;
  if (buffer.empty()) buffer.resize(512 * 1024);
  ULONG needed = 0;
  NTSTATUS status;
  while ((status = query(SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed)) ==
         kInfoLengthMismatch) {
    buffer.resize(std::max<size_t>(buffer.size() * 2, needed + 64 * 1024));
  }
  if (status < 0) return found;
  for (size_t offset = 0;;) {
    const auto* process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
    const std::wstring_view name(process->ImageName.Buffer ? process->ImageName.Buffer : L"",
                                 process->ImageName.Length / sizeof(wchar_t));
    if (name.size() == exe.size() && _wcsnicmp(name.data(), exe.data(), exe.size()) == 0) {
      found.push_back(static_cast<DWORD>(reinterpret_cast<uintptr_t>(process->UniqueProcessId)));
    }
    if (process->NextEntryOffset == 0) break;
    offset += process->NextEntryOffset;
  }
  return found;
}

bool ObsMutexExists() {
  const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kObsMutex);
  if (mutex) {
    CloseHandle(mutex);
    return true;
  }
  // An OBS running as administrator makes a mutex others can't open.
  return GetLastError() == ERROR_ACCESS_DENIED;
}

}  // namespace

ObsWatch::~ObsWatch() {
  for (void* process : processes_) CloseHandle(process);
}

bool ObsWatch::Running() {
  for (size_t i = processes_.size(); i-- > 0;) {
    if (WaitForSingleObject(processes_[i], 0) != WAIT_OBJECT_0) continue;
    CloseHandle(processes_[i]);
    processes_.erase(processes_.begin() + static_cast<std::ptrdiff_t>(i));
    process_ids_.erase(process_ids_.begin() + static_cast<std::ptrdiff_t>(i));
  }
  const bool mutex = ObsMutexExists();
  const auto now = std::chrono::steady_clock::now();
  if (now >= next_scan_ || (mutex && !had_mutex_)) Scan(now);
  had_mutex_ = mutex;
  return mutex || !processes_.empty() || unopened_;
}

void ObsWatch::Scan(std::chrono::steady_clock::time_point now) {
  next_scan_ = now + kScanInterval;
  unopened_ = false;
  for (const DWORD id : ProcessesNamed(runtime::kObsExe, buffer_)) {
    if (std::find(process_ids_.begin(), process_ids_.end(), id) != process_ids_.end()) continue;
    if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, id)) {
      processes_.push_back(process);
      process_ids_.push_back(id);
    } else {
      unopened_ = true;
    }
  }
}

}  // namespace knobs::core
