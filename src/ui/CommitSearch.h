#pragma once
#include "git/GitRepository.h"
#include <optional>
#include <windows.h>
namespace gdv
{
struct CommitSearchState
{
  CommitSearchRequest request;
  std::wstring branch{L"All branches"}, message, author, path, resultBranch, status;
  std::vector<CommitSearchMatch> matches;
  int selected{-1}, topIndex{};
  bool hasMore{};
};
std::optional<Commit> searchRepositoryCommits(HWND owner, HINSTANCE instance, const std::wstring &directory, int fontPoints,
  CommitSearchState &state);
} // namespace gdv
