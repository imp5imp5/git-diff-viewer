#include "ui/CommitPicker.h"
#include "ui/Theme.h"
#include <atomic>
#include <stdexcept>
#include <thread>
#include <windows.h>
using namespace gdv;
namespace
{
void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
std::optional<std::wstring> run(bool dark, bool empty)
{
  darkTheme = dark;
  const std::vector<Commit> matches = empty
                                        ? std::vector<Commit>{}
                                        : std::vector<Commit>{{L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", L"First subject", {}, {}},
                                            {L"aaaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", L"Second subject", {}, {}}};
  std::optional<std::wstring> result;
  std::atomic<DWORD> workerId{};
  std::thread worker([&] {
    workerId = GetCurrentThreadId();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND owner =
      CreateWindowExW(0, L"STATIC", L"Picker owner", WS_OVERLAPPEDWINDOW, 100, 100, 700, 500, nullptr, nullptr, instance, nullptr);
    result = chooseCommit(owner, instance, L"aaaa", matches);
    DestroyWindow(owner);
  });
  HWND dialog = nullptr;
  for (int attempt = 0; attempt < 200; ++attempt)
  {
    dialog = FindWindowW(L"GitDiffViewer.CommitPicker", nullptr);
    if (dialog)
      break;
    Sleep(10);
  }
  if (!dialog)
  {
    if (workerId)
      PostThreadMessageW(workerId, WM_QUIT, 0, 0);
    worker.join();
    throw std::runtime_error("commit picker did not open");
  }
  const char *error = nullptr;
  HWND list = FindWindowExW(dialog, nullptr, L"LISTBOX", nullptr);
  if (!list || SendMessageW(list, LB_GETCOUNT, 0, 0) != static_cast<LRESULT>(matches.size()))
    error = "commit picker list count";
  if (!error && !empty)
  {
    wchar_t label[200]{};
    SendMessageW(list, LB_GETTEXT, 1, reinterpret_cast<LPARAM>(label));
    if (std::wstring(label).find(L"Second subject") == std::wstring::npos)
      error = "commit picker displays the first message line";
  }
  RECT listClient{};
  GetClientRect(list, &listClient);
  RedrawWindow(list, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  HDC listDc = GetDC(list);
  COLORREF listBackground = GetPixel(listDc, 8, listClient.bottom - 8);
  ReleaseDC(list, listDc);
  if (listBackground != (dark ? RGB(0, 0, 0) : themeColor(ThemeColor::Surface)))
    error = "commit picker list background";
  HDC dc = GetDC(dialog);
  SendMessageW(dialog, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), 0);
  if (GetBkColor(dc) != themeColor(ThemeColor::Window) || GetTextColor(dc) != themeColor(ThemeColor::Text))
    error = "commit picker follows the selected theme";
  ReleaseDC(dialog, dc);
  if (error)
  {
    SendMessageW(dialog, WM_CLOSE, 0, 0);
    worker.join();
    throw std::runtime_error(error);
  }
  if (empty)
    SendMessageW(dialog, WM_CLOSE, 0, 0);
  else
  {
    SendMessageW(list, LB_SETCURSEL, 1, 0);
    SendMessageW(dialog, WM_COMMAND, 102, 0);
  }
  worker.join();
  return result;
}
} // namespace
int main()
try
{
  auto darkChoice = run(true, false);
  check(darkChoice && *darkChoice == L"aaaabbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", "dark picker selection");
  auto lightChoice = run(false, false);
  check(lightChoice && *lightChoice == *darkChoice, "light picker selection");
  check(!run(true, true), "no-match picker closes without a selection");
  return 0;
}
catch (const std::exception &error)
{
  OutputDebugStringA(error.what());
  return 1;
}
