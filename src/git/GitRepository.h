#pragma once
#include "GitClient.h"
#include "diff/DiffModel.h"
namespace gdv
{
enum class ChangeSource
{
  Staged,
  Unstaged,
  Head,
  ReadyToPush,
  Commit,
  Range
};
struct CompareRequest
{
  std::wstring directory;
  ChangeSource source{ChangeSource::Unstaged};
  std::wstring base, target;
};
struct Commit
{
  std::wstring id, subject, author, message;
};
struct RepositorySnapshot
{
  std::wstring root, branch, upstream, base, notice;
  std::wstring commitId, commitMessage;
  DiffDocument document;
  std::vector<Commit> commits;
};
class GitRepository
{
public:
  RepositorySnapshot load(const CompareRequest &request, const std::atomic_bool &cancel) const;
};
} // namespace gdv
