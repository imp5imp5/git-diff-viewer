#pragma once
#include <string>
#include <vector>
namespace gdv
{
struct ReviewComment
{
  std::wstring key, path, branch, hash, changeId;
  int firstLine{}, lastLine{};
  std::wstring excerpt, text;
};
std::wstring formatReviewComments(const std::vector<ReviewComment> &comments);
std::wstring changeIdFromMessage(const std::wstring &message);
} // namespace gdv
