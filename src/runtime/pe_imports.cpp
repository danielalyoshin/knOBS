// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/pe_imports.h"

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include "util/win_strings.h"

namespace knobs::runtime {
namespace {

// Bounds-checked view of a PE file's bytes.
class PeFile {
 public:
  explicit PeFile(std::span<const std::byte> bytes) : bytes_(bytes) {}

  // Reads the headers. Returns false if this isn't a 64-bit PE file.
  bool Parse() {
    const auto* dos = At<IMAGE_DOS_HEADER>(0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) return false;
    nt_ = At<IMAGE_NT_HEADERS64>(static_cast<uint64_t>(dos->e_lfanew));
    if (!nt_ || nt_->Signature != IMAGE_NT_SIGNATURE ||
        nt_->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
      return false;
    }
    const uint64_t sections_offset = static_cast<uint64_t>(dos->e_lfanew) +
                                     offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                     nt_->FileHeader.SizeOfOptionalHeader;
    const uint64_t count = nt_->FileHeader.NumberOfSections;
    if (sections_offset > bytes_.size() ||
        (bytes_.size() - sections_offset) / sizeof(IMAGE_SECTION_HEADER) < count) {
      return false;
    }
    sections_ = {reinterpret_cast<const IMAGE_SECTION_HEADER*>(bytes_.data() + sections_offset),
                 static_cast<size_t>(count)};
    return true;
  }

  const IMAGE_DATA_DIRECTORY* Directory(unsigned index) const {
    const auto& optional = nt_->OptionalHeader;
    if (index >= optional.NumberOfRvaAndSizes) return nullptr;
    const IMAGE_DATA_DIRECTORY& dir = optional.DataDirectory[index];
    return dir.VirtualAddress != 0 && dir.Size != 0 ? &dir : nullptr;
  }

  template <typename T>
  const T* AtRva(uint32_t rva) const {
    const auto offset = RvaToOffset(rva);
    return offset ? At<T>(*offset) : nullptr;
  }

  std::optional<std::string_view> StringAtRva(uint32_t rva) const {
    const auto offset = RvaToOffset(rva);
    if (!offset || *offset >= bytes_.size()) return std::nullopt;
    const auto* begin = reinterpret_cast<const char*>(bytes_.data() + *offset);
    const size_t limit = std::min<size_t>(bytes_.size() - *offset, MAX_PATH);
    const size_t length = strnlen(begin, limit);
    if (length == 0 || length == limit) return std::nullopt;
    return std::string_view(begin, length);
  }

 private:
  template <typename T>
  const T* At(uint64_t offset) const {
    if (offset > bytes_.size() || bytes_.size() - offset < sizeof(T)) return nullptr;
    return reinterpret_cast<const T*>(bytes_.data() + offset);
  }

  std::optional<uint64_t> RvaToOffset(uint32_t rva) const {
    for (const IMAGE_SECTION_HEADER& section : sections_) {
      const uint32_t start = section.VirtualAddress;
      const uint32_t extent = std::max(section.Misc.VirtualSize, section.SizeOfRawData);
      if (rva >= start && rva - start < extent) {
        const uint32_t delta = rva - start;
        if (delta >= section.SizeOfRawData) return std::nullopt;  // Zero-fill, not in the file.
        return uint64_t{section.PointerToRawData} + delta;
      }
    }
    if (rva < nt_->OptionalHeader.SizeOfHeaders) return rva;
    return std::nullopt;
  }

  std::span<const std::byte> bytes_;
  const IMAGE_NT_HEADERS64* nt_ = nullptr;
  std::span<const IMAGE_SECTION_HEADER> sections_;
};

// Descriptor tables end with an all-zero entry; the cap guards against
// malformed files that don't.
constexpr size_t kMaxDescriptors = 4096;

template <typename Descriptor, typename NameRva>
bool ReadDescriptorNames(const PeFile& pe, unsigned directory, NameRva name_rva,
                         std::vector<std::string>& names) {
  const IMAGE_DATA_DIRECTORY* dir = pe.Directory(directory);
  if (!dir) return true;
  for (size_t i = 0; i < kMaxDescriptors; ++i) {
    const auto* descriptor =
        pe.AtRva<Descriptor>(dir->VirtualAddress + static_cast<uint32_t>(i * sizeof(Descriptor)));
    if (!descriptor) return false;
    const uint32_t rva = name_rva(*descriptor);
    if (rva == 0) return true;
    const auto name = pe.StringAtRva(rva);
    if (!name) return false;
    names.emplace_back(*name);
  }
  return false;
}

struct HandleCloser {
  void operator()(HANDLE handle) const { CloseHandle(handle); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct ViewUnmapper {
  void operator()(const void* view) const { UnmapViewOfFile(view); }
};

}  // namespace

Result<std::vector<std::string>> ReadDllImports(const std::filesystem::path& file) {
  const std::string name = ToUtf8(file);
  HANDLE raw = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (raw == INVALID_HANDLE_VALUE) {
    return Error{std::format("Couldn't open {}: {}", name, DescribeWinError(GetLastError()))};
  }
  UniqueHandle handle(raw);

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(handle.get(), &size) || size.QuadPart == 0) {
    return Error{std::format("{} is empty or unreadable.", name)};
  }
  UniqueHandle mapping(CreateFileMappingW(handle.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
  if (!mapping) {
    return Error{std::format("Couldn't map {}: {}", name, DescribeWinError(GetLastError()))};
  }
  std::unique_ptr<const void, ViewUnmapper> view(
      MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0));
  if (!view) {
    return Error{std::format("Couldn't map {}: {}", name, DescribeWinError(GetLastError()))};
  }

  PeFile pe({static_cast<const std::byte*>(view.get()), static_cast<size_t>(size.QuadPart)});
  if (!pe.Parse()) return Error{std::format("{} isn't a 64-bit Windows binary.", name)};

  std::vector<std::string> names;
  const bool ok =
      ReadDescriptorNames<IMAGE_IMPORT_DESCRIPTOR>(
          pe, IMAGE_DIRECTORY_ENTRY_IMPORT,
          [](const IMAGE_IMPORT_DESCRIPTOR& d) { return d.Name; }, names) &&
      ReadDescriptorNames<IMAGE_DELAYLOAD_DESCRIPTOR>(
          pe, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT,
          [](const IMAGE_DELAYLOAD_DESCRIPTOR& d) { return d.DllNameRVA; }, names);
  if (!ok) return Error{std::format("{} has a malformed import table.", name)};
  return names;
}

}  // namespace knobs::runtime
