#include "MainWindow.h"
#include "Screenshot.h"
#include "Theme.h"
#include <uxtheme.h>
#include <dwmapi.h>
#include <filesystem>
#include <algorithm>
#include <commctrl.h>
#include <shobjidl.h>
#include <sstream>
#include <windowsx.h>
namespace gdv
{
namespace
{
enum
{
  Refresh = 102,
  Source,
  View,
  Base,
  Target,
  Compare,
  Files,
  Commits,
  Toggle,
  Screenshot,
  ZoomIn,
  ZoomOut,
  NextFile,
  PreviousFile,
  Theme
};
constexpr auto settingsKey = L"Software\\gaijin\\git_diff_viewer";
constexpr int minFilePaneWidth = 220;
constexpr int minDiffPaneWidth = 300;
DWORD readSetting(const wchar_t *name, DWORD fallback)
{
  DWORD value = fallback, size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER, settingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size);
  return value;
}
std::wstring readString(const wchar_t *name)
{
  wchar_t value[4096]{};
  DWORD size = sizeof(value);
  if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, name, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS)
    return {};
  return value;
}
std::wstring getText(HWND h)
{
  int n = GetWindowTextLengthW(h);
  std::wstring s(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}
ChangeSource source(HWND h) { return static_cast<ChangeSource>(SendMessageW(h, CB_GETCURSEL, 0, 0)); }
} // namespace
MainWindow::~MainWindow()
{
  if (controller_)
    controller_->shutdown();
  if (font_)
    DeleteObject(font_);
  if (backgroundBrush_)
    DeleteObject(backgroundBrush_);
  if (fieldBrush_)
    DeleteObject(fieldBrush_);
}
int MainWindow::run(HINSTANCE instance, int show, std::wstring directory, std::wstring automationDirectory)
{
  instance_ = instance;
  directory_ = std::move(directory);
  automationDirectory_ = std::move(automationDirectory);
  side_ = automationDirectory_.empty() && readSetting(L"SideBySide", 0) != 0;
  darkTheme = !automationDirectory_.empty() || readSetting(L"DarkTheme", 1) != 0;
  filePaneWidth_ = automationDirectory_.empty() ? static_cast<int>(std::min<DWORD>(readSetting(L"FilePaneWidth", 0), 4096)) : 0;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.hInstance = instance;
  wc.lpfnWndProc = procedure;
  wc.lpszClassName = L"GitDiffViewer.Main";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = static_cast<HICON>(
    LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
  wc.hIconSm = static_cast<HICON>(
    LoadImageW(instance, MAKEINTRESOURCEW(101), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"GitDiffViewer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
    CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820, nullptr, nullptr, instance, this);
  if (!hwnd_)
    return 1;
  if (automationDirectory_.empty())
  {
    WINDOWPLACEMENT placement{sizeof(placement)};
    DWORD bytes = sizeof(placement);
    if (RegGetValueW(HKEY_CURRENT_USER, settingsKey, L"WindowPlacement", RRF_RT_REG_BINARY, nullptr, &placement, &bytes) ==
          ERROR_SUCCESS &&
        bytes == sizeof(placement))
    {
      placement.length = sizeof(placement);
      if (placement.showCmd != SW_SHOWMAXIMIZED)
        placement.showCmd = SW_SHOWNORMAL;
      if (MonitorFromRect(&placement.rcNormalPosition, MONITOR_DEFAULTTONULL))
      {
        SetWindowPlacement(hwnd_, &placement);
        show = placement.showCmd;
      }
    }
  }
  ShowWindow(hwnd_, show == SW_HIDE ? SW_SHOWNORMAL : show);
  UpdateWindow(hwnd_);
  if (!automationDirectory_.empty())
    SetTimer(hwnd_, 1, 100, nullptr);
  ACCEL entries[] = {{FVIRTKEY | FCONTROL, 'R', Refresh}, {FVIRTKEY, VK_F5, Refresh}, {FVIRTKEY | FCONTROL | FSHIFT, 'D', Toggle},
    {FVIRTKEY | FCONTROL | FSHIFT, 'S', Screenshot}, {FVIRTKEY | FCONTROL, VK_OEM_PLUS, ZoomIn},
    {FVIRTKEY | FCONTROL, VK_OEM_MINUS, ZoomOut}, {FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_PLUS, ZoomIn},
    {FVIRTKEY | FCONTROL, VK_DOWN, NextFile}, {FVIRTKEY | FCONTROL, VK_UP, PreviousFile}, {FVIRTKEY | FCONTROL, VK_ADD, ZoomIn},
    {FVIRTKEY | FCONTROL, VK_SUBTRACT, ZoomOut}};
  HACCEL accel = CreateAcceleratorTableW(entries, static_cast<int>(std::size(entries)));
  MSG msg{};
  int result = 0;
  while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0)
  {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE &&
        (GetFocus() == diff_.handle() || GetFocus() == files_ || GetFocus() == commits_) &&
        !SendMessageW(commits_, CB_GETDROPPEDSTATE, 0, 0))
    {
      if (!(msg.lParam & (1LL << 30)))
        toggleCommitMessage();
      continue;
    }
    if (!TranslateAcceleratorW(hwnd_, accel, &msg) && !IsDialogMessageW(hwnd_, &msg))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  DestroyAcceleratorTable(accel);
  return result < 0 ? 1 : static_cast<int>(msg.wParam);
}
HWND MainWindow::control(const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
  return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 100, 26, hwnd_,
    reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
}
void MainWindow::createControls()
{
  refresh_ = control(L"BUTTON", L"Refresh", WS_TABSTOP, Refresh);
  info_ = control(L"STATIC", L"No repository open", SS_ENDELLIPSIS | SS_CENTERIMAGE | SS_NOPREFIX, 0);
  source_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, Source);
  for (auto name : {L"Staged", L"Unstaged", L"All local · HEAD", L"Ready to push", L"Single commit", L"Commit range"})
    SendMessageW(source_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
  SendMessageW(source_, CB_SETCURSEL, automationDirectory_.empty() ? std::min<DWORD>(5, readSetting(L"Source", 1)) : 1, 0);
  view_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, View);
  SendMessageW(view_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unified"));
  SendMessageW(view_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Side-by-side"));
  SendMessageW(view_, CB_SETCURSEL, side_ ? 1 : 0, 0);
  themeButton_ = control(L"BUTTON", L"Light theme", WS_TABSTOP, Theme);
  baseLabel_ = control(L"STATIC", L"Base branch / ref", 0, 0);
  base_ = control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, Base);
  targetLabel_ = control(L"STATIC", L"Target / commit", 0, 0);
  target_ = control(L"EDIT", L"HEAD", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, Target);
  compare_ = control(L"BUTTON", L"Compare", WS_TABSTOP, Compare);
  commitLabel_ = control(L"STATIC", L"LOCAL COMMITS", 0, 0);
  commits_ = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, Commits);
  for (HWND combo : {source_, view_, commits_})
    SetWindowSubclass(combo, comboProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  COMBOBOXINFO comboInfo{sizeof(comboInfo)};
  if (GetComboBoxInfo(commits_, &comboInfo))
  {
    commitPopup_ = comboInfo.hwndList;
    SetWindowSubclass(commitPopup_, commitListProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  }
  fileLabel_ = control(L"STATIC", L"CHANGED FILES", 0, 0);
  files_ = control(L"LISTBOX", L"",
    WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, Files);
  SetWindowSubclass(files_, filesProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd_,
    nullptr, instance_, nullptr);
  TOOLINFOW tool{sizeof(tool)};
  tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
  tool.hwnd = hwnd_;
  tool.uId = reinterpret_cast<UINT_PTR>(files_);
  tool.lpszText = const_cast<wchar_t *>(L"");
  SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
  tool.uId = reinterpret_cast<UINT_PTR>(info_);
  tool.lpszText = LPSTR_TEXTCALLBACKW;
  SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
  SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 1000);
  status_ = control(L"STATIC", L"Finding repository…", SS_LEFTNOWORDWRAP, 0);
  diff_.create(hwnd_, instance_);
  diff_.setSideBySide(side_);
  controller_ = std::make_unique<RepositoryController>(hwnd_);
  dpi_ = GetDpiForWindow(hwnd_);
  updateFonts();
  if (automationDirectory_.empty())
  {
    diff_.zoom(static_cast<int>(std::clamp<DWORD>(readSetting(L"FontSize", 11), 6, 40)) - diff_.fontSize());
    SetWindowTextW(base_, readString(L"Base").c_str());
    auto target = readString(L"Target");
    if (!target.empty())
      SetWindowTextW(target_, target.c_str());
    savedCommit_ = readString(L"Commit");
  }
  applyTheme();
  sourceChanged();
  if (!directory_.empty())
    refresh();
}
void MainWindow::updateFonts()
{
  if (font_)
    DeleteObject(font_);
  font_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  diff_.setDpi(dpi_);
  auto itemHeight = MulDiv(24, static_cast<int>(dpi_), 96);
  SendMessageW(commits_, CB_SETITEMHEIGHT, 0, itemHeight);
  SendMessageW(commits_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), itemHeight);
  SendMessageW(files_, LB_SETITEMHEIGHT, 0, itemHeight);
  // MoveWindow's height includes the dropdown, not the closed combobox field.
  // Native themed buttons have a one-pixel transparent inset. Match the visible
  // borders, not just the HWND rectangles, while accounting for the combo frame.
  const int toolbarHeight = MulDiv(30, static_cast<int>(dpi_), 96) - 2 * MulDiv(1, static_cast<int>(dpi_), 96);
  for (HWND combo : {source_, view_, commits_})
  {
    RECT bounds{};
    GetWindowRect(combo, &bounds);
    int selectionHeight = static_cast<int>(SendMessageW(combo, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
    int frameHeight = static_cast<int>(bounds.bottom - bounds.top) - selectionHeight;
    SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), std::max(1, toolbarHeight - frameHeight));
  }
  layout();
}
void MainWindow::layout()
{
  if (!refresh_)
    return;
  RECT r{};
  GetClientRect(hwnd_, &r);
  auto scale = [&](int n) { return MulDiv(n, static_cast<int>(dpi_), 96); };
  int width = r.right, height = r.bottom, pad = scale(12), gap = scale(8), row = scale(30);
  int maximumLeft = std::max(scale(minFilePaneWidth), width - pad - gap - scale(minDiffPaneWidth));
  int left = filePaneWidth_ > 0 ? std::clamp(scale(filePaneWidth_), scale(minFilePaneWidth), maximumLeft)
                                : std::clamp(width / 4, scale(minFilePaneWidth), scale(380));
  int footerHeight = scale(36);
  auto move = [&](HWND h, int x, int y, int w, int ht) { MoveWindow(h, x, y, std::max(1, w), std::max(1, ht), TRUE); };
  move(refresh_, pad, pad, scale(80), row);
  move(source_, pad + scale(92), pad + scale(1), scale(180), scale(240));
  move(view_, pad + scale(284), pad + scale(1), scale(145), scale(160));
  move(themeButton_, pad + scale(441), pad, scale(115), row);
  int infoX = pad + scale(568);
  move(info_, infoX, pad, width - infoX - pad, row);
  int y = pad + row + gap;
  auto mode = source(source_);
  bool fields = mode == ChangeSource::ReadyToPush || mode == ChangeSource::Commit || mode == ChangeSource::Range;
  bool baseVisible = mode == ChangeSource::ReadyToPush || mode == ChangeSource::Range,
       targetVisible = mode == ChangeSource::Commit || mode == ChangeSource::Range;
  for (auto h : {baseLabel_, base_})
    ShowWindow(h, baseVisible ? SW_SHOW : SW_HIDE);
  for (auto h : {targetLabel_, target_})
    ShowWindow(h, targetVisible ? SW_SHOW : SW_HIDE);
  ShowWindow(compare_, fields ? SW_SHOW : SW_HIDE);
  if (fields)
  {
    move(baseLabel_, pad, y + scale(5), scale(115), row);
    move(base_, pad + scale(118), y, scale(195), row);
    int tx = baseVisible ? pad + scale(325) : pad;
    move(targetLabel_, tx, y + scale(5), scale(110), row);
    move(target_, tx + scale(115), y, scale(165), row);
    move(compare_, targetVisible ? tx + scale(288) : pad + scale(325), y, scale(82), row);
    y += row + gap;
  }
  bool ready = mode == ChangeSource::ReadyToPush;
  ShowWindow(commits_, ready ? SW_SHOW : SW_HIDE);
  ShowWindow(commitLabel_, ready ? SW_SHOW : SW_HIDE);
  int fy = y;
  if (ready)
  {
    move(commitLabel_, pad, fy, left - pad, scale(22));
    fy += scale(24);
    move(commits_, pad, fy + scale(1), left - pad, scale(250));
    fy += row + gap;
  }
  move(fileLabel_, pad, fy, left - pad, scale(22));
  fy += scale(25);
  move(files_, pad, fy, left - pad, height - fy - footerHeight);
  move(diff_.handle(), left + gap, y, width - left - gap - pad, height - y - footerHeight);
  splitter_ = {left, y, left + gap, height - footerHeight};
  move(status_, pad, height - scale(27), width - pad * 2, scale(22));
}
void MainWindow::drawSplitter(HDC dc) const
{
  if (IsRectEmpty(&splitter_))
    return;
  RECT line = splitter_;
  int lineWidth = std::max(1, MulDiv(1, static_cast<int>(dpi_), 96));
  int center = (line.left + line.right) / 2;
  line.left = center - lineWidth / 2;
  line.right = line.left + lineWidth;
  auto brush = CreateSolidBrush(themeColor(ThemeColor::Border));
  FillRect(dc, &line, brush);
  DeleteObject(brush);
}
void MainWindow::moveSplitter(int x)
{
  RECT client{};
  GetClientRect(hwnd_, &client);
  auto scale = [&](int n) { return MulDiv(n, static_cast<int>(dpi_), 96); };
  int pad = scale(12), gap = scale(8);
  int maximum = std::max(scale(minFilePaneWidth), static_cast<int>(client.right) - pad - gap - scale(minDiffPaneWidth));
  int position = std::clamp(x, scale(minFilePaneWidth), maximum);
  if (position == splitter_.left)
    return;
  filePaneWidth_ = MulDiv(position, 96, static_cast<int>(dpi_));
  layout();
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
void MainWindow::sourceChanged()
{
  endPreview();
  series_.clear();
  SendMessageW(commits_, CB_RESETCONTENT, 0, 0);
  layout();
}
void MainWindow::refresh(bool seriesSelection)
{
  endPreview();
  if (directory_.empty())
  {
    diff_.setMessage(L"No repository found. Start gfd.exe from a repository folder.\n\nF5  "
                     L"Refresh\nCtrl+Shift+D  Toggle diff layout");
    return;
  }
  CompareRequest request{directory_, source(source_), getText(base_), getText(target_)};
  if (seriesSelection)
  {
    auto index = SendMessageW(commits_, CB_GETCURSEL, 0, 0);
    if (index > 0 && static_cast<size_t>(index) <= series_.size())
    {
      request.source = ChangeSource::Commit;
      request.target = series_[static_cast<size_t>(index) - 1].id;
    }
  }
  else if (request.source == ChangeSource::ReadyToPush)
    SendMessageW(commits_, CB_SETCURSEL, 0, 0);
  loading_ = true;
  SetWindowTextW(status_, L"Loading Git changes… You can change the source or refresh again.");
  controller_->request(std::move(request));
}
void MainWindow::loaded()
{
  auto result = controller_->takeResult();
  if (!result)
    return;
  loading_ = false;
  endPreview();
  bool initial = initialLoad_;
  initialLoad_ = false;
  if (!result->error.empty())
  {
    diff_.setMessage(L"Unable to load changes\n\n" + result->error);
    snapshot_ = {};
    selectedPath_.clear();
    SendMessageW(files_, LB_RESETCONTENT, 0, 0);
    SetWindowTextW(info_, directory_.c_str());
    SetWindowTextW(status_, L"Git failed — see the diff pane for details.");
    if (initial)
      MessageBoxW(hwnd_,
        (L"Не удалось открыть Git-репозиторий из папки:\n" + directory_ +
          L"\n\nЗапустите gfd.exe из репозитория или его вложенной папки.\n\n" + result->error)
          .c_str(),
        L"gfd — Repository error", MB_OK | MB_ICONERROR);
    return;
  }
  // Invalidate the view's pointer before replacing the owning document.
  diff_.setFile(nullptr);
  snapshot_ = std::move(result->snapshot);
  messageReturnIndex_ = -1;
  fileChanges_.clear();
  for (const auto &file : snapshot_.document.files)
  {
    size_t added = 0, removed = 0;
    for (const auto &h : file.hunks)
      for (const auto &line : h.lines)
      {
        added += line.type == DiffLineType::Added;
        removed += line.type == DiffLineType::Removed;
      }
    fileChanges_.emplace_back(added, removed);
  }
  directory_ = snapshot_.root;
  if (result->request.source == ChangeSource::ReadyToPush)
  {
    series_ = snapshot_.commits;
    SendMessageW(commits_, CB_RESETCONTENT, 0, 0);
    auto label = L"All commits (" + std::to_wstring(series_.size()) + L")";
    SendMessageW(commits_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    for (const auto &commit : series_)
    {
      label = commit.id.substr(0, 8) + L"  " + commit.subject;
      SendMessageW(commits_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(commits_, CB_SETCURSEL, 0, 0);
    if (!savedCommit_.empty())
    {
      auto id = std::move(savedCommit_);
      savedCommit_.clear();
      for (size_t i = 0; i < series_.size(); ++i)
        if (series_[i].id == id)
        {
          SendMessageW(commits_, CB_SETCURSEL, i + 1, 0);
          refresh(true);
          return;
        }
    }
  }
  auto repoName = std::filesystem::path(directory_).filename().wstring();
  auto info = repoName + L"     Branch: " + snapshot_.branch + L"     Base: " + snapshot_.base;
  if (!snapshot_.upstream.empty())
    info += L"     Upstream: " + snapshot_.upstream;
  SetWindowTextW(info_, info.c_str());
  SetWindowTextW(hwnd_, (L"GitDiffViewer — " + directory_).c_str());
  SendMessageW(files_, WM_SETREDRAW, FALSE, 0);
  tooltipIndex_ = -1;
  SendMessageW(tooltip_, TTM_POP, 0, 0);
  SendMessageW(files_, LB_RESETCONTENT, 0, 0);
  int selected = 0, index = 0;
  commitMessageFile_ = {};
  if (!snapshot_.commitId.empty())
  {
    commitMessageFile_.newPath = L"<<Commit Message>>";
    std::wistringstream message(snapshot_.commitMessage);
    std::wstring line;
    while (std::getline(message, line))
      commitMessageFile_.metadata.push_back(line.empty() ? L" " : line);
    SendMessageW(files_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"<<Commit Message>>"));
    index = 1;
  }
  size_t added = 0, removed = 0;
  for (const auto &file : snapshot_.document.files)
  {
    std::wstring label(1, statusLetter(file.status));
    label += L"   " + file.path();
    if (file.binary && file.status != FileStatus::Binary)
      label += L"  [binary]";
    SendMessageW(files_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (file.path() == selectedPath_)
      selected = index;
    ++index;
    for (const auto &h : file.hunks)
      for (const auto &line : h.lines)
      {
        added += line.type == DiffLineType::Added;
        removed += line.type == DiffLineType::Removed;
      }
  }
  SendMessageW(files_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(files_, nullptr, TRUE);
  if (!snapshot_.document.files.empty() || !snapshot_.commitId.empty())
  {
    SendMessageW(files_, LB_SETCURSEL, selected, 0);
    selectFile();
  }
  else
  {
    selectedPath_.clear();
    diff_.setMessage(snapshot_.notice.empty() ? L"No changes in this comparison.\n\nStaged shows the index; Unstaged shows "
                                                L"tracked working-tree edits.\nNew untracked files appear after git add."
                                              : snapshot_.notice);
  }
  auto status = std::to_wstring(snapshot_.document.files.size()) + L" files changed    +" + std::to_wstring(added) + L"    −" +
                std::to_wstring(removed);
  if (!snapshot_.notice.empty())
    status += L"    " + snapshot_.notice;
  else
    status += L"    |    F5 Refresh · Ctrl+Down/Up File · Space Commit message · Ctrl+Shift+D View · Ctrl+C Copy";
  SetWindowTextW(status_, status.c_str());
  layout();
}
void MainWindow::navigateFile(int direction)
{
  SetFocus(diff_.handle());
  if (loading_ || snapshot_.document.files.empty())
    return;
  int first = snapshot_.commitId.empty() ? 0 : 1;
  int current = static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0));
  int next = std::clamp(current + direction, first, first + static_cast<int>(snapshot_.document.files.size()) - 1);
  SendMessageW(files_, LB_SETCURSEL, next, 0);
  selectFile();
}
void MainWindow::toggleCommitMessage()
{
  if (loading_ || snapshot_.commitId.empty())
    return;
  endPreview();
  int current = static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0));
  if (current == 0)
  {
    if (snapshot_.document.files.empty())
      return;
    int next = std::clamp(messageReturnIndex_, 1, static_cast<int>(snapshot_.document.files.size()));
    SendMessageW(files_, LB_SETCURSEL, next, 0);
    selectFile();
    diff_.scroll(messageReturnTop_);
  }
  else
  {
    messageReturnIndex_ = current;
    messageReturnTop_ = diff_.topRow();
    SendMessageW(files_, LB_SETCURSEL, 0, 0);
    selectFile();
  }
}
void MainWindow::selectFile()
{
  endPreview();
  auto index = SendMessageW(files_, LB_GETCURSEL, 0, 0);
  if (!snapshot_.commitId.empty())
  {
    if (index == 0)
    {
      selectedPath_ = L"<<Commit Message>>";
      diff_.setFile(&commitMessageFile_, false, true);
      return;
    }
    --index;
  }
  if (index >= 0 && static_cast<size_t>(index) < snapshot_.document.files.size())
  {
    auto &file = snapshot_.document.files[static_cast<size_t>(index)];
    selectedPath_ = file.path();
    diff_.setFile(&file);
  }
}
void MainWindow::toggle()
{
  side_ = !side_;
  diff_.setSideBySide(side_);
  SendMessageW(view_, CB_SETCURSEL, side_ ? 1 : 0, 0);
}
void MainWindow::previewCommit(int index)
{
  if (loading_ || index <= 0 || static_cast<size_t>(index) > series_.size())
  {
    endPreview();
    return;
  }
  if (previewIndex_ == index)
    return;
  if (previewIndex_ < 0)
  {
    previewPreviousFile_ = diff_.file();
    previewPreviousPlain_ = diff_.plainText();
    previewTop_ = diff_.topRow();
  }
  previewIndex_ = index;
  diff_.setFile(nullptr);
  hoverMessageFile_ = {};
  const auto &commit = series_[static_cast<size_t>(index) - 1];
  hoverMessageFile_.newPath = L"<<Commit Message>> — " + commit.id.substr(0, 8);
  std::wistringstream input(commit.message);
  std::wstring line;
  while (std::getline(input, line))
    hoverMessageFile_.metadata.push_back(line.empty() ? L" " : line);
  diff_.setFile(&hoverMessageFile_, false, true);
}
void MainWindow::endPreview()
{
  if (previewIndex_ < 0)
    return;
  previewIndex_ = -1;
  diff_.setFile(previewPreviousFile_, false, previewPreviousPlain_);
  diff_.scroll(previewTop_);
  previewPreviousFile_ = nullptr;
}
LRESULT CALLBACK MainWindow::commitListProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
  auto self = reinterpret_cast<MainWindow *>(data);
  if (msg == WM_MOUSEMOVE)
  {
    auto hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, l);
    self->previewCommit(HIWORD(hit) ? -1 : LOWORD(hit));
    if (!self->automationHover_)
    {
      TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
      TrackMouseEvent(&track);
    }
  }
  else if (msg == WM_MOUSELEAVE && !self->automationHover_)
    self->endPreview();
  else if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(hwnd, commitListProcedure, id);
  return DefSubclassProc(hwnd, msg, w, l);
}
LRESULT CALLBACK MainWindow::comboProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
  auto self = reinterpret_cast<MainWindow *>(data);
  // Paint the entire closed control: owner draw alone leaves the native arrow and frame light.
  if (darkTheme && (msg == WM_PAINT || msg == WM_PRINTCLIENT))
  {
    PAINTSTRUCT ps{};
    HDC dc = msg == WM_PAINT ? BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(w);
    int saved = SaveDC(dc);
    RECT bounds{};
    GetClientRect(hwnd, &bounds);
    FillRect(dc, &bounds, self->fieldBrush_);
    COMBOBOXINFO info{sizeof(info)};
    GetComboBoxInfo(hwnd, &info);
    DRAWITEMSTRUCT item{};
    item.CtlType = ODT_COMBOBOX;
    item.CtlID = GetDlgCtrlID(hwnd);
    item.itemID = static_cast<UINT>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
    item.itemAction = ODA_DRAWENTIRE;
    item.hwndItem = hwnd;
    item.hDC = dc;
    item.rcItem = bounds;
    InflateRect(&item.rcItem, -2, -2);
    item.rcItem.right = info.rcButton.left;
    if (GetFocus() == hwnd && !(SendMessageW(hwnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
      item.itemState = ODS_FOCUS;
    self->drawListItem(item);
    auto border = CreateSolidBrush(themeColor(ThemeColor::Border));
    FrameRect(dc, &bounds, border);
    DeleteObject(border);
    int x = (info.rcButton.left + info.rcButton.right) / 2;
    int y = (bounds.top + bounds.bottom) / 2;
    int size = MulDiv(3, static_cast<int>(self->dpi_), 96);
    POINT arrow[] = {{x - size, y - 1}, {x, y + size - 1}, {x + size, y - 1}};
    auto pen = CreatePen(PS_SOLID, 1, themeColor(ThemeColor::Text));
    auto oldPen = SelectObject(dc, pen);
    Polyline(dc, arrow, 3);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    RestoreDC(dc, saved);
    if (msg == WM_PAINT)
      EndPaint(hwnd, &ps);
    return 0;
  }
  auto result = DefSubclassProc(hwnd, msg, w, l);
  if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == CB_SETCURSEL || msg == CB_SHOWDROPDOWN || msg == WM_KEYDOWN ||
      msg == WM_LBUTTONUP || msg == WM_ENABLE)
    InvalidateRect(hwnd, nullptr, FALSE);
  if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(hwnd, comboProcedure, id);
  return result;
}
void MainWindow::drawListItem(const DRAWITEMSTRUCT &item)
{
  if (item.CtlID != Files && item.CtlID != Commits && item.CtlID != Source && item.CtlID != View)
    return;
  int saved = SaveDC(item.hDC);
  RECT client{};
  GetClientRect(item.hwndItem, &client);
  IntersectClipRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
  if (item.CtlID == Files)
    IntersectClipRect(item.hDC, client.left, client.top, client.right, client.bottom);
  bool selected = (item.itemState & ODS_SELECTED) != 0;
  auto rowBrush = CreateSolidBrush(themeColor(selected ? ThemeColor::ListSelection : ThemeColor::Surface));
  FillRect(item.hDC, &item.rcItem, rowBrush);
  DeleteObject(rowBrush);
  SetTextColor(item.hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Text));
  SetBkMode(item.hDC, TRANSPARENT);
  SelectObject(item.hDC, font_);
  if (item.itemID != static_cast<UINT>(-1))
  {
    bool commit = item.CtlID == Commits;
    bool combo = item.CtlID != Files;
    auto length = SendMessageW(item.hwndItem, combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN, item.itemID, 0);
    if (length >= 0)
    {
      std::wstring value(static_cast<size_t>(length) + 1, L'\0');
      SendMessageW(item.hwndItem, combo ? CB_GETLBTEXT : LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(value.data()));
      value.resize(static_cast<size_t>(length));
      int pad = MulDiv(6, static_cast<int>(dpi_), 96);
      RECT textRect = item.rcItem;
      textRect.left += pad;
      textRect.right -= pad;
      int fileIndex = static_cast<int>(item.itemID) - (snapshot_.commitId.empty() ? 0 : 1);
      if (!combo && fileIndex >= 0 && static_cast<size_t>(fileIndex) < fileChanges_.size())
      {
        auto [added, removed] = fileChanges_[fileIndex];
        int cell = MulDiv(13, static_cast<int>(dpi_), 96);
        RECT indicator = textRect;
        indicator.left = indicator.right - cell * 2;
        indicator.right = indicator.left + cell;
        auto drawIndicator = [&](size_t count, ThemeColor role) {
          const size_t limits[] = {1, 2, 3, 5, 9, 30, 100};
          int level = 0;
          while (level < 7 && count > limits[level])
            ++level;
          wchar_t glyph = static_cast<wchar_t>(0x2581 + level);
          SetTextColor(item.hDC, themeColor(count ? role : selected ? ThemeColor::ListSelection : ThemeColor::Surface));
          DrawTextW(item.hDC, &glyph, 1, &indicator, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          OffsetRect(&indicator, cell, 0);
        };
        drawIndicator(removed, ThemeColor::RemovedIndicator);
        drawIndicator(added, ThemeColor::AddedIndicator);
        textRect.right -= cell * 2 + pad;
        SetTextColor(item.hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Text));
      }
      bool columns = commit ? item.itemID > 0 : !combo && value.size() >= 4 && value.substr(1, 3) == L"   ";
      if (columns)
      {
        RECT prefix = textRect;
        prefix.right = textRect.left + MulDiv(commit ? 80 : 28, static_cast<int>(dpi_), 96);
        auto code = value.substr(0, commit ? 8 : 1);
        DrawTextW(item.hDC, code.c_str(), static_cast<int>(code.size()), &prefix, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        textRect.left = prefix.right;
        value.erase(0, commit ? 10 : 4);
      }
      if (!combo)
      {
        auto separator = value.find_last_of(L"/\\");
        if (separator != std::wstring::npos)
          value.insert(separator + 1, 1, L' ');
      }
      SIZE textSize{};
      GetTextExtentPoint32W(item.hDC, value.c_str(), static_cast<int>(value.size()), &textSize);
      UINT align = !combo && textSize.cx > textRect.right - textRect.left ? DT_RIGHT : DT_LEFT;
      int textClip = SaveDC(item.hDC);
      IntersectClipRect(item.hDC, textRect.left, textRect.top, textRect.right, textRect.bottom);
      DrawTextW(item.hDC, value.c_str(), static_cast<int>(value.size()), &textRect,
        align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (combo ? DT_END_ELLIPSIS : 0));
      auto separator = combo ? std::wstring::npos : value.find_last_of(L"/\\");
      if (separator != std::wstring::npos)
      {
        SIZE prefixSize{};
        GetTextExtentPoint32W(item.hDC, value.c_str(), static_cast<int>(separator + 1), &prefixSize);
        int origin = align == DT_RIGHT ? textRect.right - textSize.cx : textRect.left;
        IntersectClipRect(item.hDC, textRect.left, textRect.top, origin + prefixSize.cx, textRect.bottom);
        auto pathBackground = CreateSolidBrush(themeColor(selected ? ThemeColor::ListSelection : ThemeColor::Surface));
        FillRect(item.hDC, &textRect, pathBackground);
        DeleteObject(pathBackground);
        SetTextColor(item.hDC, pathTextColor(selected));
        DrawTextW(item.hDC, value.c_str(), static_cast<int>(value.size()), &textRect,
          align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
      }
      RestoreDC(item.hDC, textClip);
    }
  }
  if (item.itemState & ODS_FOCUS)
    DrawFocusRect(item.hDC, &item.rcItem);
  RestoreDC(item.hDC, saved);
}

void MainWindow::saveSettings()
{
  if (!automationDirectory_.empty())
    return;
  HKEY key{};
  if (RegCreateKeyExW(HKEY_CURRENT_USER, settingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;
  auto number = [&](const wchar_t *name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&value), sizeof(value));
  };
  auto string = [&](const wchar_t *name, const std::wstring &value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(value.c_str()),
      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  };
  number(L"Source", static_cast<DWORD>(source(source_)));
  number(L"SideBySide", side_);
  number(L"FontSize", diff_.fontSize());
  number(L"DarkTheme", darkTheme);
  number(L"FilePaneWidth", static_cast<DWORD>(filePaneWidth_));
  string(L"Base", getText(base_));
  string(L"Target", getText(target_));
  auto index = SendMessageW(commits_, CB_GETCURSEL, 0, 0);
  string(L"Commit", index > 0 && static_cast<size_t>(index) <= series_.size() ? series_[index - 1].id : L"");
  WINDOWPLACEMENT placement{sizeof(placement)};
  if (GetWindowPlacement(hwnd_, &placement))
  {
    if (placement.showCmd == SW_SHOWMINIMIZED)
      placement.showCmd = placement.flags & WPF_RESTORETOMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    RegSetValueExW(key, L"WindowPlacement", 0, REG_BINARY, reinterpret_cast<const BYTE *>(&placement), sizeof(placement));
  }
  RegCloseKey(key);
}
void MainWindow::applyTheme()
{
  if (backgroundBrush_)
    DeleteObject(backgroundBrush_);
  if (fieldBrush_)
    DeleteObject(fieldBrush_);
  backgroundBrush_ = CreateSolidBrush(themeColor(ThemeColor::Window));
  fieldBrush_ = CreateSolidBrush(themeColor(ThemeColor::Surface));
  BOOL dark = darkTheme;
  if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark))))
    DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));
  // Explicit caption colors also invalidate DWM's cached caption on theme changes.
  COLORREF caption = themeColor(ThemeColor::Window), captionText = themeColor(ThemeColor::Text);
  DwmSetWindowAttribute(hwnd_, 35, &caption, sizeof(caption));
  DwmSetWindowAttribute(hwnd_, 36, &captionText, sizeof(captionText));
  SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  SendMessageW(hwnd_, WM_NCACTIVATE, FALSE, 0);
  SendMessageW(hwnd_, WM_NCACTIVATE, GetForegroundWindow() == hwnd_, 0);
  for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    SetWindowTheme(child, darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
  for (HWND combo : {source_, view_, commits_})
  {
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(combo, &info))
      SetWindowTheme(info.hwndList, darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
  }
  SetWindowTextW(themeButton_, darkTheme ? L"Light theme" : L"Dark theme");
  SendMessageW(tooltip_, TTM_SETTIPBKCOLOR, themeColor(ThemeColor::Surface), 0);
  SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR, themeColor(ThemeColor::Text), 0);
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}
std::wstring MainWindow::filePathAt(int index) const
{
  if (!snapshot_.commitId.empty())
    --index;
  if (index < 0 || static_cast<size_t>(index) >= snapshot_.document.files.size())
    return {};
  return snapshot_.document.files[index].path();
}
LRESULT CALLBACK MainWindow::filesProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
  auto self = reinterpret_cast<MainWindow *>(data);
  if (msg == WM_MOUSEMOVE)
  {
    auto hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, l);
    int index = HIWORD(hit) ? -1 : LOWORD(hit);
    if (index != self->tooltipIndex_)
    {
      self->tooltipIndex_ = index;
      auto path = self->filePathAt(index);
      self->tooltipText_ = path.empty() ? L"" : (std::filesystem::path(self->snapshot_.root) / path).wstring();
      int fileIndex = index - (self->snapshot_.commitId.empty() ? 0 : 1);
      if (!path.empty() && fileIndex >= 0 && static_cast<size_t>(fileIndex) < self->fileChanges_.size())
      {
        auto [added, removed] = self->fileChanges_[fileIndex];
        self->tooltipText_ += L"    -" + std::to_wstring(removed) + L" +" + std::to_wstring(added);
      }
      SendMessageW(self->tooltip_, TTM_POP, 0, 0);
      TOOLINFOW tool{sizeof(tool)};
      tool.hwnd = self->hwnd_;
      tool.uId = reinterpret_cast<UINT_PTR>(hwnd);
      tool.lpszText = self->tooltipText_.data();
      SendMessageW(self->tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
  }
  if (msg == WM_VSCROLL || msg == WM_MOUSEWHEEL)
  {
    self->tooltipIndex_ = -1;
    SendMessageW(self->tooltip_, TTM_POP, 0, 0);
  }
  if (msg == WM_CONTEXTMENU)
  {
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    int index;
    if (point.x == -1 && point.y == -1)
    {
      index = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
      RECT r{};
      SendMessageW(hwnd, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&r));
      point = {r.left + 12, r.bottom};
      ClientToScreen(hwnd, &point);
    }
    else
    {
      POINT client = point;
      ScreenToClient(hwnd, &client);
      auto hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y));
      index = HIWORD(hit) ? -1 : LOWORD(hit);
    }
    if (index < 0)
      return 0;
    SendMessageW(hwnd, LB_SETCURSEL, index, 0);
    SetFocus(hwnd);
    self->selectFile();
    auto path = self->filePathAt(index);
    if (path.empty())
      return 0;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Copy File Name");
    auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, self->hwnd_, nullptr);
    DestroyMenu(menu);
    if (command == 1)
    {
      auto memory = GlobalAlloc(GMEM_MOVEABLE, (path.size() + 1) * sizeof(wchar_t));
      if (memory)
      {
        auto ptr = GlobalLock(memory);
        if (ptr)
        {
          memcpy(ptr, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
          GlobalUnlock(memory);
          if (OpenClipboard(hwnd))
          {
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, memory))
              memory = nullptr;
            CloseClipboard();
          }
        }
        if (memory)
          GlobalFree(memory);
      }
    }
    return 0;
  }
  if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(hwnd, filesProcedure, id);
  return DefSubclassProc(hwnd, msg, w, l);
}
void MainWindow::screenshot()
{
  IFileSaveDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    return;
  COMDLG_FILTERSPEC filter{L"PNG image", L"*.png"};
  dialog->SetFileTypes(1, &filter);
  dialog->SetDefaultExtension(L"png");
  dialog->SetFileName(L"GitDiffViewer.png");
  dialog->SetTitle(L"Save application screenshot");
  std::wstring path;
  if (SUCCEEDED(dialog->Show(hwnd_)))
  {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)))
    {
      PWSTR value = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &value)))
      {
        path = value;
        CoTaskMemFree(value);
      }
      item->Release();
    }
  }
  dialog->Release();
  if (!path.empty())
    try
    {
      saveScreenshot(hwnd_, path);
      SetWindowTextW(status_, (L"Screenshot saved: " + path).c_str());
    }
    catch (...)
    {
      MessageBoxW(hwnd_, L"Could not save the screenshot. Check the destination path.", L"Screenshot", MB_ICONERROR);
    }
}
LRESULT CALLBACK MainWindow::procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
  auto self = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE)
  {
    self = static_cast<MainWindow *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->message(msg, w, l) : DefWindowProcW(hwnd, msg, w, l);
}
LRESULT MainWindow::message(UINT msg, WPARAM w, LPARAM l)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
    {
      RECT r{};
      GetClientRect(hwnd_, &r);
      FillRect(reinterpret_cast<HDC>(w), &r, backgroundBrush_);
      return 1;
    }
    case WM_PAINT:
    {
      PAINTSTRUCT paint{};
      auto dc = BeginPaint(hwnd_, &paint);
      drawSplitter(dc);
      EndPaint(hwnd_, &paint);
      return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
      auto dc = reinterpret_cast<HDC>(w);
      bool field = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX;
      SetTextColor(dc, themeColor(ThemeColor::Text));
      SetBkColor(dc, themeColor(field ? ThemeColor::Surface : ThemeColor::Window));
      return reinterpret_cast<LRESULT>(field ? fieldBrush_ : backgroundBrush_);
    }
    case WM_NOTIFY:
    {
      auto hdr = reinterpret_cast<NMHDR *>(l);
      if (hdr->hwndFrom == tooltip_ && hdr->code == TTN_GETDISPINFOW && hdr->idFrom == reinterpret_cast<UINT_PTR>(info_))
      {
        infoTooltipText_ = directory_ + L"\n" + getText(info_);
        reinterpret_cast<NMTTDISPINFOW *>(l)->lpszText = infoTooltipText_.data();
        return 0;
      }
      if (hdr->code == NM_CUSTOMDRAW && (hdr->hwndFrom == refresh_ || hdr->hwndFrom == compare_ || hdr->hwndFrom == themeButton_) &&
          darkTheme)
      {
        auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
          FillRect(draw->hdc, &draw->rc, fieldBrush_);
          auto border = CreateSolidBrush(themeColor(ThemeColor::Border));
          FrameRect(draw->hdc, &draw->rc, border);
          DeleteObject(border);
          SetBkMode(draw->hdc, TRANSPARENT);
          SetTextColor(draw->hdc, themeColor(ThemeColor::Text));
          auto label = getText(hdr->hwndFrom);
          DrawTextW(draw->hdc, label.c_str(), -1, &draw->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          if (draw->uItemState & CDIS_FOCUS)
          {
            auto r = draw->rc;
            InflateRect(&r, -3, -3);
            DrawFocusRect(draw->hdc, &r);
          }
          return CDRF_SKIPDEFAULT;
        }
      }
      break;
    }
    case WM_MEASUREITEM:
    {
      auto item = reinterpret_cast<MEASUREITEMSTRUCT *>(l);
      item->itemHeight = MulDiv(24, static_cast<int>(dpi_), 96);
      return TRUE;
    }
    case WM_DRAWITEM: drawListItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l)); return TRUE;
    case WM_TIMER:
      if (w == 1)
        automationTick();
      return 0;
    case WM_PRINTCLIENT:
    {
      RECT r{};
      GetClientRect(hwnd_, &r);
      FillRect(reinterpret_cast<HDC>(w), &r, backgroundBrush_);
      drawSplitter(reinterpret_cast<HDC>(w));
      return 0;
    }
    case WM_CREATE: createControls(); return 0;
    case WM_SIZE:
      layout();
      RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
      return 0;
    case WM_SETCURSOR:
    {
      POINT point{};
      GetCursorPos(&point);
      ScreenToClient(hwnd_, &point);
      if (draggingSplitter_ || PtInRect(&splitter_, point))
      {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return TRUE;
      }
      break;
    }
    case WM_LBUTTONDOWN:
    {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (PtInRect(&splitter_, point))
      {
        draggingSplitter_ = true;
        splitterDragOffset_ = point.x - splitter_.left;
        SetCapture(hwnd_);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return 0;
      }
      break;
    }
    case WM_MOUSEMOVE:
      if (draggingSplitter_)
      {
        moveSplitter(GET_X_LPARAM(l) - splitterDragOffset_);
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (draggingSplitter_)
      {
        moveSplitter(GET_X_LPARAM(l) - splitterDragOffset_);
        ReleaseCapture();
        return 0;
      }
      break;
    case WM_CAPTURECHANGED: draggingSplitter_ = false; break;
    case WM_CANCELMODE:
      if (draggingSplitter_)
        ReleaseCapture();
      break;
    case WM_GETMINMAXINFO:
    {
      auto p = reinterpret_cast<MINMAXINFO *>(l);
      p->ptMinTrackSize = {MulDiv(820, static_cast<int>(dpi_), 96), MulDiv(500, static_cast<int>(dpi_), 96)};
      return 0;
    }
    case WM_DPICHANGED:
    {
      dpi_ = HIWORD(w);
      auto r = reinterpret_cast<RECT *>(l);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      updateFonts();
      return 0;
    }
    case repositoryReady: loaded(); return 0;
    case WM_COMMAND:
      switch (LOWORD(w))
      {
        case Theme:
          darkTheme = !darkTheme;
          applyTheme();
          break;
        case Refresh: refresh(); break;
        case Compare: refresh(); break;
        case Source:
          if (HIWORD(w) == CBN_SELCHANGE)
          {
            sourceChanged();
            refresh();
          }
          break;
        case Files:
          if (HIWORD(w) == LBN_SELCHANGE)
            selectFile();
          break;
        case Commits:
          if (HIWORD(w) == CBN_CLOSEUP)
            endPreview();
          if (HIWORD(w) == CBN_SELENDOK || (HIWORD(w) == CBN_SELCHANGE && !SendMessageW(commits_, CB_GETDROPPEDSTATE, 0, 0)))
            refresh(true);
          break;
        case View:
          if (HIWORD(w) == CBN_SELCHANGE && (SendMessageW(view_, CB_GETCURSEL, 0, 0) == 1) != side_)
            toggle();
          break;
        case Toggle: toggle(); break;
        case Screenshot: screenshot(); break;
        case ZoomIn: diff_.zoom(1); break;
        case NextFile: navigateFile(1); break;
        case PreviousFile: navigateFile(-1); break;
        case ZoomOut: diff_.zoom(-1); break;
      }
      return 0;
    case WM_ENDSESSION:
      if (w)
        saveSettings();
      return 0;
    case WM_CLOSE:
      saveSettings();
      if (controller_)
        controller_->shutdown();
      DestroyWindow(hwnd_);
      return 0;
    case WM_DESTROY:
      if (controller_)
        controller_->shutdown();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, w, l);
}
} // namespace gdv
