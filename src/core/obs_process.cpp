// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/obs_process.h"

#include <windows.h>
#include <winternl.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string_view>
#include <utility>

#include "util/win_strings.h"

namespace knobs::core {
namespace {

constexpr std::chrono::seconds kScanInterval{2};

using QuerySystemInformation = NTSTATUS(NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
constexpr NTSTATUS kInfoLengthMismatch = static_cast<NTSTATUS>(0xC0000004);

struct Found {
  std::vector<DWORD> here;            // Process IDs in this session.
  std::vector<DWORD> other_sessions;  // The other sessions they're in, sorted.
};

// The processes named `exe`: those in Windows session `session`, and the
// other sessions that have some. Another user's can't be opened from this
// session. It reads the process list as the kernel reports it, the call
// CreateToolhelp32Snapshot makes, without the snapshot's copy of it: about a
// millisecond for 250 processes, a third of a snapshot read in full. Empty if
// it fails.
Found ProcessesNamed(std::wstring_view exe, DWORD session, std::vector<std::byte>& buffer) {
  static const auto query = reinterpret_cast<QuerySystemInformation>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  Found found;
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
      const auto other = static_cast<DWORD>(process->SessionId);
      if (other == session) {
        found.here.push_back(static_cast<DWORD>(reinterpret_cast<uintptr_t>(process->UniqueProcessId)));
      } else if (std::find(found.other_sessions.begin(), found.other_sessions.end(), other) ==
                 found.other_sessions.end()) {
        found.other_sessions.push_back(other);
      }
    }
    if (process->NextEntryOffset == 0) break;
    offset += process->NextEntryOffset;
  }
  std::sort(found.other_sessions.begin(), found.other_sessions.end());
  return found;
}

// The name of the account signed in to Windows session `session`, or "".
std::string AccountIn(DWORD session) {
  LPWSTR name = nullptr;
  DWORD bytes = 0;
  if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &name, &bytes)) return "";
  std::string account = name ? ToUtf8(std::wstring_view(name)) : "";
  WTSFreeMemory(name);
  return account;
}

bool MutexExists(const std::wstring& name) {
  const HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
  if (mutex) {
    CloseHandle(mutex);
    return true;
  }
  // An OBS running as administrator makes a mutex others can't open.
  return GetLastError() == ERROR_ACCESS_DENIED;
}

}  // namespace

ObsWatch::ObsWatch(ObsNames names) : names_(std::move(names)) {
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  session_ = session;
}

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
  const bool mutex = MutexExists(names_.mutex);
  const auto now = std::chrono::steady_clock::now();
  if (now >= next_scan_ || (mutex && !had_mutex_)) Scan(now);
  had_mutex_ = mutex;
  return mutex || !processes_.empty() || unopened_;
}

void ObsWatch::Scan(std::chrono::steady_clock::time_point now) {
  next_scan_ = now + kScanInterval;
  unopened_ = false;
  const Found found = ProcessesNamed(names_.exe, session_, buffer_);
  if (found.other_sessions != other_sessions_) {
    other_sessions_ = found.other_sessions;
    other_accounts_.clear();
    for (const DWORD session : other_sessions_) other_accounts_.push_back(AccountIn(session));
  }
  for (const DWORD id : found.here) {
    if (std::find(process_ids_.begin(), process_ids_.end(), id) != process_ids_.end()) continue;
    if (const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, id)) {
      processes_.push_back(process);
      process_ids_.push_back(id);
    } else {
      unopened_ = true;  // Still this session's OBS: it counts until a scan doesn't find it.
    }
  }
}

}  // namespace knobs::core
