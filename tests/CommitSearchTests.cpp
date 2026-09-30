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
  std::thread thread;
  Dialog(const fs::path &directory, int points)
  {
    thread = std::thread([&, directory, points] {
      HINSTANCE instance = GetModuleHandleW(nullptr);
      HWND owner =
        CreateWindowExW(0, L"STATIC", L"Search owner", WS_OVERLAPPEDWINDOW, 100, 100, 1000, 700, nullptr, nullptr, instance, nullptr);
      result = searchRepositoryCommits(owner, instance, directory.wstring(), L"main", points);
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
                  << '\0' << '\n';
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
    check(Header_GetItemCount(ListView_GetHeader(list)) == 3, "search table has author, date and message columns");
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
            dialog.cell(0, 2) == L"Subject 0",
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
    check(dialog.result && dialog.result->subject == L"Subject 1" && dialog.result->branch == L"main",
      "button and double click return the selected commit");
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
