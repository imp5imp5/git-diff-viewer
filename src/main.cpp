#include "ui/MainWindow.h"
#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <objbase.h>
#include <shellapi.h>
#include <stdexcept>
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  int count = 0;
  auto args = CommandLineToArgvW(GetCommandLineW(), &count);
  const bool startHistory = count == 1;
  std::wstring directory, automation, hashPrefix;
  bool hashRequested = false;
  for (int i = 1; i < count; ++i)
  {
    if (std::wstring(args[i]) == L"--automation-dir" && i + 1 < count)
      automation = args[++i];
    else if (std::wstring(args[i]).rfind(L"--hash:", 0) == 0)
    {
      hashPrefix = std::wstring(args[i]).substr(7);
      hashRequested = true;
    }
    else if (directory.empty())
      directory = args[i];
  }
  LocalFree(args);
  int result = 0;
  try
  {
    if (hashRequested && (hashPrefix.size() < 4 || hashPrefix.size() > 64 ||
                          !std::all_of(hashPrefix.begin(), hashPrefix.end(),
                            [](wchar_t c) { return c < 128 && iswxdigit(c) != 0; })))
      throw std::invalid_argument("--hash requires 4 to 64 hexadecimal characters.");
    if (directory.empty())
      directory = std::filesystem::current_path().wstring();
    if (!automation.empty())
    {
      automation = std::filesystem::absolute(automation).wstring();
      std::filesystem::create_directories(automation);
    }
    gdv::MainWindow window;
    result = window.run(instance, show, std::move(directory), automation, hashPrefix, startHistory);
  }
  catch (const std::exception &error)
  {
    if (!automation.empty())
      std::ofstream(std::filesystem::path(automation) / L"error.txt") << error.what();
    else
      MessageBoxA(nullptr, error.what(), "GitDiffViewer", MB_ICONERROR);
    result = 1;
  }
  catch (...)
  {
    if (!automation.empty())
      std::ofstream(std::filesystem::path(automation) / L"error.txt") << "Unknown initialization error.";
    else
      MessageBoxW(nullptr, L"The application could not initialize.", L"GitDiffViewer", MB_ICONERROR);
    result = 1;
  }
  if (SUCCEEDED(com))
    CoUninitialize();
  return result;
}
