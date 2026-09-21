#include "DiffView.h"
#include "Theme.h"
#include <algorithm>
#include <climits>
#include <sstream>
#include <windowsx.h>
namespace gdv
{
namespace
{
constexpr UINT_PTR autoScrollTimer = 2;
constexpr UINT_PTR changeFlashTimer = 3;
void fill(HDC dc, RECT r, ThemeColor color)
{
  HBRUSH b = CreateSolidBrush(themeColor(color));
  FillRect(dc, &r, b);
  DeleteObject(b);
}
void fillColor(HDC dc, RECT r, COLORREF color)
{
  HBRUSH b = CreateSolidBrush(color);
  FillRect(dc, &r, b);
  DeleteObject(b);
}
COLORREF flashColor(COLORREF color)
{
  int target = darkTheme ? 255 : 0;
  auto channel = [&](int value) { return (value * 4 + target) / 5; };
  return RGB(channel(GetRValue(color)), channel(GetGValue(color)), channel(GetBValue(color)));
}
void text(HDC dc, RECT r, const std::wstring &value, ThemeColor color, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX)
{
  SetTextColor(dc, themeColor(color));
  DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &r, flags);
}
ThemeColor background(const DiffLine *l)
{
  if (!l)
    return ThemeColor::EmptyCell;
  if (l->type == DiffLineType::Added)
    return ThemeColor::Added;
  if (l->type == DiffLineType::Removed)
    return ThemeColor::Removed;
  return ThemeColor::Surface;
}
} // namespace
DiffView::~DiffView()
{
  if (font_)
    DeleteObject(font_);
}
HWND DiffView::create(HWND parent, HINSTANCE instance)
{
  WNDCLASSW wc{};
  wc.style = CS_DBLCLKS;
  wc.hInstance = instance;
  wc.lpfnWndProc = procedure;
  wc.lpszClassName = L"GitDiffViewer.Diff";
  wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
  RegisterClassW(&wc);
  return CreateWindowExW(0, wc.lpszClassName, L"Diff", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | WS_HSCROLL, 0, 0, 100, 100,
    parent, nullptr, instance, this);
}
void DiffView::setDpi(UINT dpi)
{
  dpi_ = dpi;
  if (font_)
    DeleteObject(font_);
  font_ = CreateFontW(-MulDiv(fontPoints_, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
  HDC dc = GetDC(hwnd_);
  auto old = SelectObject(dc, font_);
  TEXTMETRICW tm{};
  GetTextMetricsW(dc, &tm);
  rowHeight_ = tm.tmHeight + MulDiv(5, static_cast<int>(dpi), 96);
  charWidth_ = tm.tmAveCharWidth;
  headerHeight_ = std::max(MulDiv(62, static_cast<int>(dpi), 96), rowHeight_ * 2);
  SelectObject(dc, old);
  ReleaseDC(hwnd_, dc);
  setFile(file_, true, plain_);
}
void DiffView::setFile(const FileDiff *file, bool preserve, bool plainText)
{
  stopAutoScroll();
  stopChangeFlash();
  plain_ = plainText;
  headerHeight_ = plain_ ? 0 : std::max(MulDiv(62, static_cast<int>(dpi_), 96), rowHeight_ * 2);
  const FileDiff *previousFile = file_;
  file_ = file;
  auto previousRows = rows_;
  rows_ = file ? buildPresentation(*file, side_) : std::vector<PresentationRow>{};
  decorateRows();
  if (preserve && previousFile == file && previousRows.size() != rows_.size() && !previousRows.empty() && !rows_.empty())
  {
    top_ = static_cast<int>(correspondingRow(previousRows, static_cast<size_t>(top_), rows_));
    if (selected_ >= 0)
      selected_ = static_cast<int>(correspondingRow(previousRows, static_cast<size_t>(selected_), rows_));
    if (anchor_ >= 0)
      anchor_ = static_cast<int>(correspondingRow(previousRows, static_cast<size_t>(anchor_), rows_));
  }
  if (!preserve)
  {
    top_ = horizontal_ = 0;
    selected_ = anchor_ = -1;
    activeChange_ = -1;
  }
  else if (activeChange_ >= static_cast<int>(navigationBlocks_.size()))
    activeChange_ = -1;
  maxWidth_ = 0;
  numberDigits_ = 7;
  if (file)
    for (const auto &text : file->metadata)
      maxWidth_ = std::max(maxWidth_, static_cast<int>(std::min<size_t>(text.size() * charWidth_, INT_MAX / 2)));
  if (file)
    for (const auto &h : file->hunks)
      for (const auto &l : h.lines)
      {
        for (auto n : {l.oldLine, l.newLine})
          if (n)
            numberDigits_ = std::max(numberDigits_, static_cast<int>(std::to_wstring(*n).size()));
        size_t width = 0;
        for (wchar_t c : l.text)
          width += c == L'\t' ? 4 - (width % 4) : 1;
        maxWidth_ = std::max(maxWidth_, static_cast<int>(std::min<size_t>(width * charWidth_, INT_MAX / 2)));
      }
  for (const auto &comment : comments_)
  {
    std::wistringstream lines(comment.text);
    std::wstring part;
    while (std::getline(lines, part))
      maxWidth_ = std::max(maxWidth_, static_cast<int>(std::min<size_t>((part.size() + 9) * charWidth_, INT_MAX / 2)));
  }
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
}
void DiffView::zoom(int steps)
{
  int size = std::clamp(fontPoints_ + steps, 6, 40);
  if (size == fontPoints_)
    return;
  fontPoints_ = size;
  setDpi(dpi_);
}
void DiffView::setMessage(std::wstring message)
{
  message_ = std::move(message);
  setFile(nullptr);
}
void DiffView::setSideBySide(bool enabled)
{
  if (side_ == enabled)
    return;
  side_ = enabled;
  auto previousRows = rows_;
  rows_ = file_ ? buildPresentation(*file_, side_) : std::vector<PresentationRow>{};
  decorateRows();
  top_ = static_cast<int>(correspondingRow(previousRows, static_cast<size_t>(top_), rows_));
  if (selected_ >= 0)
    selected_ = static_cast<int>(correspondingRow(previousRows, static_cast<size_t>(selected_), rows_));
  if (anchor_ >= 0)
    anchor_ = static_cast<int>(correspondingRow(previousRows, static_cast<size_t>(anchor_), rows_));
  activeChange_ = -1;
  stopChangeFlash();
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
}
void DiffView::decorateRows()
{
  changeBlocks_ = file_ ? findChangeBlocks(*file_, rows_) : std::vector<ChangeBlock>{};
  navigationBlocks_ = changeBlocks_;
  if (rows_.empty() || comments_.empty())
    return;
  std::vector<std::vector<size_t>> after(rows_.size());
  for (size_t comment = 0; comment < comments_.size(); ++comment)
  {
    size_t anchor = noLine;
    for (size_t row = 0; row < rows_.size(); ++row)
    {
      const DiffLine *value = line(rows_[row], rows_[row].right);
      if (!value)
        value = line(rows_[row], rows_[row].left);
      if (value && value->newLine && *value->newLine >= comments_[comment].firstLine && *value->newLine <= comments_[comment].lastLine)
        anchor = row;
    }
    if (anchor != noLine)
      after[anchor].push_back(comment);
  }
  std::vector<PresentationRow> expanded;
  std::vector<size_t> mapped(rows_.size());
  for (size_t row = 0; row < rows_.size(); ++row)
  {
    mapped[row] = expanded.size();
    expanded.push_back(std::move(rows_[row]));
    for (size_t comment : after[row])
    {
      size_t first = expanded.size();
      std::wistringstream lines(comments_[comment].text);
      std::wstring part;
      bool initial = true;
      while (std::getline(lines, part))
      {
        if (!part.empty() && part.back() == L'\r')
          part.pop_back();
        PresentationRow annotation;
        annotation.comment = comment;
        annotation.commentText = (initial ? L"Comment: " : L"") + part;
        expanded.push_back(std::move(annotation));
        initial = false;
      }
      if (initial)
      {
        PresentationRow annotation;
        annotation.comment = comment;
        annotation.commentText = L"Comment:";
        expanded.push_back(std::move(annotation));
      }
      navigationBlocks_.push_back({first, expanded.size() - 1, false, false});
    }
  }
  rows_ = std::move(expanded);
  for (auto &block : changeBlocks_)
  {
    block.first = mapped[block.first];
    block.last = mapped[block.last];
  }
  for (auto &block : navigationBlocks_)
    if (block.added || block.removed)
    {
      block.first = mapped[block.first];
      block.last = mapped[block.last];
    }
  std::stable_sort(navigationBlocks_.begin(), navigationBlocks_.end(),
    [](const ChangeBlock &a, const ChangeBlock &b) { return a.first < b.first; });
}
void DiffView::setComments(std::vector<ReviewComment> comments)
{
  comments_ = std::move(comments);
  activeChange_ = -1;
  setFile(file_, true, plain_);
}
std::optional<std::pair<int, int>> DiffView::selectedNewLines() const
{
  if (!file_ || (side_ && !selectedAfter_) || anchor_ < 0 || selected_ < 0)
    return std::nullopt;
  int first = INT_MAX, last = 0;
  for (int index = std::max(0, std::min(anchor_, selected_));
       index <= std::max(anchor_, selected_) && index < static_cast<int>(rows_.size()); ++index)
  {
    const auto &row = rows_[static_cast<size_t>(index)];
    const DiffLine *value = line(row, row.right);
    if (!value)
      value = line(row, row.left);
    if (value && value->newLine)
    {
      first = std::min(first, *value->newLine);
      last = std::max(last, *value->newLine);
    }
  }
  return first == INT_MAX ? std::nullopt : std::make_optional(std::pair{first, last});
}
int DiffView::selectedComment() const
{
  if (anchor_ < 0 || selected_ < 0)
    return -1;
  int found = INT_MAX;
  for (int index = std::max(0, std::min(anchor_, selected_));
       index <= std::max(anchor_, selected_) && index < static_cast<int>(rows_.size()); ++index)
    if (rows_[static_cast<size_t>(index)].comment != noLine)
      found = std::min(found, static_cast<int>(rows_[static_cast<size_t>(index)].comment));
  return found == INT_MAX ? -1 : found;
}

int DiffView::pageRows() const
{
  RECT r{};
  GetClientRect(hwnd_, &r);
  return std::max(1, (static_cast<int>(r.bottom) - headerHeight_) / rowHeight_);
}
void DiffView::updateScroll()
{
  if (!hwnd_)
    return;
  int count = static_cast<int>(rows_.size()), page = pageRows();
  top_ = std::clamp(top_, 0, std::max(0, count - page));
  RECT r{};
  GetClientRect(hwnd_, &r);
  int contentRight = std::max(1, static_cast<int>(r.right) - scrollbarWidth());
  int width =
    plain_ ? std::max(1, contentRight - MulDiv(28, static_cast<int>(dpi_), 96))
           : std::max(1, (side_ ? contentRight / 2 : contentRight) - (side_ ? numberDigits_ + 2 : numberDigits_ * 2 + 3) * charWidth_);
  horizontal_ = std::clamp(horizontal_, 0, std::max(0, maxWidth_ - width));
  SCROLLINFO horizontal{
    sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, std::max(0, maxWidth_ - 1), static_cast<UINT>(width), horizontal_, 0};
  SetScrollInfo(hwnd_, SB_HORZ, &horizontal, TRUE);
}
void DiffView::scrollTo(int row)
{
  activeChange_ = -1;
  top_ = row;
  updateScroll();
  InvalidateRect(hwnd_, nullptr, FALSE);
}
void DiffView::stopChangeFlash()
{
  if (hwnd_)
    KillTimer(hwnd_, changeFlashTimer);
  flashFirst_ = flashLast_ = -1;
}
int DiffView::activeChangeStart() const
{
  return activeChange_ >= 0 && activeChange_ < static_cast<int>(navigationBlocks_.size())
           ? static_cast<int>(navigationBlocks_[static_cast<size_t>(activeChange_)].first)
           : -1;
}
int DiffView::activeChangeEnd() const
{
  return activeChange_ >= 0 && activeChange_ < static_cast<int>(navigationBlocks_.size())
           ? static_cast<int>(navigationBlocks_[static_cast<size_t>(activeChange_)].last)
           : -1;
}
void DiffView::showChangeBlock(size_t index, bool flash)
{
  if (index >= navigationBlocks_.size())
    return;
  const auto &block = navigationBlocks_[index];
  int first = static_cast<int>(block.first), last = static_cast<int>(block.last);
  int page = pageRows(), height = last - first + 1;
  top_ = height >= page ? first : first - (page - height) / 2;
  updateScroll();
  activeChange_ = static_cast<int>(index);
  stopChangeFlash();
  if (flash)
  {
    flashFirst_ = first;
    flashLast_ = last;
    SetTimer(hwnd_, changeFlashTimer, 100, nullptr);
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}
void DiffView::navigateChange(int direction)
{
  if (navigationBlocks_.empty() || !direction)
    return;
  int target = activeChange_;
  if (target >= 0)
    target = std::clamp(target + (direction < 0 ? -1 : 1), 0, static_cast<int>(navigationBlocks_.size()) - 1);
  else if (direction > 0)
  {
    target = 0;
    for (size_t i = 0; i < navigationBlocks_.size(); ++i)
      if (static_cast<int>(navigationBlocks_[i].last) >= top_)
      {
        target = static_cast<int>(i);
        break;
      }
  }
  else
  {
    target = 0;
    for (size_t i = navigationBlocks_.size(); i-- > 0;)
      if (static_cast<int>(navigationBlocks_[i].first) <= top_)
      {
        target = static_cast<int>(i);
        break;
      }
  }
  if (target != activeChange_)
    showChangeBlock(static_cast<size_t>(target), true);
}
void DiffView::showFirstChange()
{
  if (!navigationBlocks_.empty())
    showChangeBlock(0, false);
}
void DiffView::setChangeMinimap(bool enabled)
{
  if (minimap_ == enabled)
    return;
  minimap_ = enabled;
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);
}
int DiffView::scrollbarWidth() const
{
  return std::max(MulDiv(14, static_cast<int>(dpi_), 96), GetSystemMetricsForDpi(SM_CXVSCROLL, dpi_));
}
RECT DiffView::scrollbarRect() const
{
  RECT area{};
  GetClientRect(hwnd_, &area);
  area.left = std::max<LONG>(0, area.right - scrollbarWidth());
  return area;
}
RECT DiffView::scrollbarThumbRect() const
{
  RECT track = scrollbarRect();
  InflateRect(&track, -MulDiv(2, static_cast<int>(dpi_), 96), -MulDiv(2, static_cast<int>(dpi_), 96));
  int count = static_cast<int>(rows_.size()), page = pageRows(), height = track.bottom - track.top;
  if (count <= page || height <= 0)
    return track;
  int thumbHeight = std::max(scrollbarWidth(), MulDiv(height, page, count));
  thumbHeight = std::min(thumbHeight, height);
  int range = std::max(1, count - page), travel = height - thumbHeight;
  int y = track.top + MulDiv(top_, travel, range);
  return {track.left, y, track.right, y + thumbHeight};
}
void DiffView::dragVerticalScrollbar(int y)
{
  RECT track = scrollbarRect(), thumb = scrollbarThumbRect();
  InflateRect(&track, -MulDiv(2, static_cast<int>(dpi_), 96), -MulDiv(2, static_cast<int>(dpi_), 96));
  int travel = (track.bottom - track.top) - (thumb.bottom - thumb.top);
  int range = std::max(0, static_cast<int>(rows_.size()) - pageRows());
  if (travel <= 0 || !range)
    return;
  int thumbTop = std::clamp(y - scrollbarDragOffset_, static_cast<int>(track.top), static_cast<int>(track.top) + travel);
  scrollTo(MulDiv(thumbTop - track.top, range, travel));
}
void DiffView::drawVerticalScrollbar(HDC dc, const RECT &area) const
{
  RECT bar{std::max<LONG>(0, area.right - scrollbarWidth()), 0, area.right, area.bottom};
  fill(dc, bar, ThemeColor::Window);
  RECT border = bar;
  border.right = border.left + std::max(1, MulDiv(1, static_cast<int>(dpi_), 96));
  fill(dc, border, ThemeColor::Border);
  RECT thumb = scrollbarThumbRect();
  COLORREF thumbColor = themeColor(ThemeColor::Border);
  if (scrollbarHover_ || scrollbarDragging_)
  {
    COLORREF target = themeColor(ThemeColor::Text);
    auto mix = [&](int a, int b) { return (a * 3 + b) / 4; };
    thumbColor = RGB(mix(GetRValue(thumbColor), GetRValue(target)), mix(GetGValue(thumbColor), GetGValue(target)),
      mix(GetBValue(thumbColor), GetBValue(target)));
  }
  fillColor(dc, thumb, thumbColor);
  if (!minimap_ || changeBlocks_.empty() || rows_.empty())
    return;
  RECT track = bar;
  InflateRect(&track, -MulDiv(2, static_cast<int>(dpi_), 96), -MulDiv(2, static_cast<int>(dpi_), 96));
  int availableWidth = std::max(1, static_cast<int>(track.right - track.left));
  int markerWidth = std::max(2, availableWidth / 2);
  int markerLeft = (track.left + track.right - markerWidth) / 2;
  track.left = markerLeft;
  track.right = markerLeft + markerWidth;
  int count = static_cast<int>(rows_.size()), height = track.bottom - track.top;
  int thickness = std::max(1, (height + 1023) / 1024);
  for (const auto &block : changeBlocks_)
  {
    int first = track.top + MulDiv(static_cast<int>(block.first), height, count);
    int last = track.top + MulDiv(static_cast<int>(block.last + 1), height, count);
    if (last - first < thickness)
    {
      int center = (first + last) / 2;
      first = std::clamp(center - thickness / 2, static_cast<int>(track.top), static_cast<int>(track.bottom) - thickness);
      last = first + thickness;
    }
    RECT marker{track.left, first, track.right, last};
    if (block.added && block.removed)
    {
      int center = (marker.left + marker.right) / 2;
      auto removed = marker;
      removed.right = center;
      if (removed.left < removed.right)
        fill(dc, removed, ThemeColor::RemovedIndicator);
      marker.left = center;
      if (marker.left < marker.right)
        fill(dc, marker, ThemeColor::AddedIndicator);
    }
    else
      fill(dc, marker, block.removed ? ThemeColor::RemovedIndicator : ThemeColor::AddedIndicator);
  }
}
const DiffLine *DiffView::line(const PresentationRow &row, size_t index) const
{
  if (!file_ || row.hunk == noLine || index == noLine)
    return nullptr;
  return &file_->hunks[row.hunk].lines[index];
}
void DiffView::paint(HDC printDC)
{
  PAINTSTRUCT ps{};
  HDC target = printDC ? printDC : BeginPaint(hwnd_, &ps);
  RECT area{};
  GetClientRect(hwnd_, &area);
  if (area.right <= 0 || area.bottom <= 0)
  {
    if (!printDC)
      EndPaint(hwnd_, &ps);
    return;
  }
  HDC dc = CreateCompatibleDC(target);
  HBITMAP bitmap = CreateCompatibleBitmap(target, area.right, area.bottom);
  auto oldBitmap = SelectObject(dc, bitmap);
  auto oldFont = SelectObject(dc, font_);
  SetBkMode(dc, TRANSPARENT);
  fill(dc, area, ThemeColor::Surface);
  int contentRight = std::max(1, static_cast<int>(area.right) - scrollbarWidth());
  int pad = MulDiv(14, static_cast<int>(dpi_), 96), half = contentRight / 2;
  RECT header{0, 0, contentRight, headerHeight_};
  if (!plain_)
  {
    fill(dc, header, ThemeColor::Window);
    RECT title{pad, 0, contentRight - pad, headerHeight_ / 2};
    text(dc, title, file_ ? file_->path() : L"GitDiffViewer", ThemeColor::Title,
      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    RECT labels{pad, headerHeight_ / 2, contentRight - pad, headerHeight_};
    if (side_)
    {
      auto left = labels;
      left.right = half;
      text(dc, left, L"BEFORE", ThemeColor::BeforeText);
      labels.left = half + pad;
      text(dc, labels, L"AFTER", ThemeColor::AfterText);
    }
    else
      text(dc, labels, L"OLD     NEW     UNIFIED DIFF", ThemeColor::HeaderText);
  }
  if (!file_)
  {
    RECT empty{pad * 2, headerHeight_ + pad * 2, contentRight - pad * 2, area.bottom - pad};
    text(dc, empty, message_, ThemeColor::MutedText, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
  }
  auto drawCell = [&](RECT r, const DiffLine *l, bool oldSide, bool unified, bool selected, bool flashing, bool marked) {
    auto color = themeColor(selected ? ThemeColor::Selection : background(l));
    fillColor(dc, r, flashing ? flashColor(color) : color);
    if (!l)
      return;
    int gutter = (unified ? numberDigits_ * 2 + 3 : numberDigits_ + 2) * charWidth_;
    RECT nums = r;
    nums.right = r.left + gutter;
    std::wstring number;
    auto field = [this](std::optional<int> n) {
      std::wstring s = n ? std::to_wstring(*n) : L"";
      if (s.size() < static_cast<size_t>(numberDigits_))
        s.insert(0, static_cast<size_t>(numberDigits_) - s.size(), L' ');
      return s;
    };
    if (unified)
      number = field(l->oldLine) + field(l->newLine) + L" ";
    else
      number = field(oldSide ? l->oldLine : l->newLine) + L" ";
    number += l->type == DiffLineType::Added ? L'+' : l->type == DiffLineType::Removed ? L'-' : L' ';
    text(dc, nums, number, ThemeColor::LineNumber);
    if (marked)
      fill(dc, {nums.right - std::max(4, MulDiv(5, static_cast<int>(dpi_), 96)), r.top, nums.right, r.bottom},
        ThemeColor::CommentIndicator);
    RECT content = r;
    content.left += gutter;
    int saved = SaveDC(dc);
    IntersectClipRect(dc, content.left, content.top, content.right, content.bottom);
    std::wstring expanded;
    expanded.reserve(l->text.size());
    size_t column = 0;
    for (auto c : l->text)
    {
      if (c == L'\t')
      {
        size_t n = 4 - column % 4;
        expanded.append(n, L' ');
        column += n;
      }
      else
      {
        expanded += c == L'\r' ? L'↵' : c;
        ++column;
      }
    }
    content.left -= horizontal_;
    content.right = std::max(content.right, content.left + maxWidth_ + charWidth_);
    text(dc, content, expanded, ThemeColor::DiffText);
    RestoreDC(dc, saved);
  };
  int visible = (area.bottom - headerHeight_ + rowHeight_ - 1) / rowHeight_;
  for (int n = 0; n < visible && top_ + n < static_cast<int>(rows_.size()); ++n)
  {
    int index = top_ + n;
    const auto &row = rows_[index];
    RECT r{0, headerHeight_ + n * rowHeight_, contentRight, headerHeight_ + (n + 1) * rowHeight_};
    bool selected = anchor_ >= 0 && selected_ >= 0 && index >= std::min(anchor_, selected_) && index <= std::max(anchor_, selected_);
    bool flashing = index >= flashFirst_ && index <= flashLast_;
    if (row.comment != noLine)
    {
      fill(dc, r, ThemeColor::Comment);
      int gutter = (side_ ? numberDigits_ + 2 : numberDigits_ * 2 + 3) * charWidth_;
      int boundary = (side_ ? half : 0) + gutter;
      fill(dc, {boundary - std::max(4, MulDiv(5, static_cast<int>(dpi_), 96)), r.top, boundary, r.bottom},
        ThemeColor::CommentIndicator);
      r.left = boundary + charWidth_ - horizontal_;
      text(dc, r, row.commentText, ThemeColor::CommentText);
    }
    else if (!row.meta.empty())
    {
      auto color = themeColor(selected ? ThemeColor::Selection : plain_ ? ThemeColor::Surface : ThemeColor::Metadata);
      fillColor(dc, r, flashing ? flashColor(color) : color);
      r.left += pad - horizontal_;
      text(dc, r, row.meta, plain_ ? ThemeColor::Text : ThemeColor::MetadataText);
    }
    else if (side_)
    {
      auto left = r;
      left.right = half;
      drawCell(left, line(row, row.left), true, false, selected, flashing, false);
      auto right = r;
      right.left = half;
      const DiffLine *after = line(row, row.right);
      bool marked = after && after->newLine && std::any_of(comments_.begin(), comments_.end(), [&](const ReviewComment &comment) {
        return *after->newLine >= comment.firstLine && *after->newLine <= comment.lastLine;
      });
      drawCell(right, after, false, false, selected, flashing, marked);
    }
    else
    {
      const DiffLine *value = line(row, row.left != noLine ? row.left : row.right);
      bool marked = value && value->newLine && std::any_of(comments_.begin(), comments_.end(), [&](const ReviewComment &comment) {
        return *value->newLine >= comment.firstLine && *value->newLine <= comment.lastLine;
      });
      drawCell(r, value, false, true, selected, flashing, marked);
    }
  }
  if (side_ && !plain_)
    fill(dc, {half, headerHeight_ / 2, half + 1, area.bottom}, ThemeColor::Border);
  if (autoScroll_)
  {
    int radius = MulDiv(10, static_cast<int>(dpi_), 96);
    auto pen = CreatePen(PS_SOLID, 1, themeColor(ThemeColor::Text));
    auto brush = CreateSolidBrush(themeColor(ThemeColor::Window));
    auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, brush);
    int x = autoOrigin_.x, y = autoOrigin_.y;
    Ellipse(dc, x - radius, y - radius, x + radius + 1, y + radius + 1);
    POINT up[] = {{x - 3, y - 3}, {x, y - 6}, {x + 3, y - 3}};
    POINT down[] = {{x - 3, y + 3}, {x, y + 6}, {x + 3, y + 3}};
    Polyline(dc, up, 3);
    Polyline(dc, down, 3);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
  }
  drawVerticalScrollbar(dc, area);
  BitBlt(target, 0, 0, area.right, area.bottom, dc, 0, 0, SRCCOPY);
  SelectObject(dc, oldFont);
  SelectObject(dc, oldBitmap);
  DeleteObject(bitmap);
  DeleteDC(dc);
  if (!printDC)
    EndPaint(hwnd_, &ps);
}
void DiffView::copy()
{
  if (anchor_ < 0 || selected_ < 0 || rows_.empty())
    return;
  std::wstring output;
  for (int i = std::min(anchor_, selected_); i <= std::max(anchor_, selected_) && i < static_cast<int>(rows_.size()); ++i)
  {
    const auto &row = rows_[i];
    auto left = line(row, row.left), right = line(row, row.right);
    if (row.comment != noLine)
      output += row.commentText;
    else if (!row.meta.empty())
      output += row.meta;
    else if (side_)
    {
      if (left)
        output += left->text;
      output += L'\t';
      if (right)
        output += right->text;
    }
    else if (left || right)
      output += (left ? left : right)->text;
    output += L"\r\n";
  }
  if (anchor_ == selected_ && output.size() >= 2)
    output.resize(output.size() - 2);
  if (!OpenClipboard(hwnd_))
    return;
  SIZE_T bytes = (output.size() + 1) * sizeof(wchar_t);
  HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
  if (data)
  {
    void *p = GlobalLock(data);
    if (p)
    {
      memcpy(p, output.c_str(), bytes);
      GlobalUnlock(data);
      EmptyClipboard();
      if (!SetClipboardData(CF_UNICODETEXT, data))
        GlobalFree(data);
    }
    else
      GlobalFree(data);
  }
  CloseClipboard();
}
LRESULT CALLBACK DiffView::procedure(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
  auto self = reinterpret_cast<DiffView *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE)
  {
    self = static_cast<DiffView *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->message(msg, w, l) : DefWindowProcW(hwnd, msg, w, l);
}
void DiffView::stopAutoScroll()
{
  if (!autoScroll_)
    return;
  autoScroll_ = false;
  KillTimer(hwnd_, autoScrollTimer);
  if (GetCapture() == hwnd_)
    ReleaseCapture();
  SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
  InvalidateRect(hwnd_, nullptr, FALSE);
}
LRESULT DiffView::message(UINT msg, WPARAM w, LPARAM l)
{
  switch (msg)
  {
    case WM_CREATE: setDpi(GetDpiForWindow(hwnd_)); return 0;
    case WM_PAINT: paint(); return 0;
    case WM_PRINTCLIENT: paint(reinterpret_cast<HDC>(w)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
      updateScroll();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_HSCROLL:
    {
      SCROLLINFO info{sizeof(info), SIF_ALL};
      GetScrollInfo(hwnd_, SB_HORZ, &info);
      int pos = horizontal_, step = charWidth_ * 3;
      switch (LOWORD(w))
      {
        case SB_LINEUP: pos -= step; break;
        case SB_LINEDOWN: pos += step; break;
        case SB_PAGEUP: pos -= info.nPage; break;
        case SB_PAGEDOWN: pos += info.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = info.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = info.nMax; break;
      }
      horizontal_ = pos;
      updateScroll();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_MBUTTONDOWN:
    {
      if (autoScroll_)
      {
        stopAutoScroll();
        return 0;
      }
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      RECT bar = scrollbarRect();
      if (rows_.empty() || GET_Y_LPARAM(l) < headerHeight_ || PtInRect(&bar, point))
        return 0;
      SetFocus(hwnd_);
      dragging_ = false;
      autoOrigin_ = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      autoRemainder_ = 0;
      autoTick_ = GetTickCount64();
      autoScroll_ = true;
      SetCapture(hwnd_);
      SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
      SetTimer(hwnd_, autoScrollTimer, 16, nullptr);
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_MBUTTONUP: return 0;
    case WM_TIMER:
      if (w == changeFlashTimer && changeFlashing())
      {
        stopChangeFlash();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      if (w == autoScrollTimer && autoScroll_)
      {
        POINT cursor{};
        GetCursorPos(&cursor);
        ScreenToClient(hwnd_, &cursor);
        int dy = MulDiv(cursor.y - autoOrigin_.y, 96, static_cast<int>(dpi_));
        auto now = GetTickCount64();
        double seconds = std::min<ULONGLONG>(now - autoTick_, 100) / 1000.0;
        autoTick_ = now;
        int distance = std::max(0, std::abs(dy) - 12);
        if (!distance)
          autoRemainder_ = 0;
        else
          autoRemainder_ += (dy < 0 ? -1 : 1) * std::min(600.0, distance * distance / 250.0 + distance / 12.0) * seconds;
        int lines = static_cast<int>(autoRemainder_);
        autoRemainder_ -= lines;
        if (lines)
          scrollTo(top_ + lines);
      }
      return 0;
    case WM_SETCURSOR:
      if (autoScroll_)
      {
        SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
        return TRUE;
      }
      if (LOWORD(l) == HTCLIENT)
      {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(hwnd_, &point);
        RECT bar = scrollbarRect();
        if (PtInRect(&bar, point))
        {
          SetCursor(LoadCursorW(nullptr, IDC_ARROW));
          return TRUE;
        }
      }
      break;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
      scrollbarDragging_ = false;
      stopAutoScroll();
      break;
    case WM_RBUTTONDOWN:
      if (autoScroll_)
      {
        stopAutoScroll();
        return 0;
      }
      break;
    case WM_MOUSEWHEEL:
    {
      stopAutoScroll();
      int delta = GET_WHEEL_DELTA_WPARAM(w);
      MSG queued{};
      while (PeekMessageW(&queued, hwnd_, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_REMOVE))
        delta += GET_WHEEL_DELTA_WPARAM(queued.wParam);
      if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL)
      {
        zoomWheel_ += delta;
        zoom(zoomWheel_ / WHEEL_DELTA);
        zoomWheel_ %= WHEEL_DELTA;
        return 0;
      }
      wheel_ += delta;
      int lines = (wheel_ / WHEEL_DELTA) * 3;
      wheel_ %= WHEEL_DELTA;
      if (lines)
        scrollTo(top_ - lines);
      return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
      if (GET_Y_LPARAM(l) < headerHeight_ || rows_.empty())
        return 0;
      int row = std::clamp(top_ + (GET_Y_LPARAM(l) - headerHeight_) / rowHeight_, 0, static_cast<int>(rows_.size()) - 1);
      if (rows_[static_cast<size_t>(row)].comment == noLine)
        return 0;
      selected_ = anchor_ = row;
      InvalidateRect(hwnd_, nullptr, FALSE);
      SendMessageW(GetParent(hwnd_), WM_APP + 2, 0, 0);
      return 0;
    }
    case WM_LBUTTONDOWN:
    {
      if (autoScroll_)
      {
        stopAutoScroll();
        return 0;
      }
      SetFocus(hwnd_);
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      RECT bar = scrollbarRect();
      if (PtInRect(&bar, point))
      {
        RECT thumb = scrollbarThumbRect();
        if (static_cast<int>(rows_.size()) > pageRows())
        {
          scrollbarDragging_ = true;
          scrollbarDragOffset_ = PtInRect(&thumb, point) ? point.y - thumb.top : (thumb.bottom - thumb.top) / 2;
          SetCapture(hwnd_);
          dragVerticalScrollbar(point.y);
        }
        scrollbarHover_ = true;
        InvalidateRect(hwnd_, &bar, FALSE);
        return 0;
      }
      if (GET_Y_LPARAM(l) < headerHeight_ || rows_.empty())
        return 0;
      selected_ = std::min(static_cast<int>(rows_.size()) - 1, top_ + (GET_Y_LPARAM(l) - headerHeight_) / rowHeight_);
      if (!(GetKeyState(VK_SHIFT) & 0x8000) || anchor_ < 0)
        selectedAfter_ = !side_ || point.x >= (bar.left / 2);
      if (!(GetKeyState(VK_SHIFT) & 0x8000) || anchor_ < 0)
        anchor_ = selected_;
      dragging_ = true;
      SetCapture(hwnd_);
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE:
    {
      POINT point{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      RECT bar = scrollbarRect();
      bool hover = PtInRect(&bar, point) || scrollbarDragging_;
      if (hover != scrollbarHover_)
      {
        scrollbarHover_ = hover;
        InvalidateRect(hwnd_, &bar, FALSE);
      }
      TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, hwnd_, 0};
      TrackMouseEvent(&tracking);
      if (scrollbarDragging_)
      {
        dragVerticalScrollbar(point.y);
        return 0;
      }
      if (dragging_ && !rows_.empty())
      {
        selected_ = std::clamp(top_ + (GET_Y_LPARAM(l) - headerHeight_) / rowHeight_, 0, static_cast<int>(rows_.size()) - 1);
        if (selected_ < top_)
          scrollTo(selected_);
        else if (selected_ >= top_ + pageRows())
          scrollTo(selected_ - pageRows() + 1);
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (scrollbarHover_ && !scrollbarDragging_)
      {
        scrollbarHover_ = false;
        RECT bar = scrollbarRect();
        InvalidateRect(hwnd_, &bar, FALSE);
      }
      return 0;
    case WM_LBUTTONUP:
      if (scrollbarDragging_)
      {
        scrollbarDragging_ = false;
        if (GetCapture() == hwnd_)
          ReleaseCapture();
        RECT bar = scrollbarRect();
        InvalidateRect(hwnd_, &bar, FALSE);
        return 0;
      }
      dragging_ = false;
      if (GetCapture() == hwnd_)
        ReleaseCapture();
      return 0;
    case WM_CAPTURECHANGED:
      dragging_ = false;
      scrollbarDragging_ = false;
      stopAutoScroll();
      return 0;
    case WM_KEYDOWN:
    {
      if (autoScroll_)
      {
        stopAutoScroll();
        if (w == VK_ESCAPE)
          return 0;
      }
      if (w == 'C' && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000))
      {
        SendMessageW(GetParent(hwnd_), WM_APP + 2, 0, 0);
        return 0;
      }
      if ((GetKeyState(VK_CONTROL) & 0x8000) && w == 'C')
      {
        copy();
        return 0;
      }
      if ((GetKeyState(VK_CONTROL) & 0x8000) && w == 'A' && !rows_.empty())
      {
        anchor_ = 0;
        selected_ = static_cast<int>(rows_.size()) - 1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      int next = selected_ >= 0 ? selected_ : top_;
      switch (w)
      {
        case VK_UP: --next; break;
        case VK_DOWN: ++next; break;
        case VK_PRIOR: next -= pageRows(); break;
        case VK_NEXT: next += pageRows(); break;
        case VK_HOME: next = 0; break;
        case VK_END: next = static_cast<int>(rows_.size()) - 1; break;
        case VK_LEFT:
          horizontal_ -= charWidth_ * 3;
          updateScroll();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return 0;
        case VK_RIGHT:
          horizontal_ += charWidth_ * 3;
          updateScroll();
          InvalidateRect(hwnd_, nullptr, FALSE);
          return 0;
        default: return DefWindowProcW(hwnd_, msg, w, l);
      }
      if (!rows_.empty())
      {
        selected_ = std::clamp(next, 0, static_cast<int>(rows_.size()) - 1);
        if (!(GetKeyState(VK_SHIFT) & 0x8000) || anchor_ < 0)
          anchor_ = selected_;
        if (selected_ < top_)
          scrollTo(selected_);
        else if (selected_ >= top_ + pageRows())
          scrollTo(selected_ - pageRows() + 1);
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    }
  }
  return DefWindowProcW(hwnd_, msg, w, l);
}
} // namespace gdv
