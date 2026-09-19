#include "ReviewComments.h"
#include <sstream>
namespace gdv
{
std::wstring changeIdFromMessage(const std::wstring &message)
{
  std::wistringstream lines(message);
  std::wstring line;
  while (std::getline(lines, line))
  {
    auto start = line.find_first_not_of(L" \t\r");
    if (start == std::wstring::npos || line.compare(start, 10, L"Change-Id:") != 0)
      continue;
    start = line.find_first_not_of(L" \t", start + 10);
    if (start == std::wstring::npos)
      continue;
    auto end = line.find_last_not_of(L" \t\r");
    return line.substr(start, end - start + 1);
  }
  return {};
}
std::wstring formatReviewComments(const std::vector<ReviewComment> &comments)
{
  std::wstring result;
  for (const auto &comment : comments)
  {
    if (!comment.branch.empty())
      result += L"Branch: " + comment.branch + L"\r\n";
    if (!comment.hash.empty())
      result += L"Hash: " + comment.hash + L"\r\n";
    if (!comment.changeId.empty())
      result += L"Change-Id: " + comment.changeId + L"\r\n";
    result += L"File: " + comment.path + L"\r\n";
    result += L"Lines: " + std::to_wstring(comment.firstLine);
    if (comment.firstLine != comment.lastLine)
      result += L".." + std::to_wstring(comment.lastLine);
    result += L"\r\n";
    if (!comment.excerpt.empty())
      result += comment.excerpt + L" ...\r\n";
    result += L"Comment: " + comment.text + L"\r\n\r\n====\r\n\r\n";
  }
  return result;
}
} // namespace gdv
