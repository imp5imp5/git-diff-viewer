#pragma once
#include <optional>
#include <string>
#include <vector>
namespace gdv
{
enum class DiffLineType
{
  Context,
  Added,
  Removed,
  Meta
};
struct DiffLine
{
  DiffLineType type{DiffLineType::Meta};
  std::optional<int> oldLine, newLine;
  std::wstring text;
};
struct DiffHunk
{
  int oldStart{}, oldCount{}, newStart{}, newCount{};
  std::wstring header;
  std::vector<DiffLine> lines;
};
enum class FileStatus
{
  Added,
  Modified,
  Deleted,
  Renamed,
  Copied,
  Binary,
  Unknown
};
struct FileDiff
{
  std::wstring oldPath, newPath;
  FileStatus status{FileStatus::Modified};
  bool binary{};
  std::vector<std::wstring> metadata;
  std::vector<DiffHunk> hunks;
  std::wstring path() const { return newPath.empty() ? oldPath : newPath; }
};
struct DiffDocument
{
  std::vector<FileDiff> files;
  std::vector<std::wstring> warnings;
};
inline wchar_t statusLetter(FileStatus s)
{
  switch (s)
  {
    case FileStatus::Added: return L'A';
    case FileStatus::Deleted: return L'D';
    case FileStatus::Renamed: return L'R';
    case FileStatus::Copied: return L'C';
    case FileStatus::Binary: return L'B';
    case FileStatus::Unknown: return L'?';
    default: return L'M';
  }
}
} // namespace gdv
