#include "ui/Settings.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace gdv;
namespace
{
void check(bool value, const char *message)
{
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main()
{
  namespace fs = std::filesystem;
  auto directory = fs::temp_directory_path() /
                   (L"gfd-settings-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  auto file = directory / L"settings.ini";
  try
  {
    auto settings = Settings::loadFile(file);
    check(settings.number(L"Source", 7) == 7, "missing number fallback");
    check(settings.string(L"Base").empty(), "missing string fallback");
    settings.setNumber(L"Source", 3);
    settings.setNumber(L"HistoryCommitCount", 25);
    settings.setString(L"Base", L"feature/проба\\line\nnext");
    WINDOWPLACEMENT placement{sizeof(placement)};
    placement.showCmd = SW_SHOWMAXIMIZED;
    placement.rcNormalPosition = {-1200, 45, 80, 765};
    settings.setWindowPlacement(placement);
    check(settings.save(), "save settings");
    check(!fs::exists(file.wstring() + L".tmp"), "temporary file removed");

    std::ifstream input(file, std::ios::binary);
    char prefix[3]{};
    input.read(prefix, sizeof(prefix));
    check(!(static_cast<unsigned char>(prefix[0]) == 0xef && static_cast<unsigned char>(prefix[1]) == 0xbb &&
            static_cast<unsigned char>(prefix[2]) == 0xbf),
      "UTF-8 file has no BOM");
    input.close();

    auto loaded = Settings::loadFile(file);
    check(loaded.number(L"Source", 0) == 3, "number roundtrip");
    check(loaded.number(L"HistoryCommitCount", 0) == 25, "history commit count roundtrip");
    check(loaded.string(L"Base") == L"feature/проба\\line\nnext", "Unicode string roundtrip");
    WINDOWPLACEMENT restored{};
    check(loaded.windowPlacement(restored), "window placement roundtrip");
    check(restored.showCmd == SW_SHOWMAXIMIZED && restored.rcNormalPosition.left == -1200 && restored.rcNormalPosition.top == 45 &&
            restored.rcNormalPosition.right == 80 && restored.rcNormalPosition.bottom == 765,
      "window placement values");
    loaded.setString(L"Source", L"invalid");
    check(loaded.number(L"Source", 9) == 9, "invalid number fallback");
    fs::remove(file);
    fs::remove(directory);
    std::cout << "Settings tests passed\n";
    return 0;
  }
  catch (const std::exception &error)
  {
    fs::remove(file);
    fs::remove(file.wstring() + L".tmp");
    fs::remove(directory);
    std::cerr << error.what() << '\n';
    return 1;
  }
}
