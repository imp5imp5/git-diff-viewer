#include "MainWindow.h"
#include "Screenshot.h"
#include "Theme.h"
#include <uxtheme.h>
#include <dwmapi.h>
#include <filesystem>
#include <algorithm>
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
  FullFile
};
constexpr int minFilePaneWidth = 220;
constexpr int minDiffPaneWidth = 300;
constexpr UINT_PTR automationTimer = 1;
constexpr UINT_PTR statusAnimationTimer = 2;
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
int MainWindow::run(HINSTANCE instance, int show, std::wstring directory, std::wstring automationDirectory)
{
  instance_ = instance;
  directory_ = std::move(directory);
  automationDirectory_ = std::move(automationDirectory);
  if (automationDirectory_.empty())
    settings_ = Settings::loadUser();
  historyInitialLimit_ = automationDirectory_.empty() ? std::clamp<size_t>(settings_.number(L"HistoryCommitCount", 10), 1, 1000) : 10;
  historyLimit_ = historyInitialLimit_;
  side_ = automationDirectory_.empty() && settings_.number(L"SideBySide", 0) != 0;
  darkTheme = !automationDirectory_.empty() || settings_.number(L"DarkTheme", 1) != 0;
  filePaneWidth_ = automationDirectory_.empty() ? static_cast<int>(std::min<DWORD>(settings_.number(L"FilePaneWidth", 0), 4096)) : 0;
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
  if (!automationDirectory_.empty())
    SetTimer(hwnd_, automationTimer, 100, nullptr);
  ACCEL entries[] = {{FVIRTKEY | FCONTROL, 'R', Refresh}, {FVIRTKEY, VK_F5, Refresh}, {FVIRTKEY | FCONTROL | FSHIFT, 'D', Toggle},
    {FVIRTKEY | FCONTROL | FSHIFT, 'S', Screenshot}, {FVIRTKEY | FCONTROL, VK_OEM_PLUS, ZoomIn},
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
      bool editing = focus == base_ || focus == target_ || focus == source_ || focus == view_ || focus == commits_;
      bool dropdown = SendMessageW(source_, CB_GETDROPPEDSTATE, 0, 0) || SendMessageW(view_, CB_GETDROPPEDSTATE, 0, 0) ||
                      SendMessageW(commits_, CB_GETDROPPEDSTATE, 0, 0);
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
  view_ = control(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, View);
  SendMessageW(view_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Unified"));
  SendMessageW(view_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Side-by-side"));
  SendMessageW(view_, CB_SETCURSEL, side_ ? 1 : 0, 0);
  fullFileButton_ = control(L"BUTTON", L"Full file", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, FullFile);
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
  if (!directory_.empty())
    refresh();
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
  move(fullFileButton_, pad + scale(441), pad, scale(100), row);
  move(themeButton_, pad + scale(553), pad, scale(115), row);
  int infoX = pad + scale(680);
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
  ShowWindow(commits_, SW_HIDE);
  ShowWindow(commitLabel_, SW_HIDE);
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
    fullFileDocuments_.clear();
    fullFileLoadingKey_.clear();
    snapshot_ = {};
    scrollContext_.clear();
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
  fileListItems_.clear();
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
  int selected = -1, fallback = -1;
  auto addItem = [&](FileListItem item) {
    int index = static_cast<int>(fileListItems_.size());
    SendMessageW(files_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.label.c_str()));
    if (!item.key.empty() && item.key == selectedListKey_)
      selected = index;
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
      FileListItem header{FileListItemKind::Commit, nullptr, commitIndex, 0, 0, commit.id.substr(0, 8) + L"  " + commit.subject,
        L"outgoing\n" + commit.id};
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
      FileListItem header{FileListItemKind::Commit, nullptr, commitIndex, 0, 0, commit.id.substr(0, 8) + L"  " + commit.subject,
        L"history\n" + commit.id};
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
      int header =
        addItem({FileListItemKind::Commit, nullptr, commitIndex, 0, 0, commit.id.substr(0, 8) + L"  " + commit.subject, key});
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
}
void MainWindow::navigateList(int direction, bool focusDiff)
{
  if (focusDiff)
    SetFocus(diff_.handle());
  if (loading_ || fileListItems_.empty() || !direction)
    return;
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
    auto saved = fileScrollPositions_.find(fileScrollKey(selectedListKey_));
    if (saved != fileScrollPositions_.end())
      diff_.scroll(saved->second);
    else if (fullFile_ && file != item.file)
      diff_.showFirstChange();
    if (fullFile_ && file == item.file)
      requestSelectedFullFile(item);
  }
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
  int index = static_cast<int>(SendMessageW(files_, LB_GETCURSEL, 0, 0));
  if (index >= 0 && static_cast<size_t>(index) < fileListItems_.size())
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
  for (const auto &file : document->files)
  {
    auto counts = changes(file);
    added += counts.first;
    removed += counts.second;
  }
  auto status =
    std::to_wstring(document->files.size()) + L" files changed    +" + std::to_wstring(added) + L"    −" + std::to_wstring(removed);
  if (!snapshot_.notice.empty())
    status += L"    " + snapshot_.notice;
  else
    status += L"    |    F5 Refresh · F Full file · Ctrl+PgUp/PgDn Change · Ctrl+Down/Up List · Space Commit message · Ctrl+Shift+D "
              L"View · Ctrl+C Copy";
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
  SendMessageW(view_, CB_SETCURSEL, side_ ? 1 : 0, 0);
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
    const FileListItem *listItem = !combo && item.itemID < fileListItems_.size() ? &fileListItems_[item.itemID] : nullptr;
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
      if (listItem && listItem->kind == FileListItemKind::File)
      {
        auto [added, removed] = std::pair{listItem->added, listItem->removed};
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
        textRect.left += pad;
        SetTextColor(item.hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Text));
      }
      bool columns = commit ? item.itemID > 0 : commitHeader || (listItem && listItem->kind == FileListItemKind::File);
      if (columns)
      {
        RECT prefix = textRect;
        bool commitColumns = commit || commitHeader;
        prefix.right = textRect.left + MulDiv(commitColumns ? 80 : 28, static_cast<int>(dpi_), 96);
        auto code = value.substr(0, commitColumns ? 8 : 1);
        DrawTextW(item.hDC, code.c_str(), static_cast<int>(code.size()), &prefix, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        textRect.left = prefix.right;
        value.erase(0, commitColumns ? 10 : 4);
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
        align | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | (combo || commitHeader ? DT_END_ELLIPSIS : 0));
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
  settings_.setNumber(L"HistoryCommitCount", static_cast<DWORD>(historyInitialLimit_));
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
  if (index < 0 || static_cast<size_t>(index) >= fileListItems_.size() || !fileListItems_[static_cast<size_t>(index)].file)
    return {};
  return fileListItems_[static_cast<size_t>(index)].file->path();
}
LRESULT CALLBACK MainWindow::filesProcedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data)
{
  auto self = reinterpret_cast<MainWindow *>(data);
  if (msg == WM_MOUSEWHEEL)
  {
    int delta = GET_WHEEL_DELTA_WPARAM(w);
    MSG queued{};
    while (PeekMessageW(&queued, hwnd, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE))
      delta += GET_WHEEL_DELTA_WPARAM(queued.wParam);
    self->fileListWheel_ += delta;
    int detents = self->fileListWheel_ / WHEEL_DELTA;
    self->fileListWheel_ %= WHEEL_DELTA;
    if (detents)
    {
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
      if (hdr->code == NM_CUSTOMDRAW &&
          (hdr->hwndFrom == refresh_ || hdr->hwndFrom == compare_ || hdr->hwndFrom == themeButton_ ||
            hdr->hwndFrom == fullFileButton_) &&
          darkTheme)
      {
        auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
          bool checked = hdr->hwndFrom == fullFileButton_ && SendMessageW(fullFileButton_, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
        case FullFile:
          if (HIWORD(w) == BN_CLICKED)
            toggleFullFile();
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
