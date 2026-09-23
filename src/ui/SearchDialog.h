#pragma once
#include <string>
#include <vector>
#include <windows.h>
namespace gdv
{
struct FindDialogResult
{
  bool accepted{};
  std::wstring text;
};
FindDialogResult findDiffText(HWND owner, HINSTANCE instance, const std::wstring &current, const std::vector<std::wstring> &history);
} // namespace gdv