#pragma once
#include <string>
#include <windows.h>
namespace gdv
{
// Captures only this application's client area, including its child controls.
void saveScreenshot(HWND window, const std::wstring &path);
} // namespace gdv
