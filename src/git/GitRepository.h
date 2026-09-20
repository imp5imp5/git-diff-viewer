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
  Range,
  History
};
struct CompareRequest
{
  std::wstring directory;
  ChangeSource source{ChangeSource::Unstaged};
  std::wstring base, target;
  bool fullFile{};
  size_t historyLimit{10};
  size_t historySkip{};
  std::wstring historyHead;
  bool historyAppend{};
  std::vector<std::wstring> historyExcludedCommits;
  std::wstring path;
  std::wstring selectionKey;
  bool selectedOnly{};
};
struct Commit
{
  std::wstring id, subject, author, message;
};
struct HistorySnapshot
{
  DiffDocument unstaged, staged, outgoing;
  std::vector<Commit> outgoingCommits, commits;
  std::vector<DiffDocument> outgoingDocuments, commitDocuments;
  std::wstring initialHead, outgoingNotice;
  size_t nextSkip{};
  bool hasMore{};
};
struct RepositorySnapshot
{
  std::wstring root, branch, upstream, base, notice;
  std::wstring commitId, commitMessage;
  DiffDocument document;
  std::vector<Commit> commits;
  std::vector<DiffDocument> commitDocuments;
  HistorySnapshot history;
};
class GitRepository
{
public:
  RepositorySnapshot load(const CompareRequest &request, const std::atomic_bool &cancel) const;
  std::vector<Commit> findCommitsByPrefix(const std::wstring &directory, const std::wstring &prefix,
    const std::atomic_bool &cancel) const;
};
} // namespace gdv
