#include "ui/MainWindow.h"
#include "diff/UnifiedDiffParser.h"
#include "git/GitClient.h"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <objbase.h>
#include <shellapi.h>
#include <stdexcept>
namespace
{
struct FilterPath
{
  std::wstring directory, path;
};
std::optional<FilterPath> resolveFilterPath(const std::wstring &path, const std::filesystem::path &current)
{
  namespace fs = std::filesystem;
  if (fs::path(path).is_absolute())
    return std::nullopt;
  auto candidate = (current / path).lexically_normal();
  auto probe = candidate;
  std::error_code error;
  while (!fs::is_directory(probe, error))
  {
    auto parent = probe.parent_path();
    if (parent == probe)
      return std::nullopt;
    probe = parent;
    error.clear();
  }
  try
  {
    std::atomic_bool cancel{false};
    auto result = gdv::GitClient{}.run(probe.wstring(), {L"rev-parse", L"--show-toplevel"}, cancel);
    if (result.exitCode)
      return std::nullopt;
    while (!result.out.empty() && (result.out.back() == '\n' || result.out.back() == '\r'))
      result.out.pop_back();
    auto root = fs::path(gdv::fromUtf8(result.out)).lexically_normal();
    auto relative = candidate.lexically_relative(root);
    if (relative.empty() || relative == L"." || *relative.begin() == L"..")
      return std::nullopt;
    return FilterPath{root.wstring(), relative.generic_wstring()};
  }
  catch (...)
  {
    return std::nullopt;
  }
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&controls);
  int count = 0;
  auto args = CommandLineToArgvW(GetCommandLineW(), &count);
  bool startHistory = count == 1 || (count == 3 && std::wstring(args[1]) == L"--");
  std::wstring directory, automation, hashPrefix, pathFilter;
  bool hashRequested = false;
  bool pathRequested = false;
  bool extraPathArgument = false;
  for (int i = 1; i < count; ++i)
  {
    if (std::wstring(args[i]) == L"--")
    {
      pathRequested = true;
      if (i + 1 < count)
        pathFilter = args[++i];
      if (i + 1 < count)
      {
        extraPathArgument = true;
        break;
      }
    }
    else if (std::wstring(args[i]) == L"--automation-dir" && i + 1 < count)
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
    if (extraPathArgument)
      throw std::invalid_argument("Only one path may follow --.");
    if (pathRequested && pathFilter.empty())
      throw std::invalid_argument("A non-empty path must follow --.");
    if (hashRequested && (hashPrefix.size() < 4 || hashPrefix.size() > 64 ||
                           !std::all_of(hashPrefix.begin(), hashPrefix.end(), [](wchar_t c) { return c < 128 && iswxdigit(c) != 0; })))
      throw std::invalid_argument("--hash requires 4 to 64 hexadecimal characters.");
    if (!pathRequested && !directory.empty())
    {
      if (auto filter = resolveFilterPath(directory, std::filesystem::current_path()))
      {
        directory = std::move(filter->directory);
        pathFilter = std::move(filter->path);
        startHistory = !hashRequested;
      }
    }
    if (directory.empty())
      directory = std::filesystem::current_path().wstring();
    if (!automation.empty())
    {
      automation = std::filesystem::absolute(automation).wstring();
      std::filesystem::create_directories(automation);
    }
    gdv::MainWindow window;
    result = window.run(instance, show, std::move(directory), automation, hashPrefix, startHistory, std::move(pathFilter));
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
