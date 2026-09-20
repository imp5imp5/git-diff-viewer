#pragma once
#include "git/GitRepository.h"
#include <optional>
#include <windows.h>
namespace gdv
{
std::optional<Commit> chooseCommit(HWND owner, HINSTANCE instance, const std::wstring &prefix,
  const std::vector<Commit> &matches);
} // namespace gdv
