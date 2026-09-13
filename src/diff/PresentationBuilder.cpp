#include "PresentationBuilder.h"
#include <algorithm>
namespace gdv
{
std::vector<PresentationRow> buildPresentation(const FileDiff &file, bool side)
{
  std::vector<PresentationRow> rows;
  for (const auto &text : file.metadata)
    rows.push_back({noLine, noLine, noLine, text});
  if (file.hunks.empty() && file.metadata.empty())
    rows.push_back({noLine, noLine, noLine, L"No text changes (empty file or metadata-only change)."});
  for (size_t h = 0; h < file.hunks.size(); ++h)
  {
    const auto &hunk = file.hunks[h];
    if (h > 0)
      rows.push_back({h, noLine, noLine, L"··· unchanged lines omitted ···"});
    rows.push_back({h, noLine, noLine, hunk.header});
    const auto &lines = hunk.lines;
    for (size_t i = 0; i < lines.size();)
    {
      if (lines[i].type == DiffLineType::Meta)
      {
        rows.push_back({h, i, i, lines[i].text});
        ++i;
        continue;
      }
      if (side && lines[i].type == DiffLineType::Removed)
      {
        std::vector<size_t> removed, added, meta;
        while (i < lines.size() && (lines[i].type == DiffLineType::Removed || lines[i].type == DiffLineType::Meta))
        {
          (lines[i].type == DiffLineType::Meta ? meta : removed).push_back(i++);
        }
        while (i < lines.size() && (lines[i].type == DiffLineType::Added || lines[i].type == DiffLineType::Meta))
        {
          (lines[i].type == DiffLineType::Meta ? meta : added).push_back(i++);
        }
        for (size_t j = 0; j < std::max(removed.size(), added.size()); ++j)
          rows.push_back({h, j < removed.size() ? removed[j] : noLine, j < added.size() ? added[j] : noLine, {}});
        for (auto m : meta)
          rows.push_back({h, m, m, lines[m].text});
      }
      else
      {
        rows.push_back(
          {h, lines[i].type == DiffLineType::Added ? noLine : i, lines[i].type == DiffLineType::Removed ? noLine : i, {}});
        ++i;
      }
    }
  }
  return rows;
}
std::vector<ChangeBlock> findChangeBlocks(const FileDiff &file, const std::vector<PresentationRow> &rows)
{
  auto changed = [&](const PresentationRow &row) {
    if (row.hunk == noLine || row.hunk >= file.hunks.size())
      return false;
    const auto &lines = file.hunks[row.hunk].lines;
    for (auto index : {row.left, row.right})
      if (index != noLine && index < lines.size() &&
          (lines[index].type == DiffLineType::Added || lines[index].type == DiffLineType::Removed))
        return true;
    return false;
  };
  std::vector<ChangeBlock> blocks;
  for (size_t i = 0; i < rows.size(); ++i)
    if (changed(rows[i]))
    {
      if (blocks.empty() || blocks.back().last + 1 != i)
        blocks.push_back({i, i});
      else
        blocks.back().last = i;
    }
  return blocks;
}
size_t correspondingRow(const std::vector<PresentationRow> &from, size_t row, const std::vector<PresentationRow> &to)
{
  if (from.empty() || to.empty())
    return 0;
  const auto &source = from[std::min(row, from.size() - 1)];
  size_t fallback = 0;
  for (size_t i = 0; i < to.size(); ++i)
  {
    const auto &target = to[i];
    if (target.hunk != source.hunk)
      continue;
    fallback = i;
    if ((source.left != noLine && source.left == target.left) || (source.right != noLine && source.right == target.right) ||
        (source.left == noLine && source.right == noLine && target.meta == source.meta))
      return i;
  }
  return fallback;
}
} // namespace gdv
