#pragma once
#include <optional>
#include <string>
#include <vector>
#include <windows.h>
namespace gdv
{
std::optional<std::wstring> chooseRef(HWND owner, HINSTANCE instance, HFONT font, const std::wstring &title,
  const std::wstring &resetLabel, const std::wstring &current, const std::vector<std::wstring> &refs);
} // namespace gdv