// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/first_run_dialog.h"

#include <windows.h>
#include <shellapi.h>

#include <format>
#include <utility>

#include "app_info.h"
#include "import/obs_config.h"
#include "runtime/obs_install.h"
#include "tray/resource.h"
#include "util/pick_folder.h"
#include "util/win_strings.h"

namespace knobs::tray {
namespace {

// In dialog units: the same for every page, so the window keeps its width as
// it moves between them.
constexpr UINT kWidthDlu = 320;

HICON LoadKnobIcon(HINSTANCE instance, int size) {
  HICON icon = nullptr;
  if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_KNOBS), size, &icon))) return nullptr;
  return icon;
}

}  // namespace

FirstRunDialog::FirstRunDialog(FirstRunHost& host, HINSTANCE instance, FirstRun first_run)
    : host_(host), instance_(instance), first_run_(std::move(first_run)), saved_(first_run_.progress()) {
  icon_large_ = LoadKnobIcon(instance_, LIM_LARGE);
  icon_small_ = LoadKnobIcon(instance_, LIM_SMALL);
}

FirstRunDialog::~FirstRunDialog() {
  if (icon_large_) DestroyIcon(icon_large_);
  if (icon_small_) DestroyIcon(icon_small_);
}

void FirstRunDialog::Show() {
  // What the first run starts from, such as a page asked for.
  host_.SaveFirstRun(saved_);
  shown_ = first_run_.View(host_.snapshot());
  Config& config = Fill(shown_);
  // Without somewhere to put it, the check box is disabled. Its state comes
  // from TDN_VERIFICATION_CLICKED instead.
  BOOL checked = FALSE;
  TaskDialogIndirect(&config.config, nullptr, nullptr, &checked);
  window_ = nullptr;
}

void FirstRunDialog::Changed(const core::Snapshot& before) {
  if (in_button_) {
    if (!changed_from_) changed_from_ = before;
    render_due_ = true;
    return;
  }
  const FirstRunAction action = first_run_.Changed(before, host_.snapshot());
  Save();
  if (action.kind == FirstRunAction::Kind::kApply) Apply(action.settings);
  Render();
}

HRESULT CALLBACK FirstRunDialog::Callback(HWND window, UINT notification, WPARAM wparam, LPARAM lparam,
                                          LONG_PTR data) {
  auto* dialog = reinterpret_cast<FirstRunDialog*>(data);
  switch (notification) {
    case TDN_CREATED:
      dialog->window_ = window;
      // For the title bar, the taskbar and Alt+Tab.
      SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(dialog->icon_large_));
      SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(dialog->icon_small_));
      SetForegroundWindow(window);
      dialog->ApplyStates();
      // Snapshots that came before the window did.
      dialog->Render();
      break;
    case TDN_NAVIGATED:
      dialog->ApplyStates();
      break;
    case TDN_BUTTON_CLICKED:
      return dialog->OnButton(static_cast<int>(wparam));
    case TDN_RADIO_BUTTON_CLICKED:
      dialog->choice_ = static_cast<int>(wparam);
      break;
    case TDN_VERIFICATION_CLICKED:
      dialog->checked_ = wparam != 0;
      break;
    case TDN_HYPERLINK_CLICKED:
      ShellExecuteW(window, L"open", reinterpret_cast<const wchar_t*>(lparam), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    case TDN_TIMER:
      // Every 200 ms or so: catches a render put off by a button's handler.
      if (dialog->render_due_ && !dialog->in_button_) dialog->Render();
      break;
    case TDN_DESTROYED:
      dialog->window_ = nullptr;
      break;
    default:
      break;
  }
  return S_OK;
}

HRESULT FirstRunDialog::OnButton(int id) {
  // The close button, Esc and Alt+F4: knobs stays in the tray, and the first
  // run resumes here next time.
  if (id == IDCANCEL) return S_OK;
  in_button_ = true;
  const core::Snapshot snapshot = host_.snapshot();
  const FirstRunAction action = first_run_.Click(id, choice_, checked_, snapshot);
  Save();
  bool close = false;
  switch (action.kind) {
    case FirstRunAction::Kind::kNone:
      break;
    case FirstRunAction::Kind::kApply:
      Apply(action.settings);
      break;
    case FirstRunAction::Kind::kReimport:
      host_.Reimport();
      break;
    case FirstRunAction::Kind::kOpenObs:
      if (const Status opened = host_.StartObs(snapshot.obs.root); !opened) ShowError(opened.error());
      break;
    case FirstRunAction::Kind::kChooseObs:
      if (const auto folder = runtime::PickObsInstallFolder(window_)) {
        core::Settings settings = snapshot.settings;
        settings.obs_dir = *folder;
        Apply(settings);
      }
      break;
    case FirstRunAction::Kind::kChooseObsSettings:
      if (const auto folder = PickFolder(window_, L"Choose OBS's settings folder")) {
        core::Settings settings = snapshot.settings;
        settings.obs_config = import::SettingsFolderFor(*folder);
        Apply(settings);
      }
      break;
    case FirstRunAction::Kind::kRestart:
      // The tray quits, which ends the dialog's loop too.
      host_.Restart();
      close = true;
      break;
    case FirstRunAction::Kind::kFinish:
      host_.FinishFirstRun(action.start_with_windows);
      close = true;
      break;
  }
  in_button_ = false;
  if (close) return S_OK;
  if (changed_from_) {
    const core::Snapshot before = std::move(*changed_from_);
    changed_from_.reset();
    Changed(before);
  } else {
    Render();
  }
  return S_FALSE;
}

