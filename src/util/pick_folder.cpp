// SPDX-License-Identifier: GPL-2.0-or-later
#include "util/pick_folder.h"

#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <string>

namespace knobs {

std::optional<std::filesystem::path> PickFolder(void* owner, std::wstring_view title) {
  using Microsoft::WRL::ComPtr;
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  std::optional<std::filesystem::path> picked;
  {
    ComPtr<IFileOpenDialog> dialog;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
      FILEOPENDIALOGOPTIONS options = 0;
      dialog->GetOptions(&options);
      // FOS_NOCHANGEDIR: libobs resolves its data files against the working
      // directory, so the dialog mustn't move it.
      dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
      const std::wstring text(title);
      dialog->SetTitle(text.c_str());
      ComPtr<IShellItem> item;
      if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner))) && SUCCEEDED(dialog->GetResult(&item))) {
        wchar_t* path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) picked = path;
        CoTaskMemFree(path);
      }
    }
  }
  if (SUCCEEDED(com)) CoUninitialize();
  return picked;
}

}  // namespace knobs
