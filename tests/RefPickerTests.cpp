#include "ui/RefPicker.h"
#include "ui/Theme.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>
using namespace gdv;
void check(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
int main()
try
{
  HWND owner = CreateWindowExW(0, L"STATIC", L"Ref picker test", WS_OVERLAPPEDWINDOW, 100, 100, 800, 600, nullptr, nullptr,
    GetModuleHandleW(nullptr), nullptr);
  check(owner != nullptr, "create owner");
  auto font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
    CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  check(font != nullptr, "create font");
  auto run = [&](bool dark, const std::wstring &filter, const std::wstring &expected) {
    darkTheme = dark;
    std::string driverError;
    std::thread driver([&] {
      HWND dialog{};
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!dialog && std::chrono::steady_clock::now() < deadline)
      {
        dialog = FindWindowW(L"GitDiffViewer.RefPicker", nullptr);
        if (!dialog)
          Sleep(10);
      }
      if (!dialog)
      {
        driverError = "ref dialog did not appear";
        return;
      }
      HWND edit{}, list{};
      while ((!edit || !list) && std::chrono::steady_clock::now() < deadline)
      {
        edit = GetDlgItem(dialog, 101);
        list = GetDlgItem(dialog, 102);
        if (!edit || !list)
          Sleep(10);
      }
      if (!edit || !list)
      {
        driverError = "ref dialog controls missing";
        PostMessageW(dialog, WM_CLOSE, 0, 0);
        return;
      }
      SetWindowTextW(edit, filter.c_str());
      if (SendMessageW(list, LB_GETCOUNT, 0, 0) != 1)
        driverError = "filter did not narrow the list to one ref";
      PostMessageW(dialog, WM_COMMAND, 103, 0);
    });
    auto result = chooseRef(owner, GetModuleHandleW(nullptr), font, L"Choose branch", L"Current HEAD (default)", L"",
      {L"main", L"feature", L"origin/topic"});
    driver.join();
    check(driverError.empty(), driverError.c_str());
    check(result && *result == expected, "ref picker returned wrong selection");
  };
  run(true, L"topic", L"origin/topic");
  run(false, L"Current HEAD", L"");
  DeleteObject(font);
  DestroyWindow(owner);
  return 0;
}
catch (const std::exception &error)
{
  std::cerr << error.what() << std::endl;
  return 1;
}