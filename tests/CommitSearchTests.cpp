#include "ui/CommitSearch.h"
#include "ui/Screenshot.h"
#include "ui/Theme.h"
#include "diff/UnifiedDiffParser.h"
#include <algorithm>
#include <chrono>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace gdv;
namespace fs = std::filesystem;
namespace
{
void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
template <typename Predicate>
void wait(Predicate predicate, const char *message)
{
  for (int attempt = 0; attempt < 500; ++attempt)
  {
    if (predicate())
      return;
    Sleep(10);
  }
  throw std::runtime_error(message);
}
struct Dialog
{
  HWND window{};
  std::optional<Commit> result;
  CommitSearchState localState;
  std::thread thread;
  Dialog(const fs::path &directory, int points, CommitSearchState *state = nullptr)
  {
    thread = std::thread([&, directory, points, state] {
      HINSTANCE instance = GetModuleHandleW(nullptr);
      HWND owner =
        CreateWindowExW(0, L"STATIC", L"Search owner", WS_OVERLAPPEDWINDOW, 100, 100, 1000, 700, nullptr, nullptr, instance, nullptr);
      result = searchRepositoryCommits(owner, instance, directory.wstring(), points, state ? *state : localState);
      DestroyWindow(owner);
    });
    try
    {
      wait(
        [&] {
          window = FindWindowW(L"GitDiffViewer.CommitSearch", nullptr);
          return window && GetDlgItem(window, IDOK) && IsWindowEnabled(GetDlgItem(window, IDOK));
        },
        "search dialog initializes asynchronously");
    }
    catch (...)
    {
      if (window)
        SendMessageW(window, WM_CLOSE, 0, 0);
      thread.join();
      throw;
    }
  }
  ~Dialog()
  {
    if (IsWindow(window))
      SendMessageW(window, WM_CLOSE, 0, 0);
    if (thread.joinable())
      thread.join();
  }
  void click(int id) { SendMessageW(GetDlgItem(window, id), BM_CLICK, 0, 0); }
  void idle()
  {
    wait([&] { return IsWindowEnabled(GetDlgItem(window, IDOK)); }, "search finishes");
  }
  std::wstring cell(int row, int column)
  {
    wchar_t value[256]{};
    NMLVDISPINFOW info{};
    info.hdr = {GetDlgItem(window, 120), 120, LVN_GETDISPINFOW};
    info.item.mask = LVIF_TEXT;
    info.item.iItem = row;
    info.item.iSubItem = column;
    info.item.pszText = value;
    info.item.cchTextMax = 256;
    SendMessageW(window, WM_NOTIFY, 120, reinterpret_cast<LPARAM>(&info));
    return value;
  }
};
int fakeGit(int argc, wchar_t **argv)
{
  for (int i = 1; i < argc; ++i)
  {
    if (std::wstring(argv[i]) == L"for-each-ref")
    {
      for (int j = i + 1; j < argc; ++j)
        if (std::wstring(argv[j]).find(L"%(objectname)") != std::wstring::npos)
        {
          std::cout << "main\t" << std::string(40, 'a') << "\t\nfeature\t" << std::string(40, 'b') << "\t\n";
          return 0;
        }
      std::cout << "main\nfeature\n";
      return 0;
    }
    if (std::wstring(argv[i]) == L"rev-parse")
    {
      std::cout << std::string(40, 'a') << '\n';
      return 0;
    }
    if (std::wstring(argv[i]) == L"log")
    {
      if (fs::exists(L"slow"))
      {
        std::ofstream(L"started") << "started";
        Sleep(10000);
        return 0;
      }
      int skip = 0, count = 101;
      for (int j = i + 1; j < argc; ++j)
      {
        std::wstring arg = argv[j];
        if (arg.rfind(L"--skip=", 0) == 0)
          skip = std::stoi(arg.substr(7));
        if (arg.rfind(L"-n", 0) == 0)
          count = std::stoi(arg.substr(2));
      }
      for (int row = skip; row < std::min(101, skip + count); ++row)
      {
        std::cout << std::setfill('0') << std::setw(40) << row << '\0' << "Subject " << row << '\0'
                  << "Test Author <test@example.invalid>" << '\0' << "2026-09-30T12:34:56+03:00" << '\0' << "Subject " << row
                  << "\n\nFull commit body with details.\n"
                  << '\0' << std::string(40, row % 2 ? 'b' : 'a') << '\0' << '\n';
      }
      return 0;
    }
  }
  return 1;
}
} // namespace
int wmain(int argc, wchar_t **argv)
try
{
  if (fs::path(argv[0]).filename() == L"git.exe")
    return fakeGit(argc, argv);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  fs::path directory = fs::temp_directory_path() /
                       (L"GitDiffViewer-search-test-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::create_directories(directory / L"bin");
  struct Cleanup
  {
    fs::path directory;
    std::wstring path;
    ~Cleanup()
    {
      SetEnvironmentVariableW(L"PATH", path.c_str());
      std::error_code error;
      fs::remove_all(directory, error);
    }
  } cleanup{directory, {}};
  DWORD length = GetEnvironmentVariableW(L"PATH", nullptr, 0);
  cleanup.path.resize(length);
  cleanup.path.resize(GetEnvironmentVariableW(L"PATH", cleanup.path.data(), length));
  fs::copy_file(argv[0], directory / L"bin" / L"git.exe");
  auto path = (directory / L"bin").wstring() + L";" + cleanup.path;
  SetEnvironmentVariableW(L"PATH", path.c_str());
  for (bool dark : {true, false})
  {
    darkTheme = dark;
    Dialog dialog(directory, dark ? 11 : 18);
    HWND list = GetDlgItem(dialog.window, 120);
    wchar_t branch[100]{};
    GetWindowTextW(GetDlgItem(dialog.window, 110), branch, 100);
    check(std::wstring(branch) == L"All branches" && SendMessageW(GetDlgItem(dialog.window, 110), CB_GETCURSEL, 0, 0) == 0,
      "All branches is first and selected by default");
    check(Header_GetItemCount(ListView_GetHeader(list)) == 4, "all-branch search also has a branch column");
    check(!IsWindowVisible(GetDlgItem(dialog.window, 103)), "open commit button is hidden without selection");
    LOGFONTW font{};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)), sizeof(font), &font);
    check(font.lfHeight == -MulDiv(dark ? 11 : 18, static_cast<int>(GetDpiForWindow(dialog.window)), 72),
      "search table inherits the parent diff font size");
    check(ListView_GetBkColor(list) == themeColor(ThemeColor::Surface), "search table follows the parent theme");
    dialog.click(IDOK);
    dialog.idle();
    check(ListView_GetItemCount(list) == 100 && IsWindowVisible(GetDlgItem(dialog.window, 104)), "search results are paginated");
    check(dialog.cell(0, 0) == L"Test Author <test@example.invalid>" && dialog.cell(0, 1) == L"2026-09-30 12:34" &&
            dialog.cell(0, 2) == L"Subject 0" && dialog.cell(0, 3) == L"main" && dialog.cell(1, 3) == L"feature",
      "search table displays commit details");
    dialog.click(104);
    dialog.idle();
    check(ListView_GetItemCount(list) == 101 && !IsWindowVisible(GetDlgItem(dialog.window, 104)), "load more appends the next page");
    // Ask for the hover hint at the first result's position.
    RECT row{};
    ListView_GetItemRect(list, 0, &row, LVIR_BOUNDS);
    SendMessageW(list, WM_MOUSEMOVE, 0, MAKELPARAM(row.left + 20, (row.top + row.bottom) / 2));
    HWND tooltip = FindWindowExW(nullptr, nullptr, TOOLTIPS_CLASSW, nullptr);
    TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
    tool.uFlags = TTF_IDISHWND;
    tool.hwnd = dialog.window;
    tool.uId = reinterpret_cast<UINT_PTR>(list);
    while (tooltip && !SendMessageW(tooltip, TTM_GETTOOLINFOW, 0, reinterpret_cast<LPARAM>(&tool)))
      tooltip = FindWindowExW(nullptr, tooltip, TOOLTIPS_CLASSW, nullptr);
    check(tooltip != nullptr, "full-message tooltip is registered for the search table");
    NMTTDISPINFOW tip{};
    tip.hdr = {tooltip, 0, TTN_GETDISPINFOW};
    SendMessageW(dialog.window, WM_NOTIFY, 0, reinterpret_cast<LPARAM>(&tip));
    std::wstring hint = tip.lpszText ? tip.lpszText : L"";
    check(hint.find(L"Full commit body with details.") != std::wstring::npos, "hover hint contains the full commit message");
    ListView_SetItemState(list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    check(IsWindowVisible(GetDlgItem(dialog.window, 103)), "selecting a result shows the open commit button");
    if (argc > 1)
    {
      fs::create_directories(argv[1]);
      saveScreenshot(dialog.window, (fs::path(argv[1]) / (dark ? L"search-dark.png" : L"search-light.png")).wstring());
    }
    if (dark)
      dialog.click(103);
    else
    {
      NMITEMACTIVATE activation{};
      activation.hdr = {list, 120, NM_DBLCLK};
      activation.iItem = 1;
      SendMessageW(dialog.window, WM_NOTIFY, 120, reinterpret_cast<LPARAM>(&activation));
    }
    dialog.thread.join();
    check(dialog.result && dialog.result->subject == L"Subject 1" && dialog.result->branch == L"feature",
      "button and double click return the selected commit with its branch");
  }
  {
    Dialog dialog(directory, 11);
    HWND branch = GetDlgItem(dialog.window, 110), list = GetDlgItem(dialog.window, 120);
    SendMessageW(branch, CB_SETCURSEL, 3, 0); // All branches, HEAD, feature, main
    SendMessageW(dialog.window, WM_COMMAND, MAKEWPARAM(110, CBN_SELCHANGE), reinterpret_cast<LPARAM>(branch));
    check(Header_GetItemCount(ListView_GetHeader(list)) == 3, "choosing a single branch removes the branch column");
    dialog.click(IDOK);
    dialog.idle();
    ListView_SetItemState(list, 1, LVIS_SELECTED, LVIS_SELECTED);
    dialog.click(103);
    dialog.thread.join();
    check(dialog.result && dialog.result->branch == L"main", "single-branch results preserve the selected branch");
  }
  {
    Dialog dialog(directory, 11);
    HWND branch = GetDlgItem(dialog.window, 110), list = GetDlgItem(dialog.window, 120);
    SetWindowTextW(branch, L"HEAD");
    SendMessageW(dialog.window, WM_COMMAND, MAKEWPARAM(110, CBN_EDITCHANGE), reinterpret_cast<LPARAM>(branch));
    SendMessageW(branch, CB_SETCURSEL, 0, 0);
    SendMessageW(dialog.window, WM_COMMAND, MAKEWPARAM(110, CBN_SELCHANGE), reinterpret_cast<LPARAM>(branch));
    check(Header_GetItemCount(ListView_GetHeader(list)) == 4, "choosing All branches restores the branch column");
  }
  CommitSearchState saved;
  {
    Dialog dialog(directory, 11, &saved);
    SetWindowTextW(GetDlgItem(dialog.window, 111), L"searched message");
    SetWindowTextW(GetDlgItem(dialog.window, 112), L"searched author");
    SetWindowTextW(GetDlgItem(dialog.window, 113), L"*.cpp");
    dialog.click(IDOK);
    dialog.idle();
    HWND list = GetDlgItem(dialog.window, 120);
    ListView_SetItemState(list, 90, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list, 90, FALSE);
    SetWindowTextW(GetDlgItem(dialog.window, 111), L"edited message");
    dialog.click(IDCANCEL);
    dialog.thread.join();
    check(saved.matches.size() == 100 && saved.selected == 90 && saved.hasMore, "closing saves results, selection and pagination");
  }
  // Reopening must restore cached results without invoking Git log.
  std::ofstream(directory / L"slow") << "slow";
  {
    Dialog dialog(directory, 18, &saved);
    HWND list = GetDlgItem(dialog.window, 120);
    wchar_t value[100]{};
    GetWindowTextW(GetDlgItem(dialog.window, 111), value, 100);
    check(std::wstring(value) == L"edited message" && saved.request.message == L"searched message",
      "edited filters and the executed search are preserved separately");
    GetWindowTextW(GetDlgItem(dialog.window, 112), value, 100);
    check(std::wstring(value) == L"searched author", "author filter is restored");
    GetWindowTextW(GetDlgItem(dialog.window, 113), value, 100);
    check(std::wstring(value) == L"*.cpp", "path filter is restored");
    check(ListView_GetItemCount(list) == 100 && ListView_GetNextItem(list, -1, LVNI_SELECTED) == 90 &&
            IsWindowVisible(GetDlgItem(dialog.window, 103)) && IsWindowVisible(GetDlgItem(dialog.window, 104)),
      "reopening restores results, selected row and action buttons without rerunning the search");
    check(ListView_GetTopIndex(list) <= 90 && ListView_GetTopIndex(list) + ListView_GetCountPerPage(list) >= 90,
      "the restored selected commit is visible");
    fs::remove(directory / L"slow");
    dialog.click(104);
    dialog.idle();
    check(ListView_GetItemCount(list) == 101, "restored search can load the next page");
    ListView_SetItemState(list, 100, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    dialog.click(103);
    dialog.thread.join();
    check(dialog.result && dialog.result->subject == L"Subject 100", "opening a cached result returns the selected commit");
  }
  {
    Dialog dialog(directory, 11, &saved);
    HWND list = GetDlgItem(dialog.window, 120);
    check(ListView_GetItemCount(list) == 101 && ListView_GetNextItem(list, -1, LVNI_SELECTED) == 100,
      "returning after opening a commit restores its selection and all loaded pages");
    HWND branch = GetDlgItem(dialog.window, 110);
    SetWindowTextW(branch, L"main");
    SendMessageW(dialog.window, WM_COMMAND, MAKEWPARAM(110, CBN_EDITCHANGE), reinterpret_cast<LPARAM>(branch));
  }
  {
    Dialog dialog(directory, 11, &saved);
    check(Header_GetItemCount(ListView_GetHeader(GetDlgItem(dialog.window, 120))) == 3 &&
            SendMessageW(GetDlgItem(dialog.window, 110), CB_GETCURSEL, 0, 0) == 3,
      "reopening restores the branch filter and corresponding columns");
  }
  {
    auto other = directory / L"other";
    fs::create_directory(other);
    Dialog dialog(other, 11, &saved);
    check(ListView_GetItemCount(GetDlgItem(dialog.window, 120)) == 0 && saved.matches.empty(),
      "changing repositories resets cached search state");
  }
  std::ofstream(directory / L"slow") << "slow";
  for (bool close : {false, true})
  {
    fs::remove(directory / L"started");
    Dialog dialog(directory, 11);
    dialog.click(IDOK);
    wait([&] { return fs::exists(directory / L"started"); }, "slow Git search starts");
    check(!IsWindowEnabled(GetDlgItem(dialog.window, IDOK)), "search runs asynchronously with Find disabled");
    auto started = std::chrono::steady_clock::now();
    if (close)
      SendMessageW(dialog.window, WM_CLOSE, 0, 0);
    else
      dialog.click(IDCANCEL);
    dialog.thread.join();
    check(!dialog.result && std::chrono::steady_clock::now() - started < std::chrono::seconds(3),
      "Cancel and closing the dialog stop the active Git process promptly");
  }
  std::cout << "Commit search UI, pagination, theme, fonts, hints, selection and cancellation passed\n";
  return 0;
}
catch (const std::exception &error)
{
  std::cerr << error.what() << '\n';
  return 1;
}
