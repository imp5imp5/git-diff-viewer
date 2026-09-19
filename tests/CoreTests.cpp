#include "diff/PresentationBuilder.h"
#include "diff/ReviewComments.h"
#include "diff/UnifiedDiffParser.h"
#include <iostream>
#include <stdexcept>
using namespace gdv;
void check(bool b, const char *message)
{
  if (!b)
    throw std::runtime_error(message);
}
int main()
try
{
  UnifiedDiffParser p;
  auto d = p.parse("diff --git a/a b/a\n--- a/a\n+++ b/a\n@@ -1,3 +1,4 @@ f\n same\n-old\n+new\n+extra\n "
                   "end\n\\ No newline at end of file\n");
  check(d.warnings.empty(), "valid patch");
  check(d.files.size() == 1, "one file");
  auto &f = d.files[0];
  check(f.hunks[0].lines[1].oldLine == 2, "old number");
  check(f.hunks[0].lines[2].newLine == 2, "new number");
  auto u = buildPresentation(f, false), s = buildPresentation(f, true);
  auto unifiedBlocks = findChangeBlocks(f, u), sideBlocks = findChangeBlocks(f, s);
  check(unifiedBlocks.size() == 1 && unifiedBlocks[0].last - unifiedBlocks[0].first == 2 && unifiedBlocks[0].added &&
          unifiedBlocks[0].removed,
    "unified change block");
  check(sideBlocks.size() == 1 && sideBlocks[0].last - sideBlocks[0].first == 1 && sideBlocks[0].added && sideBlocks[0].removed,
    "side-by-side change block");
  check(u.size() == 7 && s.size() == 6, "alignment");
  check(s[2].left == 1 && s[2].right == 2, "replacement");
  check(s[3].left == noLine && s[3].right == 3, "filler");
  check(correspondingRow(u, 3, s) == 2, "anchor");
  check(f.hunks[0].lines.size() == 6, "immutable");
  check(s[1].left == s[1].right && s[1].left == 0, "context on both sides");
  auto unequal = p.parse("diff --git a/a b/a\n@@ -1,3 +1 @@\n-one\n-two\n-three\n+replacement\n");
  auto unequalRows = buildPresentation(unequal.files[0], true);
  check(unequalRows.size() == 4 && unequalRows[1].right == 3 && unequalRows[3].right == noLine,
    "N removals and M additions produce max(N,M) rows");
  auto newline = p.parse("diff --git a/a b/a\n@@ -1 +1 @@\n-old\n\\ No newline at end of file\n+new\n\\ No "
                         "newline at end of file\n");
  auto newlineRows = buildPresentation(newline.files[0], true);
  check(newlineRows[1].left == 0 && newlineRows[1].right == 2, "no-newline markers do not break replacements");
  for (size_t i = 0; i < u.size(); ++i)
  {
    auto destination = correspondingRow(u, i, s);
    check(destination < s.size() && u[i].hunk == s[destination].hunk, "toggle preserves the hunk for every row");
  }
  auto m = p.parse("diff --git a/old name b/new name\nsimilarity index 100%\nrename from old name\nrename "
                   "to new name\ndiff --git a/bin b/bin\nBinary files a/bin and b/bin differ\ndiff --git "
                   "a/empty b/empty\nnew file mode 100644\ndiff --git a/d b/d\ndeleted file mode "
                   "100644\n--- a/d\n+++ /dev/null\n@@ -1 +0,0 @@\n-gone\n");
  check(m.files.size() == 4, "multiple files");
  check(m.files[0].status == FileStatus::Renamed && m.files[0].newPath == L"new name", "rename");
  check(m.files[1].binary, "binary");
  check(m.files[2].status == FileStatus::Added && m.files[2].hunks.empty(), "empty");
  check(m.files[3].status == FileStatus::Deleted && m.files[3].newPath.empty(), "deleted");
  check(buildPresentation(m.files[3], true).back().right == noLine, "delete filler");
  auto utf = p.parse(u8"diff --git a/тест.txt b/тест.txt\n--- a/тест.txt\n+++ b/тест.txt\n@@ -0,0 +1 @@\n+Привет 🌍\n");
  check(utf.files[0].newPath == L"тест.txt", "unicode path");
  check(toUtf8(utf.files[0].hunks[0].lines[0].text) == u8"Привет 🌍", "unicode content");
  auto q = p.parse("diff --git \"a/a\\\"b\\t.txt\" \"b/a\\\"b\\t.txt\"\nnew file mode 100644\n");
  check(q.files[0].newPath == L"a\"b\t.txt", "quoted path");
  auto oct = p.parse("diff --git \"a/\\320\\260\" \"b/\\320\\260\"\nnew file mode 100644\n");
  check(oct.files[0].newPath == L"а", "octal");
  auto h = p.parse("diff --git a/a b/a\n@@ -1 +1 @@\n-a\n+b\n@@ -20 +30 @@\n-c\n+d\n");
  check(h.files[0].hunks[1].lines[1].newLine == 30, "hunks");
  auto separated = buildPresentation(h.files[0], false);
  check(separated.size() == 7, "divider");
  check(findChangeBlocks(h.files[0], separated).size() == 2, "separated change blocks");
  check(p.parse("diff --git a/a b/a\nold mode 100644\nnew mode 100755\n").files[0].metadata.size() == 2, "mode");
  check(p.parse("diff --git a/a b/b\ncopy from a\ncopy to b\n").files[0].status == FileStatus::Copied, "copy");
  check(!p.parse("diff --git a/a b/a\n@@ -1,2 +1,2 @@\n a\n").warnings.empty(), "truncated");
  check(!p.parse("diff --git a/a b/a\n@@ -999999999999999 +1 @@\n+x\n").warnings.empty(), "malformed");
  check(
    p.parse("diff --git a/a b/a\n@@ -0,0 +1 @@\n+" + std::string(100000, 'x') + "\n").files[0].hunks[0].lines[0].text.size() == 100000,
    "long");
  std::string large = "diff --git a/a b/a\n@@ -0,0 +1,20000 @@\n";
  for (int i = 0; i < 20000; ++i)
    large += "+row\n";
  auto big = p.parse(large);
  check(buildPresentation(big.files[0], true).size() == 20001, "20k");
  for (size_t i = 0; i < large.size(); i += 997)
    p.parse(std::string_view(large).substr(0, i));
  check(!fromUtf8("\xff").empty(), "invalid UTF8");
  auto conflicts = p.parse("diff --git a/normal b/normal\n@@ -1 +1 @@\n-old\n+new\n"
                           "diff --cc conflict.txt\nindex 123,456..000\n--- a/conflict.txt\n+++ b/conflict.txt\n"
                           "@@@ -1 -1 +1 @@@\n++<<<<<<< HEAD\n"
                           "diff --git a/after b/after\n@@ -1 +1 @@\n-before\n+after\n");
  check(conflicts.files.size() == 3 && conflicts.files[0].path() == L"normal" && conflicts.files[0].hunks[0].lines.size() == 2 &&
          conflicts.files[1].path() == L"conflict.txt" && conflicts.files[1].status == FileStatus::Unknown &&
          conflicts.files[1].hunks.empty() && !conflicts.files[1].metadata.empty() && conflicts.files[2].path() == L"after" &&
          !conflicts.warnings.empty(),
    "combined conflict is visible and cannot corrupt adjacent files");
  for (auto header : {"* Unmerged path ", "diff --combined "})
  {
    auto conflict = p.parse(std::string(header) + "conflict with spaces.txt\n");
    check(conflict.files.size() == 1 && conflict.files[0].path() == L"conflict with spaces.txt" &&
            conflict.files[0].status == FileStatus::Unknown,
      "staged and combined conflict paths");
  }
  ReviewComment review;
  review.path = L"src/example.cpp";
  review.branch = L"main";
  review.hash = L"0123456789abcdef";
  review.changeId = changeIdFromMessage(L"Subject\n\nChange-Id: I123456\n");
  review.firstLine = 49;
  review.lastLine = 51;
  review.excerpt = L"  E3DCOLOR color = value;";
  review.text = L"adapt colors";
  check(review.changeId == L"I123456", "Change-Id trailer");
  check(formatReviewComments({review}) ==
          L"Branch: main\r\nHash: 0123456789abcdef\r\nChange-Id: I123456\r\nFile: src/example.cpp\r\n"
          L"Lines: 49..51\r\n  E3DCOLOR color = value; ...\r\nComment: adapt colors\r\n\r\n====\r\n\r\n",
    "comment export format");
  review.branch.clear();
  review.hash.clear();
  review.changeId.clear();
  review.firstLine = review.lastLine = 7;
  check(formatReviewComments({review}).find(L"Lines: 7\r\n") != std::wstring::npos,
    "uncommitted comment omits commit metadata and single line has no range");
  std::cout << "Parser, presentation, Unicode, truncation and 20,000-line tests passed\n";
  return 0;
}
catch (const std::exception &e)
{
  std::cerr << e.what() << '\n';
  return 1;
}
