#pragma once
#include <windows.h>
#include <array>
namespace gdv
{
inline bool darkTheme = true;
enum class ThemeColor
{
  Window,
  Surface,
  Text,
  Title,
  DiffText,
  MutedText,
  LineNumber,
  HeaderText,
  BeforeText,
  AfterText,
  EmptyCell,
  Added,
  Removed,
  Selection,
  Metadata,
  MetadataText,
  Border,
  ListSelection,
  SelectionText,
  RemovedIndicator,
  AddedIndicator,
  Count
};
inline constexpr std::array<COLORREF, static_cast<size_t>(ThemeColor::Count)> lightPalette{{
  RGB(246, 248, 250), // Window
  RGB(255, 255, 255), // Surface
  RGB(0, 0, 0),       // Text
  RGB(28, 39, 54),    // Title
  RGB(31, 43, 58),    // DiffText
  RGB(84, 100, 120),  // MutedText
  RGB(112, 123, 137), // LineNumber
  RGB(91, 108, 129),  // HeaderText
  RGB(107, 80, 85),   // BeforeText
  RGB(51, 106, 74),   // AfterText
  RGB(241, 243, 246), // EmptyCell
  RGB(222, 247, 230), // Added
  RGB(255, 230, 231), // Removed
  RGB(210, 225, 249), // Selection
  RGB(234, 242, 251), // Metadata
  RGB(71, 108, 151),  // MetadataText
  RGB(216, 223, 232), // Border
  RGB(0, 120, 215),   // ListSelection
  RGB(255, 255, 255), // SelectionText
  RGB(225, 35, 45),   // RemovedIndicator
  RGB(0, 180, 65),    // AddedIndicator
}};
inline constexpr std::array<COLORREF, static_cast<size_t>(ThemeColor::Count)> darkPalette{{
  RGB(36, 40, 47),    // Window
  RGB(27, 30, 35),    // Surface
  RGB(222, 226, 232), // Text
  RGB(232, 235, 240), // Title
  RGB(222, 226, 232), // DiffText
  RGB(163, 178, 197), // MutedText
  RGB(147, 159, 175), // LineNumber
  RGB(163, 178, 197), // HeaderText
  RGB(229, 156, 164), // BeforeText
  RGB(147, 212, 165), // AfterText
  RGB(36, 40, 47),    // EmptyCell
  RGB(30, 65, 43),    // Added
  RGB(76, 36, 42),    // Removed
  RGB(48, 76, 111),   // Selection
  RGB(36, 46, 60),    // Metadata
  RGB(154, 189, 233), // MetadataText
  RGB(67, 75, 87),    // Border
  RGB(44, 85, 133),   // ListSelection
  RGB(255, 255, 255), // SelectionText
  RGB(255, 65, 75),   // RemovedIndicator
  RGB(35, 230, 95),   // AddedIndicator
}};
inline COLORREF themeColor(ThemeColor role) { return (darkTheme ? darkPalette : lightPalette)[static_cast<size_t>(role)]; }
inline COLORREF pathTextColor(bool selected)
{
  auto text = themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Text);
  auto background = themeColor(selected ? ThemeColor::ListSelection : ThemeColor::Surface);
  return RGB((GetRValue(text) * 7 + GetRValue(background) * 3) / 10, (GetGValue(text) * 7 + GetGValue(background) * 3) / 10,
    (GetBValue(text) * 7 + GetBValue(background) * 3) / 10);
}
} // namespace gdv
