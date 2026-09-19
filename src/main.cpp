#include "ui/MainWindow.h"
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <objbase.h>
#include <shellapi.h>
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  int count = 0;
  auto args = CommandLineToArgvW(GetCommandLineW(), &count);
  std::wstring directory, automation;
  for (int i = 1; i < count; ++i)
  {
    if (std::wstring(args[i]) == L"--automation-dir" && i + 1 < count)
      automation = args[++i];
    else if (directory.empty())
      directory = args[i];
  }
  LocalFree(args);
  int result = 0;
  try
  {
    if (directory.empty())
      directory = std::filesystem::current_path().wstring();
    if (!automation.empty())
    {
      automation = std::filesystem::absolute(automation).wstring();
      std::filesystem::create_directories(automation);
    }
    gdv::MainWindow window;
    result = window.run(instance, show, std::move(directory), automation);
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
