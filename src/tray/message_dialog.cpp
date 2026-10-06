// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/message_dialog.h"

#include <commctrl.h>

#include "app_info.h"
#include "tray/resource.h"

namespace knobs::tray {

HICON LoadAppIcon(HINSTANCE instance, int size) {
  HICON icon = nullptr;
  if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_KNOBS), size, &icon))) return nullptr;
  return icon;
}

void ShowMessageDialog(HINSTANCE instance, const MessageDialog& dialog) {
  const std::wstring title = dialog.title.empty() ? std::wstring(kDisplayNameW) : dialog.title;
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hwndParent = dialog.owner;
  config.hInstance = instance;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  if (dialog.owner) config.dwFlags |= TDF_POSITION_RELATIVE_TO_WINDOW;
  config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  config.pszWindowTitle = title.c_str();
  if (dialog.custom_icon) {
    config.dwFlags |= TDF_USE_HICON_MAIN;
    config.hMainIcon = dialog.custom_icon;
  } else {
    config.pszMainIcon = dialog.icon;
  }
  if (!dialog.instruction.empty()) config.pszMainInstruction = dialog.instruction.c_str();
  if (!dialog.content.empty()) config.pszContent = dialog.content.c_str();
  if (!dialog.footer.empty()) config.pszFooter = dialog.footer.c_str();
  TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

}  // namespace knobs::tray
