// SPDX-License-Identifier: GPL-2.0-or-later
#include "core/obs_process.h"

#include <windows.h>
#include <winternl.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstddef>
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

// The processes named `exe`, in every session. Another user's can't be
// opened from this session. It reads the process list as the kernel reports
// it, the call CreateToolhelp32Snapshot makes, without the snapshot's copy of
// it: about a millisecond for 250 processes, a third of a snapshot read in
// full. nullopt if it fails.
std::optional<std::vector<ObsProcess>> ProcessesNamed(std::wstring_view exe, std::vector<std::byte>& buffer) {
  static const auto query = reinterpret_cast<QuerySystemInformation>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  if (!query) return std::nullopt;
  if (buffer.empty()) buffer.resize(512 * 1024);
  ULONG needed = 0;
  NTSTATUS status;
  while ((status = query(SystemProcessInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &needed)) ==
         kInfoLengthMismatch) {
    buffer.resize(std::max<size_t>(buffer.size() * 2, needed + 64 * 1024));
  }
  if (status < 0) return std::nullopt;
  std::vector<ObsProcess> found;
  for (size_t offset = 0;;) {
    const auto* process = reinterpret_cast<const SYSTEM_PROCESS_INFORMATION*>(buffer.data() + offset);
    const std::wstring_view name(process->ImageName.Buffer ? process->ImageName.Buffer : L"",
                                 process->ImageName.Length / sizeof(wchar_t));
    if (name.size() == exe.size() && _wcsnicmp(name.data(), exe.data(), exe.size()) == 0) {
      found.push_back({static_cast<DWORD>(reinterpret_cast<uintptr_t>(process->UniqueProcessId)),
                       static_cast<DWORD>(process->SessionId)});
    }
    if (process->NextEntryOffset == 0) break;
    offset += process->NextEntryOffset;
  }
  return found;
}

// The account signed in to Windows session `session`, as DOMAIN\name, or "".
std::string AccountIn(DWORD session) {
  const auto query = [session](WTS_INFO_CLASS what) {
    LPWSTR text = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, what, &text, &bytes)) return std::string();
    std::string value = text ? ToUtf8(std::wstring_view(text)) : "";
    WTSFreeMemory(text);
    return value;
  };
  const std::string name = query(WTSUserName);
  return name.empty() ? "" : query(WTSDomainName) + "\\" + name;
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

ObsWatch::ObsWatch(ObsNames names, System system) : names_(std::move(names)), system_(std::move(system)) {
  if (!system_.processes) {
    // The buffer for the process list is kept between scans.
    system_.processes = [buffer = std::vector<std::byte>()](std::wstring_view exe) mutable {
      return ProcessesNamed(exe, buffer);
    };
  }
  if (!system_.account) system_.account = [](unsigned long session) { return AccountIn(session); };
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  session_ = session;
  account_ = system_.account(session_);
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
  const std::optional<std::vector<ObsProcess>> found = system_.processes(names_.exe);
  // A list that can't be read says nothing: what the last scan found stands.
  if (!found) return;
  unopened_ = false;
  std::vector<unsigned long> sessions;
  for (const ObsProcess& process : *found) {
    if (process.session != session_) {
      // Session 0 runs services: no one signs in to it.
      if (process.session != 0 && std::find(sessions.begin(), sessions.end(), process.session) == sessions.end()) {
        sessions.push_back(process.session);
      }
      continue;
    }
    if (std::find(process_ids_.begin(), process_ids_.end(), process.id) != process_ids_.end()) continue;
    if (const HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, process.id)) {
      processes_.push_back(handle);
      process_ids_.push_back(process.id);
    } else {
      unopened_ = true;  // Still this session's OBS: it counts until a scan doesn't find it.
    }
  }
  std::sort(sessions.begin(), sessions.end());
  if (sessions == other_sessions_) return;
  other_sessions_ = std::move(sessions);
  others_.clear();
  for (const unsigned long session : other_sessions_) {
    const std::string account = system_.account(session);
    others_.push_back({.session = static_cast<uint32_t>(session),
                       .account = account.substr(account.rfind('\\') + 1),  // Without its domain.
                       .yours = !account.empty() && account == account_});
  }
}

}  // namespace knobs::core
