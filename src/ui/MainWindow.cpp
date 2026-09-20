#include "MainWindow.h"
#include "CommentEditor.h"
#include "CommitPicker.h"
#include "Screenshot.h"
#include "Theme.h"
#include <uxtheme.h>
#include <dwmapi.h>
#include <filesystem>
#include <algorithm>
#include <cwctype>
#include <iterator>
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
  NextChange,
  PreviousChange,
  Theme,
  FullFile,
  ExplorerLayout,
  ExplorerCommits,
  ExplorerFiles,
  CopyComments
};
constexpr int minFilePaneWidth = 220;
constexpr int minDiffPaneWidth = 300;
constexpr UINT_PTR automationTimer = 1;
constexpr UINT_PTR statusAnimationTimer = 2;
constexpr UINT_PTR explorerResizeTimer = 3;
std::wstring getText(HWND h)
{
  int n = GetWindowTextLengthW(h);
  std::wstring s(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(h, s.data(), n + 1);
  s.resize(n);
  return s;
}
ChangeSource source(HWND h) { return static_cast<ChangeSource>(SendMessageW(h, CB_GETCURSEL, 0, 0)); }
std::pair<size_t, size_t> changes(const FileDiff &file)
{
  size_t added = 0, removed = 0;
  for (const auto &hunk : file.hunks)
    for (const auto &line : hunk.lines)
    {
      added += line.type == DiffLineType::Added;
      removed += line.type == DiffLineType::Removed;
    }
  return {added, removed};
}
std::pair<size_t, size_t> changes(const DiffDocument &document)
{
  size_t added = 0, removed = 0;
  for (const auto &file : document.files)
  {
    auto counts = changes(file);
    added += counts.first;
    removed += counts.second;
  }
  return {added, removed};
}
void copyFileName(HWND owner, const std::wstring &path)
{
  auto memory = GlobalAlloc(GMEM_MOVEABLE, (path.size() + 1) * sizeof(wchar_t));
  if (!memory)
    return;
  auto ptr = GlobalLock(memory);
  if (ptr)
  {
    memcpy(ptr, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(memory);
    if (OpenClipboard(owner))
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
int editPageRows(HWND hwnd, HFONT font)
{
  RECT area{};
  GetClientRect(hwnd, &area);
  HDC dc = GetDC(hwnd);
  auto old = SelectObject(dc, font);
  TEXTMETRICW metric{};
  GetTextMetricsW(dc, &metric);
  SelectObject(dc, old);
  ReleaseDC(hwnd, dc);
  return std::max(1, static_cast<int>(area.bottom) / std::max(1, static_cast<int>(metric.tmHeight)));
}
void revealListSelection(HWND hwnd, int index)
{
  RECT area{};
  GetClientRect(hwnd, &area);
  int rowHeight = std::max(1, static_cast<int>(SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0)));
  int page = std::max(1, static_cast<int>(area.bottom) / rowHeight);
  int top = static_cast<int>(SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0));
  if (index < top)
    SendMessageW(hwnd, LB_SETTOPINDEX, index, 0);
  else if (index >= top + page)
    SendMessageW(hwnd, LB_SETTOPINDEX, index - page + 1, 0);
}
void scrollListWheel(HWND hwnd, WPARAM w, int &remainder)
{
  int delta = GET_WHEEL_DELTA_WPARAM(w);
  MSG queued{};
  while (PeekMessageW(&queued, hwnd, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE))
    delta += GET_WHEEL_DELTA_WPARAM(queued.wParam);
  remainder += delta;
  int detents = remainder / WHEEL_DELTA;
  remainder %= WHEEL_DELTA;
  if (!detents)
    return;
  UINT wheelLines = 3;
  SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &wheelLines, 0);
  RECT area{};
  GetClientRect(hwnd, &area);
  int itemHeight = std::max(1, static_cast<int>(SendMessageW(hwnd, LB_GETITEMHEIGHT, 0, 0)));
  int page = std::max(1, static_cast<int>(area.bottom - area.top) / itemHeight);
  int step = wheelLines == WHEEL_PAGESCROLL ? page : static_cast<int>(wheelLines);
  int count = static_cast<int>(SendMessageW(hwnd, LB_GETCOUNT, 0, 0));
  int top = static_cast<int>(SendMessageW(hwnd, LB_GETTOPINDEX, 0, 0));
  if (step > 0)
    SendMessageW(hwnd, LB_SETTOPINDEX, std::clamp(top - detents * step, 0, std::max(0, count - page)), 0);
}
} // namespace
MainWindow::~MainWindow()
{
  if (controller_)
    controller_->shutdown();
  if (font_)
    DeleteObject(font_);
  if (boldFont_)
    DeleteObject(boldFont_);
  if (backgroundBrush_)
    DeleteObject(backgroundBrush_);
  if (fieldBrush_)
    DeleteObject(fieldBrush_);
}
int MainWindow::run(HINSTANCE instance, int show, std::wstring directory, std::wstring automationDirectory, std::wstring hashPrefix)
{
  instance_ = instance;
  directory_ = std::move(directory);
  automationDirectory_ = std::move(automationDirectory);
  if (automationDirectory_.empty())
    settings_ = Settings::loadUser();
  auto statsMode = settings_.string(L"FileStatsMode");
  if (statsMode == L"none")
    fileStatsMode_ = FileStatsMode::None;
  else if (statsMode == L"bars")
    fileStatsMode_ = FileStatsMode::Bars;
  else if (statsMode == L"numbers")
    fileStatsMode_ = FileStatsMode::Numbers;
  historyInitialLimit_ = automationDirectory_.empty() ? std::clamp<size_t>(settings_.number(L"HistoryCommitCount", 10), 1, 1000) : 10;
  historyLimit_ = historyInitialLimit_;
  side_ = automationDirectory_.empty() && settings_.number(L"SideBySide", 0) != 0;
  darkTheme = !automationDirectory_.empty() || settings_.number(L"DarkTheme", 1) != 0;
  filePaneWidth_ = automationDirectory_.empty() ? static_cast<int>(std::min<DWORD>(settings_.number(L"FilePaneWidth", 0), 4096)) : 0;
  explorerLayout_ = automationDirectory_.empty() && settings_.number(L"ExplorerLayout", 0) != 0;
  explorerCommitWidth_ = static_cast<int>(std::clamp<DWORD>(settings_.number(L"ExplorerCommitWidth", 300), 160, 4096));
  explorerMessageWidth_ = static_cast<int>(std::clamp<DWORD>(settings_.number(L"ExplorerMessageWidth", 360), 160, 4096));
  explorerTopHeight_ = static_cast<int>(std::clamp<DWORD>(settings_.number(L"ExplorerTopHeight", 280), 140, 4096));
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
    if (settings_.windowPlacement(placement))
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
  if (!hashPrefix.empty())
  {
    std::atomic_bool cancel{false};
    auto matches = GitRepository{}.findCommitsByPrefix(directory_, hashPrefix, cancel);
    std::optional<std::wstring> selected;
    if (matches.size() == 1)
      selected = matches.front().id;
    else
      selected = chooseCommit(hwnd_, instance_, hashPrefix, matches);
    if (selected)
    {
      SendMessageW(source_, CB_SETCURSEL, static_cast<WPARAM>(ChangeSource::Commit), 0);
      sourceChanged();
      SetWindowTextW(target_, selected->c_str());
    }
  }
  if (!directory_.empty())
    refresh();
  if (!automationDirectory_.empty())
    SetTimer(hwnd_, automationTimer, 100, nullptr);
  ACCEL entries[] = {{FVIRTKEY | FCONTROL, 'R', Refresh}, {FVIRTKEY, VK_F5, Refresh}, {FVIRTKEY | FCONTROL | FSHIFT, 'D', Toggle},
    {FVIRTKEY | FCONTROL | FSHIFT, 'S', Screenshot}, {FVIRTKEY, VK_F2, CopyComments}, {FVIRTKEY | FCONTROL, VK_OEM_PLUS, ZoomIn},
    {FVIRTKEY | FCONTROL, VK_OEM_MINUS, ZoomOut}, {FVIRTKEY | FCONTROL | FSHIFT, VK_OEM_PLUS, ZoomIn},
    {FVIRTKEY | FCONTROL, VK_DOWN, NextFile}, {FVIRTKEY | FCONTROL, VK_UP, PreviousFile}, {FVIRTKEY | FCONTROL, VK_NEXT, NextChange},
    {FVIRTKEY | FCONTROL, VK_PRIOR, PreviousChange}, {FVIRTKEY | FCONTROL, VK_ADD, ZoomIn},
    {FVIRTKEY | FCONTROL, VK_SUBTRACT, ZoomOut}};
  HACCEL accel = CreateAcceleratorTableW(entries, static_cast<int>(std::size(entries)));
  MSG msg{};
  int result = 0;
  while ((result = GetMessageW(&msg, nullptr, 0, 0)) > 0)
  {
    if (msg.message == WM_KEYDOWN && msg.wParam == 'F' && !(msg.lParam & (1LL << 30)) && !(GetKeyState(VK_CONTROL) & 0x8000) &&
        !(GetKeyState(VK_MENU) & 0x8000))
    {
      auto focus = GetFocus();
      bool editing = focus == base_ || focus == target_ || focus == source_ || focus == commits_;
      bool dropdown = SendMessageW(source_, CB_GETDROPPEDSTATE, 0, 0) || SendMessageW(commits_, CB_GETDROPPEDSTATE, 0, 0);
      if (!editing && !dropdown)
      {
        SendMessageW(fullFileButton_, BM_CLICK, 0, 0);
        continue;
      }
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE &&
        (GetFocus() == diff_.handle() || GetFocus() == files_ || GetFocus() == commits_) &&
        !SendMessageW(commits_, CB_GETDROPPEDSTATE, 0, 0))
    {
      if (!(msg.lParam & (1LL << 30)))
      {
        int index = GetFocus() == files_ ? static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0)) : -1;
        if (index >= 0 && static_cast<size_t>(index) < fileListItems_.size() &&
            fileListItems_[static_cast<size_t>(index)].kind == FileListItemKind::LoadMore)
          loadMoreHistory();
        else
          toggleCommitMessage();
      }
      continue;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && GetFocus() == files_)
    {
      int index = static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0));
      if (index >= 0 && static_cast<size_t>(index) < fileListItems_.size() &&
          fileListItems_[static_cast<size_t>(index)].kind == FileListItemKind::LoadMore)
      {
        loadMoreHistory();
        continue;
      }
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
  SendMessageW(source_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"History"));
  SendMessageW(source_, CB_SETCURSEL, automationDirectory_.empty() ? std::min<DWORD>(6, settings_.number(L"Source", 1)) : 1, 0);
  view_ = control(L"BUTTON", L"Side-by-side", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, View);
  SendMessageW(view_, BM_SETCHECK, side_ ? BST_CHECKED : BST_UNCHECKED, 0);
  fullFileButton_ = control(L"BUTTON", L"Full file", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, FullFile);
  themeButton_ = control(L"BUTTON", L"\u25D0", WS_TABSTOP, Theme);
  layoutButton_ = control(L"BUTTON", L"Wide Diff", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, ExplorerLayout);
  copyCommentsButton_ = control(L"BUTTON", L"\u29C9", WS_TABSTOP, CopyComments);
  EnableWindow(copyCommentsButton_, FALSE);
  SendMessageW(layoutButton_, BM_SETCHECK, explorerLayout_ ? BST_CHECKED : BST_UNCHECKED, 0);
  baseLabel_ = control(L"STATIC", L"Base branch / ref", 0, 0);
  base_ = control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, Base);
  targetLabel_ = control(L"STATIC", L"Target / commit", 0, 0);
  target_ = control(L"EDIT", L"HEAD", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, Target);
  compare_ = control(L"BUTTON", L"Compare", WS_TABSTOP, Compare);
  commitLabel_ = control(L"STATIC", L"LOCAL COMMITS", 0, 0);
  commits_ = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP | WS_VSCROLL, Commits);
  for (HWND combo : {source_, commits_})
    SetWindowSubclass(combo, comboProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  COMBOBOXINFO comboInfo{sizeof(comboInfo)};
  if (GetComboBoxInfo(commits_, &comboInfo))
  {
    commitPopup_ = comboInfo.hwndList;
    SetWindowSubclass(commitPopup_, commitListProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  }
  fileLabel_ = control(L"STATIC", L"CHANGED FILES", 0, 0);
  explorerCommitLabel_ = control(L"STATIC", L"COMMITS", 0, 0);
  explorerMessageLabel_ = control(L"STATIC", L"COMMIT MESSAGE", 0, 0);
  explorerFilesLabel_ = control(L"STATIC", L"FILES", 0, 0);
  explorerCommits_ = control(L"LISTBOX", L"",
    WS_TABSTOP | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, ExplorerCommits);
  explorerMessage_ = control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0);
  explorerFiles_ = control(L"LISTBOX", L"",
    WS_TABSTOP | WS_BORDER | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, ExplorerFiles);
  SetWindowSubclass(explorerCommits_, explorerListProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  SetWindowSubclass(explorerFiles_, explorerListProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  SetWindowSubclass(explorerMessage_, explorerMessageProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  for (int i = 0; i < 3; ++i)
  {
    explorerBars_[i] = control(L"STATIC", L"", SS_NOTIFY, 0);
    SetWindowSubclass(explorerBars_[i], explorerBarProcedure, static_cast<UINT_PTR>(i), reinterpret_cast<DWORD_PTR>(this));
  }
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
  tool.uId = reinterpret_cast<UINT_PTR>(explorerFiles_);
  SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
  tool.uId = reinterpret_cast<UINT_PTR>(info_);
  tool.lpszText = LPSTR_TEXTCALLBACKW;
  SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
  const std::pair<HWND, const wchar_t *> buttonHints[] = {
    {refresh_, L"Reload Git changes and clear review comments.\nHotkeys: F5 or Ctrl+R."},
    {view_, L"Switch between side-by-side and unified diffs.\nHotkey: Ctrl+Shift+D."},
    {fullFileButton_, L"Show the complete selected file or only changed hunks.\nHotkey: F (outside text fields)."},
    {themeButton_, L"Switch between dark and light themes."},
    {layoutButton_, L"Show Commits, Files and Commit message above a full-width diff, or return to classic layout."},
    {copyCommentsButton_, L"Copy all review comments to the clipboard.\nHotkey: F2."},
    {compare_, L"Compare the entered base and target refs."}};
  for (const auto &[button, hint] : buttonHints)
  {
    tool.uId = reinterpret_cast<UINT_PTR>(button);
    tool.lpszText = const_cast<wchar_t *>(hint);
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
  }
  SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 1000);
  status_ = control(L"STATIC", L"Finding repository…", SS_OWNERDRAW, 0);
  diff_.create(hwnd_, instance_);
  diff_.setSideBySide(side_);
  controller_ = std::make_unique<RepositoryController>(hwnd_);
  dpi_ = GetDpiForWindow(hwnd_);
  updateFonts();
  if (automationDirectory_.empty())
  {
    diff_.zoom(static_cast<int>(std::clamp<DWORD>(settings_.number(L"FontSize", 11), 6, 40)) - diff_.fontSize());
    readyBase_ = settings_.string(L"ReadyBase");
    rangeBase_ = settings_.string(L"RangeBase");
    if (rangeBase_.empty())
      rangeBase_ = settings_.string(L"Base");
    auto target = settings_.string(L"Target");
    if (!target.empty())
      SetWindowTextW(target_, target.c_str());
    savedCommit_ = settings_.string(L"Commit");
  }
  baseMode_ = source(source_);
  if (baseMode_ == ChangeSource::ReadyToPush)
    SetWindowTextW(base_, readyBase_.c_str());
  else if (baseMode_ == ChangeSource::Range)
    SetWindowTextW(base_, rangeBase_.c_str());
  applyTheme();
  sourceChanged();
}
void MainWindow::updateFonts()
{
  if (font_)
    DeleteObject(font_);
  if (boldFont_)
    DeleteObject(boldFont_);
  font_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  boldFont_ = CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  for (HWND child = GetWindow(hwnd_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  diff_.setDpi(dpi_);
  auto itemHeight = MulDiv(24, static_cast<int>(dpi_), 96);
  SendMessageW(commits_, CB_SETITEMHEIGHT, 0, itemHeight);
  SendMessageW(commits_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), itemHeight);
  SendMessageW(files_, LB_SETITEMHEIGHT, 0, itemHeight);
  SendMessageW(explorerCommits_, LB_SETITEMHEIGHT, 0, itemHeight);
  SendMessageW(explorerFiles_, LB_SETITEMHEIGHT, 0, itemHeight);
  // MoveWindow's height includes the dropdown, not the closed combobox field.
  // Native themed buttons have a one-pixel transparent inset. Match the visible
  // borders, not just the HWND rectangles, while accounting for the combo frame.
  const int toolbarHeight = MulDiv(30, static_cast<int>(dpi_), 96) - 2 * MulDiv(1, static_cast<int>(dpi_), 96);
  for (HWND combo : {source_, commits_})
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
  int toolbarX = pad;
  move(refresh_, toolbarX, pad, scale(80), row);
  toolbarX += scale(92);
  move(source_, toolbarX, pad + scale(1), scale(180), scale(240));
  toolbarX += scale(192);
  move(view_, toolbarX, pad, scale(125), row);
  toolbarX += scale(137);
  move(fullFileButton_, toolbarX, pad, scale(100), row);
  toolbarX += scale(112);
  move(themeButton_, toolbarX, pad, row, row);
  toolbarX += scale(42);
  move(layoutButton_, toolbarX, pad, scale(90), row);
  toolbarX += scale(102);
  move(copyCommentsButton_, toolbarX, pad, row, row);
  toolbarX += scale(42);
  int y = pad + row + gap;
  if (width < toolbarX + scale(220))
  {
    move(info_, pad, y, width - pad * 2, row);
    y += row + gap;
  }
  else
    move(info_, toolbarX, pad, width - toolbarX - pad, row);
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
  ShowWindow(commits_, SW_HIDE);
  ShowWindow(commitLabel_, SW_HIDE);
  for (auto h : {fileLabel_, files_})
    ShowWindow(h, explorerLayout_ ? SW_HIDE : SW_SHOW);
  for (auto h : {explorerCommitLabel_, explorerMessageLabel_, explorerFilesLabel_, explorerCommits_, explorerMessage_, explorerFiles_,
         explorerBars_[0], explorerBars_[1], explorerBars_[2]})
    ShowWindow(h, explorerLayout_ ? SW_SHOW : SW_HIDE);
  if (explorerLayout_)
  {
    int contentWidth = std::max(1, width - pad * 2);
    int minPane = scale(160), minDiffHeight = scale(180), minTop = scale(140);
    int first = std::clamp(scale(explorerCommitWidth_), minPane, std::max(minPane, contentWidth - 2 * minPane - 2 * gap));
    int second = std::clamp(scale(explorerMessageWidth_), minPane, std::max(minPane, contentWidth - first - minPane - 2 * gap));
    int third = std::max(1, contentWidth - first - second - 2 * gap);
    int available = std::max(1, height - y - footerHeight);
    int top = std::clamp(scale(explorerTopHeight_), minTop, std::max(minTop, available - gap - minDiffHeight));
    int listY = y + scale(25);
    int listHeight = std::max(1, top - scale(25));
    int barWidth = scale(13);
    int x1 = pad + first, x2 = x1 + gap + second;
    move(explorerCommitLabel_, pad, y, first, scale(22));
    move(explorerCommits_, pad, listY, first - barWidth, listHeight);
    move(explorerBars_[0], pad + first - barWidth, listY, barWidth, listHeight);
    move(explorerFilesLabel_, x1 + gap, y, second, scale(22));
    move(explorerFiles_, x1 + gap, listY, second - barWidth, listHeight);
    move(explorerBars_[1], x1 + gap + second - barWidth, listY, barWidth, listHeight);
    move(explorerMessageLabel_, x2 + gap, y, third, scale(22));
    move(explorerMessage_, x2 + gap, listY, third - barWidth, listHeight);
    move(explorerBars_[2], x2 + gap + third - barWidth, listY, barWidth, listHeight);
    move(diff_.handle(), pad, y + top + gap, contentWidth, available - top - gap);
    explorerSplitters_[0] = {x1, y, x1 + gap, y + top};
    explorerSplitters_[1] = {x2, y, x2 + gap, y + top};
    explorerSplitters_[2] = {pad, y + top, width - pad, y + top + gap};
    SetRectEmpty(&splitter_);
    move(status_, pad, height - scale(27), width - pad * 2, scale(22));
    for (int i = 0; i < 3; ++i)
      updateExplorerBar(i);
    return;
  }
  for (auto &rect : explorerSplitters_)
    SetRectEmpty(&rect);
  int fy = y;
  move(fileLabel_, pad, fy, left - pad, scale(22));
  fy += scale(25);
  move(files_, pad, fy, left - pad, height - fy - footerHeight);
  move(diff_.handle(), left + gap, y, width - left - gap - pad, height - y - footerHeight);
  splitter_ = {left, y, left + gap, height - footerHeight};
  move(status_, pad, height - scale(27), width - pad * 2, scale(22));
}
void MainWindow::drawSplitter(HDC dc) const
{
  if (explorerLayout_)
  {
    auto brush = CreateSolidBrush(themeColor(ThemeColor::Border));
    for (int i = 0; i < 3; ++i)
    {
      RECT line = explorerSplitters_[i];
      if (IsRectEmpty(&line))
        continue;
      if (i == 2)
        line.top = (line.top + line.bottom) / 2, line.bottom = line.top + std::max(1, MulDiv(1, dpi_, 96));
      else
        line.left = (line.left + line.right) / 2, line.right = line.left + std::max(1, MulDiv(1, dpi_, 96));
      FillRect(dc, &line, brush);
    }
    DeleteObject(brush);
    return;
  }
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
void MainWindow::setExplorerLayout(bool enabled)
{
  if (explorerLayout_ == enabled)
    return;
  int top = diff_.topRow();
  explorerLayout_ = enabled;
  tooltipIndex_ = -1;
  tooltipOwner_ = nullptr;
  SendMessageW(tooltip_, TTM_POP, 0, 0);
  SendMessageW(layoutButton_, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  if (enabled)
    rebuildExplorer();
  layout();
  diff_.scroll(top);
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
void MainWindow::redrawExplorerPanels()
{
  if (!explorerLayout_)
    return;
  RECT client{};
  GetClientRect(hwnd_, &client);
  RECT upper{0, explorerSplitters_[0].top, client.right, explorerSplitters_[2].bottom};
  RedrawWindow(hwnd_, &upper, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
  for (auto panel : {explorerCommitLabel_, explorerFilesLabel_, explorerMessageLabel_, explorerCommits_, explorerFiles_,
         explorerMessage_, explorerBars_[0], explorerBars_[1], explorerBars_[2]})
    RedrawWindow(panel, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
  ++explorerResizeRepaints_;
}
void MainWindow::moveExplorerSplitter(int index, int position)
{
  RECT client{};
  GetClientRect(hwnd_, &client);
  auto scale = [&](int n) { return MulDiv(n, static_cast<int>(dpi_), 96); };
  int pad = scale(12), gap = scale(8), minimum = scale(160);
  if (index == 0)
    explorerCommitWidth_ =
      MulDiv(std::clamp(position - pad, minimum, std::max(minimum, static_cast<int>(client.right) - 2 * pad - 2 * gap - 2 * minimum)),
        96, dpi_);
  else if (index == 1)
    explorerMessageWidth_ = MulDiv(
      std::clamp(position - static_cast<int>(explorerSplitters_[0].right), minimum,
        std::max(minimum, static_cast<int>(client.right) - pad - static_cast<int>(explorerSplitters_[0].right) - gap - minimum)),
      96, dpi_);
  else
    explorerTopHeight_ = MulDiv(std::clamp(position - static_cast<int>(explorerSplitters_[0].top), scale(140),
                                  std::max(scale(140), static_cast<int>(client.bottom) - scale(36) -
                                                         static_cast<int>(explorerSplitters_[0].top) - gap - scale(180))),
      96, dpi_);
  if ((index == 0 && scale(explorerCommitWidth_) == explorerSplitters_[0].left - pad) ||
      (index == 1 && scale(explorerMessageWidth_) == explorerSplitters_[1].left - explorerSplitters_[0].right) ||
      (index == 2 && scale(explorerTopHeight_) == explorerSplitters_[2].top - explorerSplitters_[0].top))
    return;
  layout();
  InvalidateRect(hwnd_, nullptr, TRUE);
}
void MainWindow::rebuildExplorer()
{
  if (!explorerCommits_)
    return;
  std::wstring oldKey = selectedListKey_;
  std::wstring oldPath = selectedPath_;
  SendMessageW(explorerCommits_, WM_SETREDRAW, FALSE, 0);
  SendMessageW(explorerCommits_, LB_RESETCONTENT, 0, 0);
  explorerGroups_.clear();
  auto add = [&](FileListItem item) {
    explorerGroups_.push_back(std::move(item));
    SendMessageW(explorerCommits_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(explorerGroups_.back().label.c_str()));
  };
  auto mode = source(source_);
  if (mode == ChangeSource::History)
  {
    for (const auto &item : fileListItems_)
      if (item.kind == FileListItemKind::Section || item.kind == FileListItemKind::Commit || item.kind == FileListItemKind::LoadMore)
        add(item);
  }
  else if (mode == ChangeSource::ReadyToPush || mode == ChangeSource::Range)
  {
    if (mode == ChangeSource::ReadyToPush)
    {
      FileListItem group{FileListItemKind::Section, nullptr, 0, 0, 0, L"Ready to push", L"ready-summary"};
      group.document = &snapshot_.document;
      add(std::move(group));
    }
    for (const auto &item : fileListItems_)
      if (item.kind == FileListItemKind::Commit || (mode == ChangeSource::Range && item.kind == FileListItemKind::Summary))
        add(item);
    if (explorerGroups_.empty())
    {
      FileListItem group{FileListItemKind::Section, nullptr, 0, 0, 0, L"Summary", L"summary"};
      group.document = &snapshot_.document;
      add(std::move(group));
    }
  }
  else
  {
    FileListItem group{FileListItemKind::Section, nullptr, 0, 0, 0,
      mode == ChangeSource::Commit     ? L"Commit"
      : mode == ChangeSource::Staged   ? L"Staged"
      : mode == ChangeSource::Unstaged ? L"Unstaged"
                                       : L"All local",
      L"explorer-summary"};
    group.document = &snapshot_.document;
    add(std::move(group));
  }
  SendMessageW(explorerCommits_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(explorerCommits_, nullptr, TRUE);
  updateExplorerBar(0);
  int selection = 0;
  for (size_t i = 0; i < explorerGroups_.size(); ++i)
  {
    const auto &group = explorerGroups_[i];
    std::wstring prefix = group.key + L"\n";
    if (group.key == oldKey || oldKey.rfind(prefix, 0) == 0 ||
        (group.group == FileListGroup::Unstaged && oldKey.rfind(L"history-unstaged\n", 0) == 0) ||
        (group.group == FileListGroup::Staged && oldKey.rfind(L"history-staged\n", 0) == 0) ||
        (group.group == FileListGroup::Outgoing && group.kind == FileListItemKind::Section &&
          oldKey.rfind(L"history-outgoing-summary\n", 0) == 0))
    {
      selection = static_cast<int>(i);
      break;
    }
  }
  if (!explorerGroups_.empty())
  {
    SendMessageW(explorerCommits_, LB_SETCURSEL, selection, 0);
    selectedListKey_ = oldKey;
    selectedPath_ = oldPath;
    selectExplorerGroup(selection, true);
  }
}
void MainWindow::selectExplorerGroup(int index, bool preserveFile, bool selectLastFile)
{
  if (index < 0 || static_cast<size_t>(index) >= explorerGroups_.size())
    return;
  auto &group = explorerGroups_[static_cast<size_t>(index)];
  if (group.kind == FileListItemKind::LoadMore)
  {
    loadMoreHistory();
    return;
  }
  const DiffDocument *document = group.document;
  if (!document && group.kind == FileListItemKind::Commit && group.commitIndex < snapshot_.commitDocuments.size())
    document = &snapshot_.commitDocuments[group.commitIndex];
  if (!document && group.kind == FileListItemKind::Summary)
    document = &snapshot_.document;
  std::wstring keepKey = preserveFile ? selectedListKey_ : L"";
  if (!preserveFile)
  {
    auto remembered = explorerFileSelections_.find(scrollContext_ + L"\n" + group.key);
    if (remembered != explorerFileSelections_.end())
      keepKey = remembered->second;
  }
  activeExplorerGroupKey_ = group.key;
  SendMessageW(explorerFiles_, WM_SETREDRAW, FALSE, 0);
  SendMessageW(explorerFiles_, LB_RESETCONTENT, 0, 0);
  explorerFileItems_.clear();
  size_t maxAdded = 0, maxRemoved = 0, added = 0, removed = 0;
  if (document)
    for (const auto &file : document->files)
    {
      auto counts = changes(file);
      added += counts.first;
      removed += counts.second;
      std::wstring key = group.key + L"\n" + file.path();
      auto old = fileItemIndex_.find(&file);
      if (old != fileItemIndex_.end())
        key = fileListItems_[static_cast<size_t>(old->second)].key;
      if (group.group == FileListGroup::Outgoing && group.kind == FileListItemKind::Section)
        key = L"history-outgoing-summary\n" + file.path();
      FileListItem item{FileListItemKind::File, &file, group.commitIndex, counts.first, counts.second,
        std::wstring(1, statusLetter(file.status)) + L"   " + file.path(), std::move(key)};
      item.group = group.group;
      item.document = document;
      item.commit = group.commit;
      maxAdded = std::max(maxAdded, item.added);
      maxRemoved = std::max(maxRemoved, item.removed);
      explorerFileItems_.push_back(std::move(item));
      SendMessageW(explorerFiles_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(explorerFileItems_.back().label.c_str()));
    }
  for (auto &item : explorerFileItems_)
  {
    item.maxAdded = maxAdded;
    item.maxRemoved = maxRemoved;
  }
  SendMessageW(explorerFiles_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(explorerFiles_, nullptr, TRUE);
  updateExplorerBar(1);
  size_t fileCount = document ? document->files.size() : 0;
  group.added = added;
  group.removed = removed;
  std::wstring message;
  if (group.commit)
    message = group.commit->message;
  else if (group.kind == FileListItemKind::Commit && group.commitIndex < snapshot_.commits.size())
    message = snapshot_.commits[group.commitIndex].message;
  else if (source(source_) == ChangeSource::Commit)
    message = snapshot_.commitMessage;
  else
  {
    message = group.label;
    if (group.group == FileListGroup::Outgoing && fileCount == 0 && !snapshot_.history.outgoingNotice.empty())
      message += L"\n\n" + snapshot_.history.outgoingNotice;
    if (group.kind == FileListItemKind::Section && group.group == FileListGroup::History)
      message += snapshot_.history.commits.empty()
                   ? L"\n\nNo commits in history."
                   : L"\n\n" + std::to_wstring(snapshot_.history.commits.size()) + L" commits loaded, newest first.";
  }
  if (group.kind != FileListItemKind::Section || group.group != FileListGroup::History)
    message += L"\n\n----------------------------------------\n" + std::to_wstring(fileCount) + L" files changed    -" +
               std::to_wstring(removed) + L" +" + std::to_wstring(added);
  std::wstring lines;
  for (wchar_t c : message)
  {
    if (c == L'\n' && (lines.empty() || lines.back() != L'\r'))
      lines += L'\r';
    lines += c;
  }
  SetWindowTextW(explorerMessage_, lines.c_str());
  SendMessageW(explorerMessage_, EM_SETSEL, 0, 0);
  updateExplorerBar(2);
  int selected = selectLastFile && !explorerFileItems_.empty() ? static_cast<int>(explorerFileItems_.size()) - 1 : 0;
  for (size_t i = 0; i < explorerFileItems_.size(); ++i)
    if (explorerFileItems_[i].key == keepKey)
    {
      selected = static_cast<int>(i);
      break;
    }
  if (!explorerFileItems_.empty())
  {
    SendMessageW(explorerFiles_, LB_SETCURSEL, selected, 0);
    selectExplorerFile(selected);
  }
  else
  {
    rememberFileScroll();
    selectedListKey_ = group.key;
    selectedPath_ = L"<<" + group.label + L">>";
    for (size_t i = 0; i < fileListItems_.size(); ++i)
      if (fileListItems_[i].key == group.key)
      {
        SendMessageW(files_, LB_SETCURSEL, i, 0);
        break;
      }
    diff_.setMessage(message);
    updateStatus();
  }
}
void MainWindow::selectExplorerFile(int index)
{
  if (index < 0 || static_cast<size_t>(index) >= explorerFileItems_.size())
    return;
  endPreview();
  rememberFileScroll();
  const auto &item = explorerFileItems_[static_cast<size_t>(index)];
  selectedListKey_ = item.key;
  if (!activeExplorerGroupKey_.empty())
    explorerFileSelections_[scrollContext_ + L"\n" + activeExplorerGroupKey_] = item.key;
  selectedPath_ = item.file->path();
  auto old = fileItemIndex_.find(item.file);
  if (old != fileItemIndex_.end() && fileListItems_[static_cast<size_t>(old->second)].key == item.key)
    SendMessageW(files_, LB_SETCURSEL, old->second, 0);
  const FileDiff *file = item.file;
  if (fullFile_)
  {
    auto found = fullFileDocuments_.find(selectedListKey_);
    if (found != fullFileDocuments_.end() && !found->second.files.empty())
      file = &found->second.files.front();
  }
  diff_.setFile(file);
  syncComments();
  auto saved = fileScrollPositions_.find(fileScrollKey(selectedListKey_));
  if (saved != fileScrollPositions_.end())
    diff_.scroll(saved->second);
  else if (fullFile_ && file != item.file)
    diff_.showFirstChange();
  if (fullFile_ && file == item.file)
    requestSelectedFullFile(item);
  updateStatus();
}
void MainWindow::sourceChanged()
{
  endPreview();
  loadMoreLoading_ = false;
  if (baseMode_ == ChangeSource::ReadyToPush)
    readyBase_ = getText(base_);
  else if (baseMode_ == ChangeSource::Range)
    rangeBase_ = getText(base_);
  auto nextMode = source(source_);
  if (nextMode == ChangeSource::ReadyToPush)
    SetWindowTextW(base_, readyBase_.c_str());
  else if (nextMode == ChangeSource::Range)
    SetWindowTextW(base_, rangeBase_.c_str());
  baseMode_ = nextMode;
  series_.clear();
  SendMessageW(commits_, CB_RESETCONTENT, 0, 0);
  layout();
}
void MainWindow::refresh(bool seriesSelection)
{
  endPreview();
  comments_.clear();
  diff_.setComments({});
  EnableWindow(copyCommentsButton_, FALSE);
  InvalidateRect(files_, nullptr, FALSE);
  InvalidateRect(explorerFiles_, nullptr, FALSE);
  if (directory_.empty())
  {
    diff_.setMessage(L"No repository found. Start gfd.exe from a repository folder.\n\nF5  "
                     L"Refresh\nCtrl+Shift+D  Toggle diff layout");
    return;
  }
  CompareRequest request{directory_, source(source_), getText(base_), getText(target_), false};
  if (request.source == ChangeSource::History)
    request.historyLimit = historyLimit_;
  if (seriesSelection)
  {
    auto index = SendMessageW(commits_, CB_GETCURSEL, 0, 0);
    if (index > 0 && static_cast<size_t>(index) <= series_.size())
    {
      request.source = ChangeSource::Commit;
      request.target = series_[static_cast<size_t>(index) - 1].id;
    }
  }
  loading_ = true;
  startStatusAnimation();
  SetWindowTextW(status_, L"Loading Git changes… You can change the source or refresh again.");
  controller_->request(std::move(request));
}
void MainWindow::loadMoreHistory()
{
  if (loading_ || source(source_) != ChangeSource::History || !snapshot_.history.hasMore || snapshot_.history.initialHead.empty())
    return;
  endPreview();
  rememberFileScroll();
  historyListTop_ = static_cast<int>(SendMessageW(files_, LB_GETTOPINDEX, 0, 0));
  historyExplorerTop_ = static_cast<int>(SendMessageW(explorerCommits_, LB_GETTOPINDEX, 0, 0));
  historyDiffTop_ = diff_.topRow();
  CompareRequest request{directory_, ChangeSource::History, {}, {}, false};
  request.historyLimit = 10;
  request.historySkip = snapshot_.history.nextSkip;
  request.historyHead = snapshot_.history.initialHead;
  request.historyAppend = true;
  request.historyExcludedCommits.reserve(snapshot_.history.outgoingCommits.size());
  for (const auto &commit : snapshot_.history.outgoingCommits)
    request.historyExcludedCommits.push_back(commit.id);
  loading_ = true;
  startStatusAnimation();
  loadMoreLoading_ = true;
  for (auto &item : fileListItems_)
    if (item.kind == FileListItemKind::LoadMore)
      item.label = L"Loading...";
  SetWindowTextW(status_, L"Loading more commits...");
  InvalidateRect(files_, nullptr, FALSE);
  controller_->request(std::move(request));
}
void MainWindow::loaded()
{
  auto result = controller_->takeResult();
  if (!result)
    return;
  loading_ = false;
  stopStatusAnimation();
  endPreview();
  if (result->request.selectedOnly)
  {
    if (fullFileLoadingKey_ == result->request.selectionKey)
      fullFileLoadingKey_.clear();
    if (!result->error.empty())
    {
      SetWindowTextW(status_, (L"Unable to load the full file: " + result->error).c_str());
      return;
    }
    if (result->snapshot.document.files.empty())
    {
      SetWindowTextW(status_, L"Git returned no diff for the selected file.");
      return;
    }
    auto document = fullFileDocuments_.insert_or_assign(result->request.selectionKey, std::move(result->snapshot.document)).first;
    if (fullFile_ && selectedListKey_ == result->request.selectionKey && !document->second.files.empty())
    {
      diff_.setFile(&document->second.files.front());
      syncComments();
      auto saved = fileScrollPositions_.find(fileScrollKey(selectedListKey_));
      if (saved != fileScrollPositions_.end())
        diff_.scroll(saved->second);
      else
        diff_.showFirstChange();
    }
    updateStatus();
    return;
  }
  bool initial = initialLoad_;
  initialLoad_ = false;
  if (!result->error.empty())
  {
    if (result->request.historyAppend)
    {
      loadMoreLoading_ = false;
      for (auto &item : fileListItems_)
        if (item.kind == FileListItemKind::LoadMore)
          item.label = L"Load more";
      for (size_t i = 0; i < fileListItems_.size(); ++i)
        if (fileListItems_[i].key == selectedListKey_)
        {
          SendMessageW(files_, LB_SETCURSEL, i, 0);
          break;
        }
      SendMessageW(files_, LB_SETTOPINDEX, historyListTop_, 0);
      InvalidateRect(files_, nullptr, FALSE);
      SetWindowTextW(status_, (L"Unable to load more commits: " + result->error).c_str());
      return;
    }
    rememberFileScroll();
    loadMoreLoading_ = false;
    diff_.setMessage(L"Unable to load changes\n\n" + result->error);
    fileListItems_.clear();
    fileItemIndex_.clear();
    explorerGroups_.clear();
    explorerFileItems_.clear();
    SendMessageW(explorerCommits_, LB_RESETCONTENT, 0, 0);
    SendMessageW(explorerFiles_, LB_RESETCONTENT, 0, 0);
    fullFileDocuments_.clear();
    fullFileLoadingKey_.clear();
    snapshot_ = {};
    scrollContext_.clear();
    explorerFileSelections_.clear();
    activeExplorerGroupKey_.clear();
    selectedPath_.clear();
    selectedListKey_.clear();
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
  rememberFileScroll();
  diff_.setFile(nullptr);
  explorerGroups_.clear();
  explorerFileItems_.clear();
  SendMessageW(explorerCommits_, LB_RESETCONTENT, 0, 0);
  SendMessageW(explorerFiles_, LB_RESETCONTENT, 0, 0);
  fileListItems_.clear();
  fileItemIndex_.clear();
  bool historyAppend = result->request.historyAppend;
  if (!historyAppend)
  {
    loadMoreLoading_ = false;
    fullFileDocuments_.clear();
    fullFileLoadingKey_.clear();
  }
  if (historyAppend)
  {
    auto &page = result->snapshot.history;
    snapshot_.history.commits.insert(snapshot_.history.commits.end(), std::make_move_iterator(page.commits.begin()),
      std::make_move_iterator(page.commits.end()));
    snapshot_.history.commitDocuments.insert(snapshot_.history.commitDocuments.end(),
      std::make_move_iterator(page.commitDocuments.begin()), std::make_move_iterator(page.commitDocuments.end()));
    snapshot_.history.hasMore = page.hasMore;
    snapshot_.history.nextSkip = page.nextSkip;
    historyLimit_ += 10;
    loadMoreLoading_ = false;
  }
  else
    snapshot_ = std::move(result->snapshot);
  scrollContext_ = snapshot_.root + L"\n" + std::to_wstring(static_cast<int>(result->request.source));
  if (result->request.source != ChangeSource::History)
    scrollContext_ += L"\n" + result->request.base + L"\n" + result->request.target;
  messageReturnIndex_ = -1;
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
          selectedListKey_ = L"commit\n" + id;
          break;
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
  fileListItems_.clear();
  fileItemIndex_.clear();
  int selected = -1, fallback = -1;
  auto addItem = [&](FileListItem item) {
    int index = static_cast<int>(fileListItems_.size());
    SendMessageW(files_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
    if (!item.key.empty() && item.key == selectedListKey_)
      selected = index;
    if (item.file)
      fileItemIndex_[item.file] = index;
    fileListItems_.push_back(std::move(item));
    return index;
  };
  auto addFile = [&](const FileDiff &file, std::wstring key, size_t commitIndex = 0, FileListGroup group = FileListGroup::None,
                   const DiffDocument *document = nullptr, const Commit *commit = nullptr) {
    std::wstring label(1, statusLetter(file.status));
    label += L"   " + file.path();
    if (file.binary && file.status != FileStatus::Binary)
      label += L"  [binary]";
    auto [added, removed] = changes(file);
    FileListItem item{FileListItemKind::File, &file, commitIndex, added, removed, std::move(label), std::move(key)};
    item.group = group;
    item.document = document;
    item.commit = commit;
    return addItem(std::move(item));
  };
  commitMessageFile_ = {};
  if (!snapshot_.commitId.empty())
  {
    commitMessageFile_.newPath = L"<<Commit Message>>";
    std::wistringstream message(snapshot_.commitMessage);
    std::wstring line;
    while (std::getline(message, line))
      commitMessageFile_.metadata.push_back(line.empty() ? L" " : line);
    fallback = addItem({FileListItemKind::CommitMessage, nullptr, 0, 0, 0, L"<<Commit Message>>", L"message"});
  }
  bool history = result->request.source == ChangeSource::History;
  bool grouped = result->request.source == ChangeSource::Range || result->request.source == ChangeSource::ReadyToPush;
  if (history)
  {
    auto addSection = [&](const wchar_t *label, const wchar_t *key, FileListGroup group, const DiffDocument *document) {
      FileListItem item{FileListItemKind::Section, nullptr, 0, 0, 0, label, key};
      if (document)
      {
        auto [added, removed] = changes(*document);
        item.added = added;
        item.removed = removed;
      }
      item.group = group;
      item.document = document;
      int index = addItem(std::move(item));
      if (fallback < 0)
        fallback = index;
    };
    auto addSpacer = [&](const wchar_t *key) { addItem({FileListItemKind::Spacer, nullptr, 0, 0, 0, L"", key}); };
    addSection(L"Unstaged", L"history-section\nunstaged", FileListGroup::Unstaged, &snapshot_.history.unstaged);
    if (snapshot_.history.unstaged.files.empty())
      addItem({FileListItemKind::Notice, nullptr, 0, 0, 0, L"No unstaged changes.", L"history-empty-unstaged"});
    else
      for (const auto &file : snapshot_.history.unstaged.files)
        addFile(file, L"history-unstaged\n" + file.path(), 0, FileListGroup::Unstaged, &snapshot_.history.unstaged);
    addSpacer(L"history-spacer-unstaged");

    addSection(L"Staged", L"history-section\nstaged", FileListGroup::Staged, &snapshot_.history.staged);
    if (snapshot_.history.staged.files.empty())
      addItem({FileListItemKind::Notice, nullptr, 0, 0, 0, L"No staged changes.", L"history-empty-staged"});
    else
      for (const auto &file : snapshot_.history.staged.files)
        addFile(file, L"history-staged\n" + file.path(), 0, FileListGroup::Staged, &snapshot_.history.staged);
    addSpacer(L"history-spacer-staged");

    addSection(L"Ready to push", L"history-section\noutgoing", FileListGroup::Outgoing, &snapshot_.history.outgoing);
    size_t outgoingCount = std::min(snapshot_.history.outgoingCommits.size(), snapshot_.history.outgoingDocuments.size());
    if (!outgoingCount)
      addItem({FileListItemKind::Notice, nullptr, 0, 0, 0,
        snapshot_.history.outgoingNotice.empty() ? L"No commits ready to push." : snapshot_.history.outgoingNotice,
        L"history-empty-outgoing"});
    for (size_t commitIndex = 0; commitIndex < outgoingCount; ++commitIndex)
    {
      const auto &commit = snapshot_.history.outgoingCommits[commitIndex];
      const auto &document = snapshot_.history.outgoingDocuments[commitIndex];
      auto [added, removed] = changes(document);
      FileListItem header{FileListItemKind::Commit, nullptr, commitIndex, added, removed,
        commit.id.substr(0, 8) + L"   " + commit.subject, L"outgoing\n" + commit.id};
      header.group = FileListGroup::Outgoing;
      header.document = &document;
      header.commit = &commit;
      addItem(std::move(header));
      for (const auto &file : document.files)
        addFile(file, L"outgoing\n" + commit.id + L"\n" + file.path(), commitIndex, FileListGroup::Outgoing, &document, &commit);
    }
    addSpacer(L"history-spacer-outgoing");

    addSection(L"History", L"history-section\ncommits", FileListGroup::History, nullptr);
    size_t historyCount = std::min(snapshot_.history.commits.size(), snapshot_.history.commitDocuments.size());
    for (size_t commitIndex = 0; commitIndex < historyCount; ++commitIndex)
    {
      const auto &commit = snapshot_.history.commits[commitIndex];
      const auto &document = snapshot_.history.commitDocuments[commitIndex];
      auto [added, removed] = changes(document);
      FileListItem header{FileListItemKind::Commit, nullptr, commitIndex, added, removed,
        commit.id.substr(0, 8) + L"   " + commit.subject, L"history\n" + commit.id};
      header.group = FileListGroup::History;
      header.document = &document;
      header.commit = &commit;
      addItem(std::move(header));
      for (const auto &file : document.files)
        addFile(file, L"history\n" + commit.id + L"\n" + file.path(), commitIndex, FileListGroup::History, &document, &commit);
    }
    if (snapshot_.history.hasMore)
      addItem({FileListItemKind::LoadMore, nullptr, 0, 0, 0, L"Load more", L"history-load-more"});
  }
  else if (grouped)
  {
    size_t count = std::min(snapshot_.commits.size(), snapshot_.commitDocuments.size());
    for (size_t commitIndex = 0; commitIndex < count; ++commitIndex)
    {
      const auto &commit = snapshot_.commits[commitIndex];
      auto key = L"commit\n" + commit.id;
      auto [added, removed] = changes(snapshot_.commitDocuments[commitIndex]);
      int header = addItem(
        {FileListItemKind::Commit, nullptr, commitIndex, added, removed, commit.id.substr(0, 8) + L"   " + commit.subject, key});
      if (fallback < 0)
        fallback = header;
      for (const auto &file : snapshot_.commitDocuments[commitIndex].files)
      {
        int index = addFile(file, key + L"\n" + file.path(), commitIndex);
        if (count == 1)
        {
          if (fallback == header)
            fallback = index;
          if (selected < 0 && file.path() == selectedPath_)
            selected = index;
        }
      }
    }
    if (count > 1)
    {
      if (count)
        addItem({FileListItemKind::Spacer, nullptr, 0, 0, 0, L"", L"range-spacer"});
      int summary = addItem({FileListItemKind::Summary, nullptr, 0, 0, 0, L"Summary", L"summary"});
      fallback = summary;
      for (const auto &file : snapshot_.document.files)
      {
        int index = addFile(file, L"summary\n" + file.path());
        if (fallback == summary)
          fallback = index;
        if (selected < 0 && file.path() == selectedPath_)
          selected = index;
      }
    }
  }
  else
    for (const auto &file : snapshot_.document.files)
    {
      int index = addFile(file, L"file\n" + file.path());
      if (fallback < 0)
        fallback = index;
      if (selected < 0 && file.path() == selectedPath_)
        selected = index;
    }
  // File rows form one contiguous run per commit, section, or summary.
  for (size_t first = 0; first < fileListItems_.size();)
  {
    if (fileListItems_[first].kind != FileListItemKind::File)
    {
      ++first;
      continue;
    }
    size_t last = first, maxAdded = 0, maxRemoved = 0;
    while (last < fileListItems_.size() && fileListItems_[last].kind == FileListItemKind::File)
    {
      maxAdded = std::max(maxAdded, fileListItems_[last].added);
      maxRemoved = std::max(maxRemoved, fileListItems_[last].removed);
      ++last;
    }
    for (size_t index = first; index < last; ++index)
    {
      fileListItems_[index].maxAdded = maxAdded;
      fileListItems_[index].maxRemoved = maxRemoved;
    }
    first = last;
  }
  SendMessageW(files_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(files_, nullptr, TRUE);
  if (!fileListItems_.empty())
  {
    if (selected < 0)
      selected = std::max(0, fallback);
    SendMessageW(files_, LB_SETCURSEL, selected, 0);
    selectFile();
    if (historyAppend)
    {
      diff_.scroll(historyDiffTop_);
      SendMessageW(files_, LB_SETTOPINDEX, historyListTop_, 0);
    }
  }
  else
  {
    selectedPath_.clear();
    selectedListKey_.clear();
    diff_.setMessage(snapshot_.notice.empty() ? L"No changes in this comparison.\n\nStaged shows the index; Unstaged shows "
                                                L"tracked working-tree edits.\nNew untracked files appear after git add."
                                              : snapshot_.notice);
  }
  if (!loading_)
    updateStatus();
  layout();
  if (explorerLayout_)
  {
    rebuildExplorer();
    if (historyAppend)
    {
      diff_.scroll(historyDiffTop_);
      SendMessageW(explorerCommits_, LB_SETTOPINDEX, historyExplorerTop_, 0);
      updateExplorerBar(0);
    }
  }
}
void MainWindow::navigateList(int direction, bool focusDiff)
{
  if (focusDiff)
    SetFocus(diff_.handle());
  if (loading_ || fileListItems_.empty() || !direction)
    return;
  if (explorerLayout_)
  {
    int group = static_cast<int>(SendMessageW(explorerCommits_, LB_GETCURSEL, 0, 0));
    int file = static_cast<int>(SendMessageW(explorerFiles_, LB_GETCURSEL, 0, 0));
    int count = static_cast<int>(explorerFileItems_.size());
    if ((direction > 0 && file + 1 < count) || (direction < 0 && file > 0))
    {
      int next = file + direction;
      SendMessageW(explorerFiles_, LB_SETCURSEL, next, 0);
      revealListSelection(explorerFiles_, next);
      updateExplorerBar(1);
      selectExplorerFile(next);
    }
    else
    {
      int next = group + direction;
      if (next < 0 || static_cast<size_t>(next) >= explorerGroups_.size() ||
          explorerGroups_[static_cast<size_t>(next)].kind == FileListItemKind::LoadMore)
        return;
      SendMessageW(explorerCommits_, LB_SETCURSEL, next, 0);
      revealListSelection(explorerCommits_, next);
      updateExplorerBar(0);
      selectExplorerGroup(next, false, direction < 0);
    }
    return;
  }
  int current = static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0));
  if (current < 0)
    current = direction < 0 ? static_cast<int>(fileListItems_.size()) : -1;
  for (int next = current + (direction < 0 ? -1 : 1); next >= 0 && next < static_cast<int>(fileListItems_.size());
       next += direction < 0 ? -1 : 1)
    if (fileListItems_[static_cast<size_t>(next)].kind != FileListItemKind::Spacer &&
        fileListItems_[static_cast<size_t>(next)].kind != FileListItemKind::Notice)
    {
      SendMessageW(files_, LB_SETCURSEL, next, 0);
      if (fileListItems_[static_cast<size_t>(next)].kind == FileListItemKind::LoadMore)
        updateStatus();
      else
        selectFile();
      return;
    }
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
  rememberFileScroll();
  int index = static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0));
  if (index < 0 || static_cast<size_t>(index) >= fileListItems_.size())
    return;
  const auto &item = fileListItems_[static_cast<size_t>(index)];
  if (item.kind == FileListItemKind::LoadMore)
  {
    loadMoreHistory();
    return;
  }
  if (item.kind == FileListItemKind::Spacer)
  {
    auto selectable = [&](int candidate) {
      auto kind = fileListItems_[static_cast<size_t>(candidate)].kind;
      return kind != FileListItemKind::Spacer && kind != FileListItemKind::Notice;
    };
    int replacement = -1;
    for (int next = index + 1; next < static_cast<int>(fileListItems_.size()); ++next)
      if (selectable(next))
      {
        replacement = next;
        break;
      }
    for (int previous = index - 1; replacement < 0 && previous >= 0; --previous)
      if (selectable(previous))
      {
        replacement = previous;
        break;
      }
    if (replacement >= 0)
    {
      SendMessageW(files_, LB_SETCURSEL, replacement, 0);
      selectFile();
    }
    return;
  }
  if (item.kind == FileListItemKind::Notice)
  {
    for (int previous = index - 1; previous >= 0; --previous)
      if (fileListItems_[static_cast<size_t>(previous)].kind == FileListItemKind::Section)
      {
        SendMessageW(files_, LB_SETCURSEL, previous, 0);
        selectFile();
        break;
      }
    return;
  }
  selectedListKey_ = item.key;
  diff_.setComments({});
  updateStatus();
  if (item.kind == FileListItemKind::CommitMessage)
  {
    selectedPath_ = L"<<Commit Message>>";
    diff_.setFile(&commitMessageFile_, false, true);
    return;
  }
  if (item.kind == FileListItemKind::Commit)
  {
    const auto &commit = item.commit ? *item.commit : snapshot_.commits[item.commitIndex];
    selectedPath_ = L"<<Commit Message>>";
    listMessageFile_ = {};
    listMessageFile_.newPath = L"<<Commit Message>> - " + commit.id.substr(0, 8);
    std::wistringstream message(commit.message);
    std::wstring line;
    while (std::getline(message, line))
      listMessageFile_.metadata.push_back(line.empty() ? L" " : line);
    diff_.setFile(&listMessageFile_, false, true);
    return;
  }
  if (item.kind == FileListItemKind::Section)
  {
    selectedPath_ = L"<<" + item.label + L">>";
    listMessageFile_ = {};
    listMessageFile_.newPath = selectedPath_;
    listMessageFile_.metadata = {item.label, L" "};
    const auto *document = item.document;
    if (document && !document->files.empty())
    {
      size_t added = 0, removed = 0;
      for (const auto &file : document->files)
      {
        auto counts = changes(file);
        added += counts.first;
        removed += counts.second;
      }
      listMessageFile_.metadata.push_back(std::to_wstring(document->files.size()) + L" files changed, +" + std::to_wstring(added) +
                                          L", -" + std::to_wstring(removed) + L".");
    }
    else if (item.group == FileListGroup::Unstaged)
      listMessageFile_.metadata.push_back(L"No unstaged changes.");
    else if (item.group == FileListGroup::Staged)
      listMessageFile_.metadata.push_back(L"No staged changes.");
    else if (item.group == FileListGroup::History)
      listMessageFile_.metadata.push_back(snapshot_.history.commits.empty()
                                            ? L"No commits in history."
                                            : std::to_wstring(snapshot_.history.commits.size()) + L" commits loaded, newest first.");
    else if (!snapshot_.history.outgoingNotice.empty())
      listMessageFile_.metadata.push_back(snapshot_.history.outgoingNotice);
    else
      listMessageFile_.metadata.push_back(L"No commits ready to push.");
    const std::vector<Commit> *sectionCommits = nullptr;
    if (item.group == FileListGroup::Outgoing)
      sectionCommits = &snapshot_.history.outgoingCommits;
    else if (item.group == FileListGroup::History)
      sectionCommits = &snapshot_.history.commits;
    if (sectionCommits && !sectionCommits->empty())
    {
      listMessageFile_.metadata.insert(listMessageFile_.metadata.end(), {L" ", L"Commits:"});
      for (size_t commitIndex = 0; commitIndex < sectionCommits->size(); ++commitIndex)
      {
        if (commitIndex)
        {
          listMessageFile_.metadata.push_back(L" ");
          listMessageFile_.metadata.push_back(std::wstring(80, L'_'));
        }
        listMessageFile_.metadata.push_back(L" ");
        const auto &commit = (*sectionCommits)[commitIndex];
        std::wistringstream message(commit.message);
        std::wstring line;
        while (std::getline(message, line))
          listMessageFile_.metadata.push_back(line.empty() ? L" " : line);
      }
    }
    diff_.setFile(&listMessageFile_, false, true);
    return;
  }
  if (item.kind == FileListItemKind::Summary)
  {
    selectedPath_ = L"<<Summary>>";
    listMessageFile_ = {};
    listMessageFile_.newPath = L"<<Summary>>";
    listMessageFile_.metadata = {L"Summary", L" ", L"Combined changes from " + snapshot_.base + L".", L" ", L"Commits:"};
    for (const auto &commit : snapshot_.commits)
      listMessageFile_.metadata.push_back(commit.id.substr(0, 8) + L"  " + commit.subject);
    diff_.setFile(&listMessageFile_, false, true);
    return;
  }
  if (item.file)
  {
    selectedPath_ = item.file->path();
    const FileDiff *file = item.file;
    if (fullFile_)
    {
      auto cached = fullFileDocuments_.find(selectedListKey_);
      if (cached != fullFileDocuments_.end() && !cached->second.files.empty())
        file = &cached->second.files.front();
    }
    diff_.setFile(file);
    syncComments();
    auto saved = fileScrollPositions_.find(fileScrollKey(selectedListKey_));
    if (saved != fileScrollPositions_.end())
      diff_.scroll(saved->second);
    else if (fullFile_ && file != item.file)
      diff_.showFirstChange();
    if (fullFile_ && file == item.file)
      requestSelectedFullFile(item);
  }
}
void MainWindow::syncComments()
{
  std::vector<ReviewComment> visible;
  if (diff_.file() && !diff_.plainText())
    for (const auto &comment : comments_)
      if (comment.key == selectedListKey_)
        visible.push_back(comment);
  diff_.setComments(std::move(visible));
}
void MainWindow::copyComments()
{
  if (!comments_.empty())
    copyFileName(hwnd_, formatReviewComments(comments_));
}
void MainWindow::openCommentEditor()
{
  if (diff_.plainText() || !diff_.file())
    return;
  const FileListItem *item = nullptr;
  if (explorerLayout_)
  {
    int index = static_cast<int>(SendMessageW(explorerFiles_, LB_GETCURSEL, 0, 0));
    if (index >= 0 && static_cast<size_t>(index) < explorerFileItems_.size())
      item = &explorerFileItems_[static_cast<size_t>(index)];
  }
  else
  {
    int index = static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0));
    if (index >= 0 && static_cast<size_t>(index) < fileListItems_.size())
      item = &fileListItems_[static_cast<size_t>(index)];
  }
  if (!item || !item->file || item->key != selectedListKey_)
    return;
  auto selected = diff_.selectedNewLines();
  int selectedAnnotation = diff_.selectedComment();
  size_t existing = comments_.size(), visible = 0;
  for (size_t index = 0; index < comments_.size(); ++index)
  {
    const auto &comment = comments_[index];
    if (comment.key != item->key)
      continue;
    bool intersects = selected && selected->first <= comment.lastLine && selected->second >= comment.firstLine;
    if (intersects || selectedAnnotation == static_cast<int>(visible))
    {
      existing = index;
      break;
    }
    ++visible;
  }
  if (existing == comments_.size() && !selected)
    return;
  const std::wstring *oldText = existing < comments_.size() ? &comments_[existing].text : nullptr;
  auto edit = editReviewComment(hwnd_, instance_, oldText);
  if (edit.action == CommentEditAction::Cancel)
    return;
  if (edit.action == CommentEditAction::Delete)
    comments_.erase(comments_.begin() + static_cast<std::ptrdiff_t>(existing));
  else if (existing < comments_.size())
    comments_[existing].text = std::move(edit.text);
  else
  {
    ReviewComment comment;
    comment.key = item->key;
    comment.path = item->file->path();
    comment.firstLine = selected->first;
    comment.lastLine = selected->second;
    const Commit *commit = item->commit;
    if (!commit && item->key.rfind(L"commit\n", 0) == 0 && item->commitIndex < snapshot_.commits.size())
      commit = &snapshot_.commits[item->commitIndex];
    if (commit)
    {
      comment.hash = commit->id;
      comment.changeId = changeIdFromMessage(commit->message);
    }
    else if (!snapshot_.commitId.empty())
    {
      comment.hash = snapshot_.commitId;
      comment.changeId = changeIdFromMessage(snapshot_.commitMessage);
    }
    if (!comment.hash.empty())
      comment.branch = snapshot_.branch;
    for (const auto &hunk : diff_.file()->hunks)
    {
      for (const auto &line : hunk.lines)
      {
        if (!line.newLine || *line.newLine < comment.firstLine || *line.newLine > comment.lastLine)
          continue;
        int nonspace = 0;
        for (wchar_t c : line.text)
          nonspace += !iswspace(c);
        if (nonspace > 1)
        {
          comment.excerpt = line.text;
          break;
        }
      }
      if (!comment.excerpt.empty())
        break;
    }
    comment.text = std::move(edit.text);
    comments_.push_back(std::move(comment));
  }
  syncComments();
  EnableWindow(copyCommentsButton_, !comments_.empty());
  InvalidateRect(files_, nullptr, FALSE);
  InvalidateRect(explorerFiles_, nullptr, FALSE);
}

void MainWindow::requestSelectedFullFile(const FileListItem &item)
{
  if (!item.file || item.key.empty())
    return;
  CompareRequest request{directory_, source(source_), getText(base_), getText(target_), true};
  request.path = item.file->path();
  request.selectionKey = item.key;
  request.selectedOnly = true;
  if (!snapshot_.commitId.empty())
  {
    request.source = ChangeSource::Commit;
    request.base.clear();
    request.target = snapshot_.commitId;
  }
  else if (item.group == FileListGroup::Unstaged)
  {
    request.source = ChangeSource::Unstaged;
    request.base.clear();
    request.target.clear();
  }
  else if (item.group == FileListGroup::Staged)
  {
    request.source = ChangeSource::Staged;
    request.base.clear();
    request.target.clear();
  }
  else if ((item.group == FileListGroup::Outgoing || item.group == FileListGroup::History) && item.commit)
  {
    request.source = ChangeSource::Commit;
    request.base.clear();
    request.target = item.commit->id;
  }
  else if (item.key.rfind(L"commit\n", 0) == 0 && item.commitIndex < snapshot_.commits.size())
  {
    request.source = ChangeSource::Commit;
    request.base.clear();
    request.target = snapshot_.commits[item.commitIndex].id;
  }
  fullFileLoadingKey_ = item.key;
  loading_ = true;
  startStatusAnimation();
  SetWindowTextW(status_, L"Loading the selected full file...");
  controller_->request(std::move(request));
}
void MainWindow::updateStatus()
{
  const DiffDocument *document = &snapshot_.document;
  if (explorerLayout_)
  {
    int group = static_cast<int>(SendMessageW(explorerCommits_, LB_GETCURSEL, 0, 0));
    if (group >= 0 && static_cast<size_t>(group) < explorerGroups_.size())
    {
      const auto &item = explorerGroups_[static_cast<size_t>(group)];
      if (item.document)
        document = item.document;
      else if (item.kind == FileListItemKind::Commit && item.commitIndex < snapshot_.commitDocuments.size())
        document = &snapshot_.commitDocuments[item.commitIndex];
    }
  }
  int index = static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0));
  if (!explorerLayout_ && index >= 0 && static_cast<size_t>(index) < fileListItems_.size())
  {
    const auto &item = fileListItems_[static_cast<size_t>(index)];
    if (item.document)
      document = item.document;
    bool commitItem =
      item.kind == FileListItemKind::Commit || (item.kind == FileListItemKind::File && item.key.rfind(L"commit\n", 0) == 0);
    if (!item.document && commitItem && item.commitIndex < snapshot_.commitDocuments.size())
      document = &snapshot_.commitDocuments[item.commitIndex];
  }
  size_t added = 0, removed = 0;
  bool historySection = false;
  int group = static_cast<int>(SendMessageW(explorerCommits_, LB_GETCURSEL, 0, 0));
  if (explorerLayout_ && group >= 0 && static_cast<size_t>(group) < explorerGroups_.size())
  {
    const auto &item = explorerGroups_[static_cast<size_t>(group)];
    added = item.added;
    removed = item.removed;
    historySection = item.kind == FileListItemKind::Section && item.group == FileListGroup::History;
  }
  else
    for (const auto &file : document->files)
    {
      auto counts = changes(file);
      added += counts.first;
      removed += counts.second;
    }
  std::wstring status;
  if (historySection)
    status = snapshot_.history.commits.empty() ? L"No commits in history."
                                               : std::to_wstring(snapshot_.history.commits.size()) + L" commits loaded";
  else
    status =
      std::to_wstring(document->files.size()) + L" files changed    -" + std::to_wstring(removed) + L"    +" + std::to_wstring(added);
  if (!snapshot_.notice.empty())
    status += L"    " + snapshot_.notice;
  else
    status += L"    |    F5 Refresh | F Full file | Ctrl+PgUp/PgDn Change | Ctrl+Down/Up List | Space Commit message | Ctrl+Shift+D "
              L"View | C Comment | F2 Copy comments | Ctrl+C Copy";
  SetWindowTextW(status_, status.c_str());
}
void MainWindow::rememberFileScroll()
{
  if (!scrollContext_.empty() && !selectedListKey_.empty() && diff_.file() && !diff_.plainText() &&
      (!fullFile_ || fullFileLoadingKey_ != selectedListKey_))
    fileScrollPositions_[fileScrollKey(selectedListKey_)] = diff_.topRow();
}
std::wstring MainWindow::fileScrollKey(const std::wstring &path) const
{
  return scrollContext_ + (fullFile_ ? L"\nfull\n" : L"\ncompact\n") + path;
}
void MainWindow::toggle()
{
  side_ = !side_;
  diff_.setSideBySide(side_);
  SendMessageW(view_, BM_SETCHECK, side_ ? BST_CHECKED : BST_UNCHECKED, 0);
}
void MainWindow::toggleFullFile()
{
  bool enabled = SendMessageW(fullFileButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
  if (enabled == fullFile_)
    return;
  rememberFileScroll();
  diff_.setFile(nullptr);
  fullFile_ = enabled;
  diff_.setChangeMinimap(fullFile_);
  selectFile();
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
  syncComments();
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
  if (item.CtlID != Files && item.CtlID != ExplorerCommits && item.CtlID != ExplorerFiles && item.CtlID != Commits &&
      item.CtlID != Source)
    return;
  int saved = SaveDC(item.hDC);
  RECT client{};
  GetClientRect(item.hwndItem, &client);
  IntersectClipRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right, item.rcItem.bottom);
  if (item.CtlID == Files || item.CtlID == ExplorerCommits || item.CtlID == ExplorerFiles)
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
    bool combo = item.CtlID == Commits || item.CtlID == Source;
    const auto *items = item.CtlID == Files             ? &fileListItems_
                        : item.CtlID == ExplorerCommits ? &explorerGroups_
                        : item.CtlID == ExplorerFiles   ? &explorerFileItems_
                                                        : nullptr;
    const FileListItem *listItem = items && item.itemID < items->size() ? &(*items)[item.itemID] : nullptr;
    if (listItem && listItem->kind == FileListItemKind::Spacer)
    {
      RestoreDC(item.hDC, saved);
      return;
    }
    auto length = SendMessageW(item.hwndItem, combo ? CB_GETLBTEXTLEN : LB_GETTEXTLEN, item.itemID, 0);
    if (length >= 0)
    {
      std::wstring value(static_cast<size_t>(length) + 1, L'\0');
      SendMessageW(item.hwndItem, combo ? CB_GETLBTEXT : LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(value.data()));
      value.resize(static_cast<size_t>(length));
      int pad = MulDiv(6, static_cast<int>(dpi_), 96);
      if (listItem && listItem->kind == FileListItemKind::LoadMore)
      {
        if (loadMoreLoading_)
          value = L"Loading...";
        RECT button = item.rcItem;
        InflateRect(&button, -MulDiv(12, static_cast<int>(dpi_), 96), -MulDiv(2, static_cast<int>(dpi_), 96));
        auto buttonBrush = CreateSolidBrush(themeColor(selected ? ThemeColor::ListSelection : ThemeColor::Metadata));
        FillRect(item.hDC, &button, buttonBrush);
        DeleteObject(buttonBrush);
        auto border = CreateSolidBrush(themeColor(ThemeColor::Border));
        FrameRect(item.hDC, &button, border);
        DeleteObject(border);
        DrawTextW(item.hDC, value.c_str(), static_cast<int>(value.size()), &button, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        if (item.itemState & ODS_FOCUS)
        {
          InflateRect(&button, -2, -2);
          DrawFocusRect(item.hDC, &button);
        }
        RestoreDC(item.hDC, saved);
        return;
      }
      RECT textRect = item.rcItem;
      textRect.left += pad;
      textRect.right -= pad;
      if (listItem && listItem->kind == FileListItemKind::Notice)
      {
        textRect.left += MulDiv(18, static_cast<int>(dpi_), 96);
        SetTextColor(item.hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::MutedText));
      }
      bool commitHeader = listItem && listItem->kind == FileListItemKind::Commit;
      bool groupHeader = listItem && (listItem->kind == FileListItemKind::Summary || listItem->kind == FileListItemKind::Section);
      bool sectionHeader = commitHeader || groupHeader;
      if (sectionHeader)
      {
        if (groupHeader)
          SelectObject(item.hDC, boldFont_ ? boldFont_ : font_);
        RECT separator{textRect.left, item.rcItem.top, textRect.right, item.rcItem.top + 1};
        auto brush = CreateSolidBrush(themeColor(ThemeColor::Border));
        FillRect(item.hDC, &separator, brush);
        DeleteObject(brush);
        textRect.top += MulDiv(2, static_cast<int>(dpi_), 96);
        SetTextColor(item.hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Title));
      }
      bool sectionTotals = listItem && listItem->kind == FileListItemKind::Section &&
                           (listItem->group == FileListGroup::Unstaged || listItem->group == FileListGroup::Staged ||
                             listItem->group == FileListGroup::Outgoing);
      bool commitTotals = false;
      if (commitHeader)
      {
        SIZE zeroWidth{};
        GetTextExtentPoint32W(item.hDC, L"0", 1, &zeroWidth);
        commitTotals = item.rcItem.right - item.rcItem.left > 60 * zeroWidth.cx;
      }
      if (sectionTotals || commitTotals)
      {
        auto drawTotal = [&](size_t count, wchar_t sign, ThemeColor role) {
          std::wstring value = sign + std::to_wstring(count);
          SIZE extent{};
          GetTextExtentPoint32W(item.hDC, value.c_str(), static_cast<int>(value.size()), &extent);
          RECT column{textRect.right - extent.cx, textRect.top, textRect.right, textRect.bottom};
          SetTextColor(item.hDC,
            selected ? (role == ThemeColor::RemovedIndicator ? RGB(255, 208, 192) : RGB(208, 255, 192)) : themeColor(role));
          DrawTextW(item.hDC, value.c_str(), static_cast<int>(value.size()), &column,
            DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
          textRect.right = column.left;
        };
        drawTotal(listItem->added, L'+', ThemeColor::AddedIndicator);
        SIZE spaceWidth{};
        GetTextExtentPoint32W(item.hDC, L" ", 1, &spaceWidth);
        textRect.right -= spaceWidth.cx;
        drawTotal(listItem->removed, L'-', ThemeColor::RemovedIndicator);
        textRect.right -= pad;
        SetTextColor(item.hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Title));
      }
      if (listItem && listItem->kind == FileListItemKind::File)
      {
        auto [added, removed] = std::pair{listItem->added, listItem->removed};
        FileStatsMode mode = fileStatsMode_;
        SIZE zeroWidth{};
        if (mode == FileStatsMode::Auto)
          GetTextExtentPoint32W(item.hDC, L"0", 1, &zeroWidth);
        if (mode == FileStatsMode::Auto)
          mode = item.rcItem.right - item.rcItem.left > 40 * zeroWidth.cx ? FileStatsMode::Numbers : FileStatsMode::Bars;
        if (mode == FileStatsMode::Bars)
        {
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
            SetTextColor(item.hDC,
              count ? (selected ? (role == ThemeColor::RemovedIndicator ? RGB(255, 208, 192) : RGB(208, 255, 192)) : themeColor(role))
                    : themeColor(selected ? ThemeColor::ListSelection : ThemeColor::Surface));
            DrawTextW(item.hDC, &glyph, 1, &indicator, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            OffsetRect(&indicator, cell, 0);
          };
          drawIndicator(removed, ThemeColor::RemovedIndicator);
          drawIndicator(added, ThemeColor::AddedIndicator);
          textRect.right -= cell * 2 + pad;
        }
        else if (mode == FileStatsMode::Numbers)
        {
          auto drawCount = [&](size_t count, size_t maximum, wchar_t sign, ThemeColor role) {
            std::wstring widest = sign + std::to_wstring(maximum);
            SIZE extent{};
            GetTextExtentPoint32W(item.hDC, widest.c_str(), static_cast<int>(widest.size()), &extent);
            RECT column{textRect.right - extent.cx, textRect.top, textRect.right, textRect.bottom};
            std::wstring value = sign + std::to_wstring(count);
            SetTextColor(item.hDC,
              selected ? (role == ThemeColor::RemovedIndicator ? RGB(255, 208, 192) : RGB(208, 255, 192)) : themeColor(role));
            DrawTextW(item.hDC, value.c_str(), static_cast<int>(value.size()), &column,
              DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            textRect.right = column.left;
          };
          drawCount(added, listItem->maxAdded, L'+', ThemeColor::AddedIndicator);
          SIZE spaceWidth{};
          GetTextExtentPoint32W(item.hDC, L" ", 1, &spaceWidth);
          textRect.right -= spaceWidth.cx;
          drawCount(removed, listItem->maxRemoved, L'-', ThemeColor::RemovedIndicator);
          textRect.right -= pad;
        }
        bool hasComment =
          std::any_of(comments_.begin(), comments_.end(), [&](const ReviewComment &comment) { return comment.key == listItem->key; });
        if (hasComment)
        {
          int markerWidth = MulDiv(23, static_cast<int>(dpi_), 96);
          RECT marker{textRect.right - markerWidth, textRect.top, textRect.right, textRect.bottom};
          SetTextColor(item.hDC, selected ? RGB(255, 230, 150) : themeColor(ThemeColor::CommentIndicator));
          DrawTextW(item.hDC, L"\xD83D\xDCAC", 2, &marker, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
          textRect.right -= markerWidth;
        }
        textRect.left += pad;
        SetTextColor(item.hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Text));
      }
      bool columns = commit ? item.itemID > 0 : commitHeader || (listItem && listItem->kind == FileListItemKind::File);
      if (columns)
      {
        RECT prefix = textRect;
        bool commitColumns = commit || commitHeader;
        auto code = value.substr(0, commitColumns ? 8 : 1);
        if (commitHeader)
        {
          SIZE hashWidth{}, spacesWidth{};
          GetTextExtentPoint32W(item.hDC, L"00000000", 8, &hashWidth);
          GetTextExtentPoint32W(item.hDC, L"   ", 3, &spacesWidth);
          prefix.right = textRect.left + hashWidth.cx + spacesWidth.cx;
        }
        else
          prefix.right = textRect.left + MulDiv(commit ? 80 : 28, static_cast<int>(dpi_), 96);
        DrawTextW(item.hDC, code.c_str(), static_cast<int>(code.size()), &prefix, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        textRect.left = prefix.right;
        value.erase(0, commitHeader ? 11 : commit ? 10 : 4);
      }
      bool file = listItem && listItem->kind == FileListItemKind::File;
      if (file)
      {
        auto separator = value.find_last_of(L"/\\");
        if (separator != std::wstring::npos)
          value.insert(separator + 1, 1, L' ');
      }
      SIZE textSize{};
      GetTextExtentPoint32W(item.hDC, value.c_str(), static_cast<int>(value.size()), &textSize);
      UINT align = file && textSize.cx > textRect.right - textRect.left ? DT_RIGHT : DT_LEFT;
      int textClip = SaveDC(item.hDC);
      IntersectClipRect(item.hDC, textRect.left, textRect.top, textRect.right, textRect.bottom);
      DrawTextW(item.hDC, value.c_str(), static_cast<int>(value.size()), &textRect,
        align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (combo || sectionHeader ? DT_END_ELLIPSIS : 0));
      auto separator = file ? value.find_last_of(L"/\\") : std::wstring::npos;
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

void MainWindow::drawStatus(const DRAWITEMSTRUCT &item) const
{
  RECT area = item.rcItem;
  auto background = CreateSolidBrush(themeColor(ThemeColor::Window));
  FillRect(item.hDC, &area, background);
  DeleteObject(background);
  if (statusAnimationActive_)
  {
    int areaWidth = static_cast<int>(area.right - area.left);
    int width = std::max(MulDiv(90, static_cast<int>(dpi_), 96), areaWidth / 6);
    int travel = std::max(1, areaWidth + width);
    int left = area.left - width + (statusAnimationPhase_ * MulDiv(14, static_cast<int>(dpi_), 96)) % travel;
    RECT highlight{left, area.top, left + width, area.bottom};
    auto base = themeColor(ThemeColor::Window), accent = themeColor(ThemeColor::ListSelection);
    auto blend = [](BYTE first, BYTE second) { return static_cast<BYTE>((first + second) / 2); };
    auto color = RGB(blend(GetRValue(base), GetRValue(accent)), blend(GetGValue(base), GetGValue(accent)),
      blend(GetBValue(base), GetBValue(accent)));
    auto brush = CreateSolidBrush(color);
    FillRect(item.hDC, &highlight, brush);
    DeleteObject(brush);
  }
  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, themeColor(ThemeColor::Text));
  SelectObject(item.hDC, font_);
  area.left += MulDiv(2, static_cast<int>(dpi_), 96);
  auto text = getText(status_);
  DrawTextW(item.hDC, text.c_str(), static_cast<int>(text.size()), &area,
    DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void MainWindow::startStatusAnimation()
{
  statusAnimationActive_ = true;
  statusAnimationPhase_ = 0;
  SetTimer(hwnd_, statusAnimationTimer, 50, nullptr);
  InvalidateRect(status_, nullptr, FALSE);
}

void MainWindow::stopStatusAnimation()
{
  statusAnimationActive_ = false;
  KillTimer(hwnd_, statusAnimationTimer);
  InvalidateRect(status_, nullptr, FALSE);
}

void MainWindow::saveSettings()
{
  if (!automationDirectory_.empty())
    return;
  settings_.setNumber(L"Source", static_cast<DWORD>(source(source_)));
  settings_.setNumber(L"SideBySide", side_);
  settings_.setNumber(L"FontSize", diff_.fontSize());
  settings_.setNumber(L"DarkTheme", darkTheme);
  settings_.setNumber(L"FilePaneWidth", static_cast<DWORD>(filePaneWidth_));
  settings_.setNumber(L"ExplorerLayout", explorerLayout_);
  settings_.setNumber(L"ExplorerCommitWidth", static_cast<DWORD>(explorerCommitWidth_));
  settings_.setNumber(L"ExplorerMessageWidth", static_cast<DWORD>(explorerMessageWidth_));
  settings_.setNumber(L"ExplorerTopHeight", static_cast<DWORD>(explorerTopHeight_));
  settings_.setNumber(L"HistoryCommitCount", static_cast<DWORD>(historyInitialLimit_));
  const wchar_t *statsModes[] = {L"none", L"bars", L"numbers", L"auto"};
  settings_.setString(L"FileStatsMode", statsModes[static_cast<size_t>(fileStatsMode_)]);
  if (source(source_) == ChangeSource::ReadyToPush)
    readyBase_ = getText(base_);
  else if (source(source_) == ChangeSource::Range)
    rangeBase_ = getText(base_);
  settings_.setString(L"ReadyBase", readyBase_);
  settings_.setString(L"RangeBase", rangeBase_);
  settings_.setString(L"Base", rangeBase_);
  settings_.setString(L"Target", getText(target_));
  std::wstring commit;
  if (source(source_) == ChangeSource::ReadyToPush && selectedListKey_.rfind(L"commit\n", 0) == 0)
  {
    size_t end = selectedListKey_.find(L'\n', 7);
    commit = selectedListKey_.substr(7, end - 7);
  }
  settings_.setString(L"Commit", commit);
  WINDOWPLACEMENT placement{sizeof(placement)};
  if (GetWindowPlacement(hwnd_, &placement))
  {
    if (placement.showCmd == SW_SHOWMINIMIZED)
      placement.showCmd = placement.flags & WPF_RESTORETOMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    settings_.setWindowPlacement(placement);
  }
  settings_.save();
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
  for (HWND combo : {source_, commits_})
  {
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(combo, &info))
      SetWindowTheme(info.hwndList, darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
  }
  SetWindowTextW(themeButton_, L"\u25D0");
  SendMessageW(tooltip_, TTM_SETTIPBKCOLOR, themeColor(ThemeColor::Surface), 0);
  SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR, themeColor(ThemeColor::Text), 0);
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}
std::wstring MainWindow::filePathAt(int index) const
{
  if (index < 0 || static_cast<size_t>(index) >= fileListItems_.size() || !fileListItems_[static_cast<size_t>(index)].file)
    return {};
  return fileListItems_[static_cast<size_t>(index)].file->path();
}
RECT MainWindow::explorerThumb(int index) const
{
  HWND bar = explorerBars_[index];
  HWND target = index == 0 ? explorerCommits_ : index == 1 ? explorerFiles_ : explorerMessage_;
  RECT track{};
  GetClientRect(bar, &track);
  if (!target)
    return track;
  RECT area{};
  GetClientRect(target, &area);
  int count = 0, top = 0, page = 1;
  if (index == 2)
  {
    count = static_cast<int>(SendMessageW(target, EM_GETLINECOUNT, 0, 0));
    top = static_cast<int>(SendMessageW(target, EM_GETFIRSTVISIBLELINE, 0, 0));
    page = editPageRows(target, font_);
  }
  else
  {
    count = static_cast<int>(SendMessageW(target, LB_GETCOUNT, 0, 0));
    top = static_cast<int>(SendMessageW(target, LB_GETTOPINDEX, 0, 0));
    page = std::max(1, static_cast<int>(area.bottom) / std::max(1, static_cast<int>(SendMessageW(target, LB_GETITEMHEIGHT, 0, 0))));
  }
  int height = track.bottom - track.top;
  if (count <= page || height <= 0)
    return track;
  int thumbHeight = std::clamp(MulDiv(height, page, count), static_cast<int>(track.right - track.left), height);
  int travel = height - thumbHeight;
  int maximum = std::max(1, count - page);
  int y = track.top + MulDiv(std::clamp(top, 0, maximum), travel, maximum);
  return {track.left, y, track.right, y + thumbHeight};
}
void MainWindow::updateExplorerBar(int index)
{
  if (explorerBars_[index])
    InvalidateRect(explorerBars_[index], nullptr, FALSE);
}
void MainWindow::scrollExplorerBar(int index, int top)
{
  HWND target = index == 0 ? explorerCommits_ : index == 1 ? explorerFiles_ : explorerMessage_;
  if (index == 2)
  {
    int current = static_cast<int>(SendMessageW(target, EM_GETFIRSTVISIBLELINE, 0, 0));
    SendMessageW(target, EM_LINESCROLL, 0, top - current);
  }
  else
    SendMessageW(target, LB_SETTOPINDEX, top, 0);
  updateExplorerBar(index);
}
LRESULT CALLBACK MainWindow::explorerBarProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
  auto self = reinterpret_cast<MainWindow *>(data);
  int index = static_cast<int>(id);
  if (msg == WM_PAINT || msg == WM_PRINTCLIENT)
  {
    PAINTSTRUCT ps{};
    HDC dc = msg == WM_PAINT ? BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(w);
    RECT track{};
    GetClientRect(hwnd, &track);
    FillRect(dc, &track, self->fieldBrush_);
    RECT thumb = self->explorerThumb(index);
    auto brush = CreateSolidBrush(themeColor(ThemeColor::Border));
    FillRect(dc, &thumb, brush);
    DeleteObject(brush);
    if (msg == WM_PAINT)
      EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_LBUTTONDOWN || msg == WM_MOUSEMOVE)
  {
    if (msg == WM_LBUTTONDOWN)
    {
      RECT thumb = self->explorerThumb(index);
      int y = GET_Y_LPARAM(l);
      self->explorerBarDrag_[index] = y >= thumb.top && y < thumb.bottom ? y - thumb.top : (thumb.bottom - thumb.top) / 2;
      SetCapture(hwnd);
    }
    if (self->explorerBarDrag_[index] >= 0)
    {
      RECT track{}, thumb = self->explorerThumb(index);
      GetClientRect(hwnd, &track);
      int travel = (track.bottom - track.top) - (thumb.bottom - thumb.top);
      if (travel > 0)
      {
        HWND target = index == 0 ? self->explorerCommits_ : index == 1 ? self->explorerFiles_ : self->explorerMessage_;
        RECT area{};
        GetClientRect(target, &area);
        int count = index == 2 ? static_cast<int>(SendMessageW(target, EM_GETLINECOUNT, 0, 0))
                               : static_cast<int>(SendMessageW(target, LB_GETCOUNT, 0, 0));
        int page =
          index == 2
            ? editPageRows(target, self->font_)
            : std::max(1, static_cast<int>(area.bottom) / std::max(1, static_cast<int>(SendMessageW(target, LB_GETITEMHEIGHT, 0, 0))));
        int maximum = std::max(0, count - page);
        int y = std::clamp(GET_Y_LPARAM(l) - self->explorerBarDrag_[index], 0, travel);
        self->scrollExplorerBar(index, MulDiv(y, maximum, travel));
      }
    }
    return 0;
  }
  if (msg == WM_LBUTTONUP || msg == WM_CAPTURECHANGED)
  {
    self->explorerBarDrag_[index] = -1;
    if (msg == WM_LBUTTONUP && GetCapture() == hwnd)
      ReleaseCapture();
    return 0;
  }
  if (msg == WM_MOUSEWHEEL)
  {
    HWND target = index == 0 ? self->explorerCommits_ : index == 1 ? self->explorerFiles_ : self->explorerMessage_;
    SendMessageW(target, msg, w, l);
    self->updateExplorerBar(index);
    return 0;
  }
  if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(hwnd, explorerBarProcedure, id);
  return DefSubclassProc(hwnd, msg, w, l);
}
LRESULT CALLBACK MainWindow::explorerMessageProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
  auto self = reinterpret_cast<MainWindow *>(data);
  if ((msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && (w == 'A' || w == 'a')) || (msg == WM_CHAR && w == 1))
  {
    SendMessageW(hwnd, EM_SETSEL, 0, -1);
    return 0;
  }
  auto result = DefSubclassProc(hwnd, msg, w, l);
  if (msg == WM_MOUSEWHEEL || msg == WM_VSCROLL || msg == WM_KEYDOWN || msg == WM_SIZE)
    self->updateExplorerBar(2);
  if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(hwnd, explorerMessageProcedure, id);
  return result;
}
LRESULT CALLBACK MainWindow::explorerListProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
  auto self = reinterpret_cast<MainWindow *>(data);
  bool commits = hwnd == self->explorerCommits_;
  int bar = commits ? 0 : 1;
  if (msg == WM_MOUSEWHEEL)
  {
    int &remainder = self->explorerWheel_[bar];
    scrollListWheel(hwnd, w, remainder);
    self->updateExplorerBar(bar);
    self->tooltipIndex_ = -1;
    SendMessageW(self->tooltip_, TTM_POP, 0, 0);
    return 0;
  }
  if (!commits && msg == WM_MOUSEMOVE)
  {
    auto hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, l);
    int index = HIWORD(hit) ? -1 : LOWORD(hit);
    if (index != self->tooltipIndex_ || self->tooltipOwner_ != hwnd)
    {
      self->tooltipIndex_ = index;
      self->tooltipOwner_ = hwnd;
      self->tooltipText_.clear();
      if (index >= 0 && static_cast<size_t>(index) < self->explorerFileItems_.size())
      {
        const auto &item = self->explorerFileItems_[static_cast<size_t>(index)];
        self->tooltipText_ = (std::filesystem::path(self->snapshot_.root) / item.file->path()).wstring() + L"    -" +
                             std::to_wstring(item.removed) + L" +" + std::to_wstring(item.added);
      }
      SendMessageW(self->tooltip_, TTM_POP, 0, 0);
      TOOLINFOW tool{sizeof(tool)};
      tool.hwnd = self->hwnd_;
      tool.uId = reinterpret_cast<UINT_PTR>(hwnd);
      tool.lpszText = self->tooltipText_.data();
      SendMessageW(self->tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
  }
  if (!commits && msg == WM_CONTEXTMENU)
  {
    POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
    int index = -1;
    if (point.x == -1 && point.y == -1)
    {
      index = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
      RECT row{};
      if (index >= 0 && SendMessageW(hwnd, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row)) != LB_ERR)
      {
        point = {row.left + 12, row.bottom};
        ClientToScreen(hwnd, &point);
      }
    }
    else
    {
      POINT client = point;
      ScreenToClient(hwnd, &client);
      auto hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(client.x, client.y));
      index = HIWORD(hit) ? -1 : LOWORD(hit);
    }
    if (index < 0 || static_cast<size_t>(index) >= self->explorerFileItems_.size())
      return 0;
    SendMessageW(hwnd, LB_SETCURSEL, index, 0);
    SetFocus(hwnd);
    self->selectExplorerFile(index);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Copy File Name");
    int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, self->hwnd_, nullptr);
    DestroyMenu(menu);
    if (command == 1)
      copyFileName(hwnd, self->explorerFileItems_[static_cast<size_t>(index)].file->path());
    return 0;
  }
  auto result = DefSubclassProc(hwnd, msg, w, l);
  if (msg == WM_VSCROLL || msg == LB_SETTOPINDEX || msg == LB_SETCURSEL || msg == WM_KEYDOWN || msg == WM_SIZE)
    self->updateExplorerBar(bar);
  if (msg == WM_NCDESTROY)
    RemoveWindowSubclass(hwnd, explorerListProcedure, id);
  return result;
}
LRESULT CALLBACK MainWindow::filesProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
  auto self = reinterpret_cast<MainWindow *>(data);
  if (msg == WM_MOUSEWHEEL)
  {
    scrollListWheel(hwnd, w, self->fileListWheel_);
    self->tooltipIndex_ = -1;
    SendMessageW(self->tooltip_, TTM_POP, 0, 0);
    return 0;
  }
  if (msg == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN))
  {
    self->navigateList(w == VK_UP ? -1 : 1, false);
    return 0;
  }
  if (msg == WM_KEYDOWN && (w == VK_RETURN || w == VK_SPACE))
  {
    int index = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
    if (index >= 0 && static_cast<size_t>(index) < self->fileListItems_.size() &&
        self->fileListItems_[static_cast<size_t>(index)].kind == FileListItemKind::LoadMore)
    {
      self->loadMoreHistory();
      return 0;
    }
  }
  if (msg == WM_LBUTTONUP)
  {
    auto result = DefSubclassProc(hwnd, msg, w, l);
    auto hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, l);
    int index = HIWORD(hit) ? -1 : LOWORD(hit);
    if (index >= 0 && static_cast<size_t>(index) < self->fileListItems_.size() &&
        self->fileListItems_[static_cast<size_t>(index)].kind == FileListItemKind::LoadMore)
      self->loadMoreHistory();
    return result;
  }
  if (msg == WM_MOUSEMOVE)
  {
    auto hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, l);
    int index = HIWORD(hit) ? -1 : LOWORD(hit);
    if (index != self->tooltipIndex_)
    {
      self->tooltipIndex_ = index;
      auto path = self->filePathAt(index);
      self->tooltipText_ = path.empty() ? L"" : (std::filesystem::path(self->snapshot_.root) / path).wstring();
      if (!path.empty() && index >= 0 && static_cast<size_t>(index) < self->fileListItems_.size())
      {
        const auto &item = self->fileListItems_[static_cast<size_t>(index)];
        self->tooltipText_ += L"    -" + std::to_wstring(item.removed) + L" +" + std::to_wstring(item.added);
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
      bool field = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || reinterpret_cast<HWND>(l) == explorerMessage_;
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
      if (hdr->code == NM_CUSTOMDRAW &&
          (hdr->hwndFrom == refresh_ || hdr->hwndFrom == compare_ || hdr->hwndFrom == themeButton_ || hdr->hwndFrom == view_ ||
            hdr->hwndFrom == fullFileButton_ || hdr->hwndFrom == layoutButton_ || hdr->hwndFrom == copyCommentsButton_) &&
          darkTheme)
      {
        auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
          bool checked = (hdr->hwndFrom == view_ || hdr->hwndFrom == fullFileButton_ || hdr->hwndFrom == layoutButton_) &&
                         SendMessageW(hdr->hwndFrom, BM_GETCHECK, 0, 0) == BST_CHECKED;
          auto buttonBrush = checked ? CreateSolidBrush(themeColor(ThemeColor::ListSelection)) : fieldBrush_;
          FillRect(draw->hdc, &draw->rc, buttonBrush);
          if (checked)
            DeleteObject(buttonBrush);
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
    case WM_DRAWITEM:
      if (reinterpret_cast<DRAWITEMSTRUCT *>(l)->hwndItem == status_)
        drawStatus(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
      else
        drawListItem(*reinterpret_cast<DRAWITEMSTRUCT *>(l));
      return TRUE;
    case WM_TIMER:
      if (w == automationTimer)
        automationTick();
      else if (w == statusAnimationTimer && statusAnimationActive_)
      {
        ++statusAnimationPhase_;
        InvalidateRect(status_, nullptr, FALSE);
      }
      else if (w == explorerResizeTimer)
      {
        KillTimer(hwnd_, explorerResizeTimer);
        redrawExplorerPanels();
      }
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
      if (explorerLayout_)
      {
        for (int i = 0; i < 3; ++i)
          if (explorerDragIndex_ == i || PtInRect(&explorerSplitters_[i], point))
          {
            SetCursor(LoadCursorW(nullptr, i == 2 ? IDC_SIZENS : IDC_SIZEWE));
            return TRUE;
          }
      }
      else if (draggingSplitter_ || PtInRect(&splitter_, point))
      {
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return TRUE;
      }
      break;
    }
    case WM_LBUTTONDOWN:
    {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (explorerLayout_)
        for (int i = 0; i < 3; ++i)
          if (PtInRect(&explorerSplitters_[i], point))
          {
            explorerDragIndex_ = i;
            splitterDragOffset_ = i == 2 ? point.y : point.x;
            SetCapture(hwnd_);
            SetCursor(LoadCursorW(nullptr, i == 2 ? IDC_SIZENS : IDC_SIZEWE));
            return 0;
          }
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
      if (explorerDragIndex_ >= 0)
      {
        int position = explorerDragIndex_ == 2 ? GET_Y_LPARAM(l) : GET_X_LPARAM(l);
        if (position != splitterDragOffset_)
        {
          splitterDragOffset_ = position;
          moveExplorerSplitter(explorerDragIndex_, position);
          KillTimer(hwnd_, explorerResizeTimer);
          SetTimer(hwnd_, explorerResizeTimer, 100, nullptr);
        }
        return 0;
      }
      if (draggingSplitter_)
      {
        moveSplitter(GET_X_LPARAM(l) - splitterDragOffset_);
        return 0;
      }
      break;
    case WM_LBUTTONUP:
      if (explorerDragIndex_ >= 0)
      {
        int position = explorerDragIndex_ == 2 ? GET_Y_LPARAM(l) : GET_X_LPARAM(l);
        if (position != splitterDragOffset_)
          moveExplorerSplitter(explorerDragIndex_, position);
        KillTimer(hwnd_, explorerResizeTimer);
        explorerDragIndex_ = -1;
        ReleaseCapture();
        redrawExplorerPanels();
        return 0;
      }
      if (draggingSplitter_)
      {
        moveSplitter(GET_X_LPARAM(l) - splitterDragOffset_);
        ReleaseCapture();
        return 0;
      }
      break;
    case WM_CAPTURECHANGED:
    {
      bool explorerWasDragging = explorerDragIndex_ >= 0;
      KillTimer(hwnd_, explorerResizeTimer);
      draggingSplitter_ = false;
      explorerDragIndex_ = -1;
      if (explorerWasDragging)
        redrawExplorerPanels();
      break;
    }
    case WM_CANCELMODE:
      if (draggingSplitter_ || explorerDragIndex_ >= 0)
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
    case WM_APP + 2: openCommentEditor(); return 0;
    case WM_COMMAND:
      switch (LOWORD(w))
      {
        case Theme:
          darkTheme = !darkTheme;
          applyTheme();
          break;
        case FullFile:
          if (HIWORD(w) == BN_CLICKED)
            toggleFullFile();
          break;
        case ExplorerLayout:
          if (HIWORD(w) == BN_CLICKED)
            setExplorerLayout(SendMessageW(layoutButton_, BM_GETCHECK, 0, 0) == BST_CHECKED);
          break;
        case ExplorerCommits:
          if (HIWORD(w) == LBN_SELCHANGE)
            selectExplorerGroup(static_cast<int>(SendMessageW(explorerCommits_, LB_GETCURSEL, 0, 0)));
          break;
        case ExplorerFiles:
          if (HIWORD(w) == LBN_SELCHANGE)
            selectExplorerFile(static_cast<int>(SendMessageW(explorerFiles_, LB_GETCURSEL, 0, 0)));
          break;
        case Refresh: refresh(); break;
        case CopyComments: copyComments(); break;
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
          if (HIWORD(w) == BN_CLICKED)
            toggle();
          break;
        case Toggle: toggle(); break;
        case Screenshot: screenshot(); break;
        case ZoomIn: diff_.zoom(1); break;
        case NextFile: navigateList(1); break;
        case PreviousFile: navigateList(-1); break;
        case NextChange:
          SetFocus(diff_.handle());
          diff_.navigateChange(1);
          break;
        case PreviousChange:
          SetFocus(diff_.handle());
          diff_.navigateChange(-1);
          break;
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
