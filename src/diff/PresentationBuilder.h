#pragma once
#include "DiffModel.h"
#include <limits>
namespace gdv
{
constexpr size_t noLine = std::numeric_limits<size_t>::max();
struct PresentationRow
{
  size_t hunk{noLine}, left{noLine}, right{noLine};
  std::wstring meta;
  size_t comment{noLine};
  std::wstring commentText;
};
struct ChangeBlock
{
  size_t first{}, last{};
  bool added{}, removed{};
};
// Indices keep presentation independent of vector reallocations.
std::vector<PresentationRow> buildPresentation(const FileDiff &file, bool sideBySide);
std::vector<ChangeBlock> findChangeBlocks(const FileDiff &file, const std::vector<PresentationRow> &rows);
size_t correspondingRow(const std::vector<PresentationRow> &from, size_t row, const std::vector<PresentationRow> &to);
} // namespace gdv