void FirstRunDialog::Render() {
  if (!window_ || in_button_) {
    render_due_ = true;
    return;
  }
  render_due_ = false;
  const core::Snapshot& snapshot = host_.snapshot();
  PageView view;
  if (pending_ && snapshot.settings != *pending_) {
    // Until the core has the new settings, the page is as it was, with
    // nothing to click.
    view = shown_;
    for (PageButton& button : view.buttons) button.enabled = false;
    for (PageButton& choice : view.choices) choice.enabled = false;
    view.waiting = true;
  } else {
    pending_.reset();
    view = first_run_.View(snapshot, 0);
    if (view.page == shown_.page) view = first_run_.View(snapshot, choice_, checked_);
  }
  if (view == shown_) return;
  shown_ = std::move(view);
  Config& config = Fill(shown_);
  SendMessageW(window_, TDM_NAVIGATE_PAGE, 0, reinterpret_cast<LPARAM>(&config.config));
}

FirstRunDialog::Config& FirstRunDialog::Fill(const PageView& view) {
  Config& next = configs_[next_config_];
  next_config_ = (next_config_ + 1) % configs_.size();
  next = Config{};
  next.title = std::format(L"{} setup", kDisplayNameW);
  next.instruction = FromUtf8(view.instruction);
  next.content = FromUtf8(view.content);
  next.footer = FromUtf8(view.footer);
  next.check = FromUtf8(view.check);
  for (const PageButton& button : view.buttons) next.button_texts.push_back(FromUtf8(button.text));
  for (const PageButton& choice : view.choices) next.choice_texts.push_back(FromUtf8(choice.text));
  for (size_t i = 0; i < view.buttons.size(); ++i) {
    next.buttons.push_back({view.buttons[i].id, next.button_texts[i].c_str()});
  }
  for (size_t i = 0; i < view.choices.size(); ++i) {
    next.choices.push_back({view.choices[i].id, next.choice_texts[i].c_str()});
  }

  TASKDIALOGCONFIG& config = next.config;
  config.cbSize = sizeof(config);
  config.hInstance = instance_;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_ENABLE_HYPERLINKS | TDF_CALLBACK_TIMER;
  if (view.command_links) config.dwFlags |= TDF_USE_COMMAND_LINKS;
  if (view.waiting) config.dwFlags |= TDF_SHOW_MARQUEE_PROGRESS_BAR;
  if (!view.check.empty() && view.checked) config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
  config.pszWindowTitle = next.title.c_str();
  switch (view.icon) {
    case PageIcon::kKnob:
      if (icon_large_) {
        config.dwFlags |= TDF_USE_HICON_MAIN;
        config.hMainIcon = icon_large_;
      }
      break;
    case PageIcon::kInformation:
      config.pszMainIcon = TD_INFORMATION_ICON;
      break;
    case PageIcon::kWarning:
      config.pszMainIcon = TD_WARNING_ICON;
      break;
    case PageIcon::kError:
      config.pszMainIcon = TD_ERROR_ICON;
      break;
  }
  if (!next.instruction.empty()) config.pszMainInstruction = next.instruction.c_str();
  if (!next.content.empty()) config.pszContent = next.content.c_str();
  if (!next.footer.empty()) config.pszFooter = next.footer.c_str();
  if (!next.check.empty()) config.pszVerificationText = next.check.c_str();
  config.cButtons = static_cast<UINT>(next.buttons.size());
  config.pButtons = next.buttons.empty() ? nullptr : next.buttons.data();
  config.nDefaultButton = view.default_button;
  config.cRadioButtons = static_cast<UINT>(next.choices.size());
  config.pRadioButtons = next.choices.empty() ? nullptr : next.choices.data();
  config.nDefaultRadioButton = view.default_choice;
  config.pfCallback = Callback;
  config.lpCallbackData = reinterpret_cast<LONG_PTR>(this);
  config.cxWidth = kWidthDlu;
  return next;
}

void FirstRunDialog::ApplyStates() {
  for (const PageButton& button : shown_.buttons) {
    if (!button.enabled) SendMessageW(window_, TDM_ENABLE_BUTTON, button.id, FALSE);
  }
  for (const PageButton& choice : shown_.choices) {
    if (!choice.enabled) SendMessageW(window_, TDM_ENABLE_RADIO_BUTTON, choice.id, FALSE);
  }
  if (shown_.waiting) SendMessageW(window_, TDM_SET_PROGRESS_BAR_MARQUEE, TRUE, 0);
  choice_ = shown_.default_choice;
  checked_ = shown_.checked;
}

void FirstRunDialog::Apply(const core::Settings& settings) {
  host_.ApplySettings(settings);
  pending_ = settings;
}

void FirstRunDialog::Save() {
  if (first_run_.progress() == saved_) return;
  saved_ = first_run_.progress();
  host_.SaveFirstRun(saved_);
}

void FirstRunDialog::ShowError(const std::string& text) {
  const std::wstring title = std::format(L"{} setup", kDisplayNameW);
  const std::wstring content = FromUtf8(text);
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hwndParent = window_;
  config.hInstance = instance_;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
  config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  config.pszWindowTitle = title.c_str();
  config.pszMainIcon = TD_WARNING_ICON;
  config.pszContent = content.c_str();
  TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

}  // namespace knobs::tray
