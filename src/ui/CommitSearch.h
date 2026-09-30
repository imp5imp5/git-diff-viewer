#pragma once
#include "git/GitRepository.h"
#include <optional>
#include <windows.h>
namespace gdv
{
std::optional<Commit> searchRepositoryCommits(HWND owner, HINSTANCE instance, const std::wstring &directory,
  const std::wstring &branch, int fontPoints);
} // namespace gdv
