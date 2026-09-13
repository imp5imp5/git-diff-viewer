#include "Settings.h"
#include "diff/UnifiedDiffParser.h"
#include <array>
#include <fstream>
#include <limits>
#include <shlobj.h>
#include <sstream>
namespace gdv
{
namespace
{
constexpr auto section = L"GitDiffViewer";
constexpr auto legacyKey = L"Software\\gaijin\\git_diff_viewer";
std::filesystem::path userSettingsPath()
{
  PWSTR directory = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &directory)))
    return {};
  std::filesystem::path path = std::filesystem::path(directory) / L"Gaijin" / L"GitDiffViewer" / L"settings.ini";
  CoTaskMemFree(directory);
  return path;
}
std::wstring escape(std::wstring_view value)
{
  std::wstring result;
  for (wchar_t c : value)
  {
    if (c == L'\\')
      result += L"\\\\";
    else if (c == L'\n')
      result += L"\\n";
    else if (c == L'\r')
      result += L"\\r";
    else
      result += c;
  }
  return result;
}
std::wstring unescape(std::wstring_view value)
{
  std::wstring result;
  for (size_t i = 0; i < value.size(); ++i)
  {
    if (value[i] != L'\\' || i + 1 >= value.size())
      result += value[i];
    else if (value[i + 1] == L'n')
    {
      result += L'\n';
      ++i;
    }
    else if (value[i + 1] == L'r')
    {
      result += L'\r';
      ++i;
    }
    else if (value[i + 1] == L'\\')
    {
      result += L'\\';
      ++i;
    }
    else
      result += value[i];
  }
  return result;
}
} // namespace
Settings Settings::loadUser()
{
  Settings result(userSettingsPath());
  if (result.path_.empty())
    return result;
  std::error_code error;
  bool exists = std::filesystem::is_regular_file(result.path_, error);
  result.load();
  if (!exists)
    result.importLegacyRegistry();
  return result;
}
Settings Settings::loadFile(std::filesystem::path path)
{
  Settings result(std::move(path));
  result.load();
  return result;
}
void Settings::load()
{
  values_.clear();
  std::ifstream input(path_, std::ios::binary);
  if (!input)
    return;
  std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  auto content = fromUtf8(bytes);
  if (!content.empty() && content.front() == 0xfeff)
    content.erase(content.begin());
  std::wistringstream lines(content);
  std::wstring line;
  bool active = false;
  while (std::getline(lines, line))
  {
    if (!line.empty() && line.back() == L'\r')
      line.pop_back();
    if (line == L"[" + std::wstring(section) + L"]")
    {
      active = true;
      continue;
    }
    if (!line.empty() && line.front() == L'[')
    {
      active = false;
      continue;
    }
    auto equals = line.find(L'=');
    if (active && equals != std::wstring::npos && equals > 0)
      values_[line.substr(0, equals)] = unescape(std::wstring_view(line).substr(equals + 1));
  }
}
DWORD Settings::number(const wchar_t *name, DWORD fallback) const
{
  auto found = values_.find(name);
  if (found == values_.end())
    return fallback;
  try
  {
    size_t end = 0;
    auto value = std::stoull(found->second, &end);
    if (end != found->second.size() || value > std::numeric_limits<DWORD>::max())
      return fallback;
    return static_cast<DWORD>(value);
  }
  catch (...)
  {
    return fallback;
  }
}
std::wstring Settings::string(const wchar_t *name) const
{
  auto found = values_.find(name);
  return found == values_.end() ? L"" : found->second;
}
bool Settings::windowPlacement(WINDOWPLACEMENT &placement) const
{
  auto found = values_.find(L"WindowPlacement");
  if (found == values_.end())
    return false;
  std::wistringstream input(found->second);
  std::array<long long, 5> fields{};
  wchar_t comma = 0;
  for (size_t i = 0; i < fields.size(); ++i)
  {
    if (!(input >> fields[i]) || (i + 1 < fields.size() && (!(input >> comma) || comma != L',')))
      return false;
  }
  if (input >> comma)
    return false;
  for (size_t i = 1; i < fields.size(); ++i)
    if (fields[i] < std::numeric_limits<LONG>::min() || fields[i] > std::numeric_limits<LONG>::max())
      return false;
  placement = {sizeof(placement)};
  placement.showCmd = fields[0] == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
  placement.rcNormalPosition = {
    static_cast<LONG>(fields[1]), static_cast<LONG>(fields[2]), static_cast<LONG>(fields[3]), static_cast<LONG>(fields[4])};
  return placement.rcNormalPosition.right > placement.rcNormalPosition.left &&
         placement.rcNormalPosition.bottom > placement.rcNormalPosition.top;
}
void Settings::setNumber(const wchar_t *name, DWORD value) { values_[name] = std::to_wstring(value); }
void Settings::setString(const wchar_t *name, std::wstring value) { values_[name] = std::move(value); }
void Settings::setWindowPlacement(const WINDOWPLACEMENT &placement)
{
  values_[L"WindowPlacement"] = std::to_wstring(placement.showCmd) + L"," + std::to_wstring(placement.rcNormalPosition.left) + L"," +
                                std::to_wstring(placement.rcNormalPosition.top) + L"," +
                                std::to_wstring(placement.rcNormalPosition.right) + L"," +
                                std::to_wstring(placement.rcNormalPosition.bottom);
}
bool Settings::save()
{
  if (path_.empty())
    return false;
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error)
    return false;
  std::wstring content = L"# GitDiffViewer settings\r\n[" + std::wstring(section) + L"]\r\n";
  for (const auto &[name, value] : values_)
    content += name + L"=" + escape(value) + L"\r\n";
  auto temporary = path_;
  temporary += L".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  auto bytes = toUtf8(content);
  output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output || !MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    std::filesystem::remove(temporary, error);
    return false;
  }
  if (legacyRegistry_)
  {
    if (RegDeleteKeyW(HKEY_CURRENT_USER, legacyKey) == ERROR_SUCCESS)
      legacyRegistry_ = false;
  }
  return true;
}
void Settings::importLegacyRegistry()
{
  HKEY key{};
  if (RegOpenKeyExW(HKEY_CURRENT_USER, legacyKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
    return;
  legacyRegistry_ = true;
  auto number = [&](const wchar_t *name) {
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(&value), &size) == ERROR_SUCCESS && type == REG_DWORD &&
        size == sizeof(value))
      setNumber(name, value);
  };
  auto string = [&](const wchar_t *name) {
    DWORD size = 0, type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || !size)
      return;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(value.data()), &size) == ERROR_SUCCESS)
    {
      while (!value.empty() && value.back() == L'\0')
        value.pop_back();
      setString(name, std::move(value));
    }
  };
  for (auto name : {L"Source", L"SideBySide", L"FontSize", L"DarkTheme", L"FilePaneWidth"})
    number(name);
  for (auto name : {L"Base", L"Target", L"Commit"})
    string(name);
  WINDOWPLACEMENT placement{sizeof(placement)};
  DWORD size = sizeof(placement), type = 0;
  if (RegQueryValueExW(key, L"WindowPlacement", nullptr, &type, reinterpret_cast<BYTE *>(&placement), &size) == ERROR_SUCCESS &&
      type == REG_BINARY && size == sizeof(placement))
  {
    if (placement.showCmd == SW_SHOWMINIMIZED)
      placement.showCmd = placement.flags & WPF_RESTORETOMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    setWindowPlacement(placement);
  }
  RegCloseKey(key);
  save();
}
} // namespace gdv
