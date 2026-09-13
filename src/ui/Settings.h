#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <windows.h>
namespace gdv
{
class Settings
{
public:
  Settings() = default;
  static Settings loadUser();
  static Settings loadFile(std::filesystem::path path);
  DWORD number(const wchar_t *name, DWORD fallback) const;
  std::wstring string(const wchar_t *name) const;
  bool windowPlacement(WINDOWPLACEMENT &placement) const;
  void setNumber(const wchar_t *name, DWORD value);
  void setString(const wchar_t *name, std::wstring value);
  void setWindowPlacement(const WINDOWPLACEMENT &placement);
  bool save();
  const std::filesystem::path &path() const { return path_; }

private:
  explicit Settings(std::filesystem::path path) : path_(std::move(path)) {}
  void load();
  void importLegacyRegistry();
  std::filesystem::path path_;
  std::map<std::wstring, std::wstring> values_;
  bool legacyRegistry_{};
};
} // namespace gdv
