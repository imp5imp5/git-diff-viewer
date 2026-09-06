#pragma once
#include "diff/PresentationBuilder.h"
#include <windows.h>
namespace gdv
{
class DiffView
{
public:
  ~DiffView();
  HWND create(HWND parent, HINSTANCE instance);
  void setFile(const FileDiff *file, bool preserve = false, bool plainText = false);
  bool plainText() const { return plain_; }
  void setMessage(std::wstring message);
  void setSideBySide(bool enabled);
  void setDpi(UINT dpi);
  void zoom(int steps);
  int fontSize() const { return fontPoints_; }
  HWND handle() const { return hwnd_; }
  int topRow() const { return top_; }
  int rowCount() const { return static_cast<int>(rows_.size()); }
  std::wstring messageText() const { return file_ ? L"" : message_; }
  const FileDiff *file() const { return file_; }
  const std::vector<PresentationRow> &presentation() const { return rows_; }
  int visibleRowCount() const { return pageRows(); }
  void scroll(int row) { scrollTo(row); }

private:
  static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
  LRESULT message(UINT, WPARAM, LPARAM);
  void paint(HDC printDC = nullptr);
  void updateScroll();
  void scrollTo(int row);
  void copy();
  void stopAutoScroll();
  bool autoScroll_{};
  POINT autoOrigin_{};
  double autoRemainder_{};
  ULONGLONG autoTick_{};
  int pageRows() const;
  const DiffLine *line(const PresentationRow &row, size_t index) const;
  HWND hwnd_{};
  HFONT font_{};
  const FileDiff *file_{};
  std::vector<PresentationRow> rows_;
  std::wstring message_{L"Open a Git repository to review its local changes."};
  bool side_{}, dragging_{}, plain_{};
  int top_{}, horizontal_{}, selected_{-1}, anchor_{-1}, rowHeight_{22}, charWidth_{8}, headerHeight_{62}, maxWidth_{}, wheel_{},
    zoomWheel_{}, numberDigits_{7}, fontPoints_{11};
  UINT dpi_{96};
};
} // namespace gdv
