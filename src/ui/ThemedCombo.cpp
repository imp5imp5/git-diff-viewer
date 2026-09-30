#include "ThemedCombo.h"
#include "Theme.h"
#include <commctrl.h>
#include <uxtheme.h>
namespace gdv
{
namespace
{
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR)
{
  if (darkTheme && (message == WM_PAINT || message == WM_PRINTCLIENT))
  {
    PAINTSTRUCT paint{};
    HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(w);
    int saved = SaveDC(dc);
    RECT bounds{};
    GetClientRect(window, &bounds);
    auto surface = CreateSolidBrush(themeColor(ThemeColor::Surface));
    FillRect(dc, &bounds, surface);
    DeleteObject(surface);
    COMBOBOXINFO info{sizeof(info)};
    GetComboBoxInfo(window, &info);
    // Editable combos keep their native edit child. Dropdown-list combos ask
    // the owner to draw the selected item, including any custom formatting.
    if ((GetWindowLongPtrW(window, GWL_STYLE) & 3) == CBS_DROPDOWNLIST)
    {
      DRAWITEMSTRUCT item{};
      item.CtlType = ODT_COMBOBOX;
      item.CtlID = GetDlgCtrlID(window);
      item.itemID = static_cast<UINT>(SendMessageW(window, CB_GETCURSEL, 0, 0));
      item.itemAction = ODA_DRAWENTIRE;
      item.hwndItem = window;
      item.hDC = dc;
      item.rcItem = bounds;
      InflateRect(&item.rcItem, -2, -2);
      item.rcItem.right = info.rcButton.left;
      if (GetFocus() == window && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
        item.itemState = ODS_FOCUS;
      SendMessageW(GetParent(window), WM_DRAWITEM, item.CtlID, reinterpret_cast<LPARAM>(&item));
    }
    auto border = CreateSolidBrush(themeColor(ThemeColor::Border));
    FrameRect(dc, &bounds, border);
    DeleteObject(border);
    int x = (info.rcButton.left + info.rcButton.right) / 2;
    int y = (bounds.top + bounds.bottom) / 2;
    int size = MulDiv(3, static_cast<int>(GetDpiForWindow(window)), 96);
    POINT arrow[] = {{x - size, y - 1}, {x, y + size - 1}, {x + size, y - 1}};
    auto pen = CreatePen(PS_SOLID, 1, themeColor(ThemeColor::Text));
    auto oldPen = SelectObject(dc, pen);
    Polyline(dc, arrow, 3);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    RestoreDC(dc, saved);
    if (message == WM_PAINT)
      EndPaint(window, &paint);
    return 0;
  }
  auto result = DefSubclassProc(window, message, w, l);
  if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == CB_SETCURSEL || message == CB_SHOWDROPDOWN ||
      message == WM_KEYDOWN || message == WM_LBUTTONUP || message == WM_ENABLE || message == WM_SETFONT)
    InvalidateRect(window, nullptr, FALSE);
  if (message == WM_NCDESTROY)
    RemoveWindowSubclass(window, procedure, id);
  return result;
}
} // namespace
void installThemedCombo(HWND window)
{
  COMBOBOXINFO info{sizeof(info)};
  if (GetComboBoxInfo(window, &info))
  {
    SetWindowTheme(info.hwndList, darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    if (darkTheme && (GetWindowLongPtrW(window, GWL_STYLE) & 3) == CBS_DROPDOWN)
    {
      auto style = GetWindowLongPtrW(info.hwndItem, GWL_STYLE);
      auto extended = GetWindowLongPtrW(info.hwndItem, GWL_EXSTYLE);
      SetWindowLongPtrW(info.hwndItem, GWL_STYLE, style & ~WS_BORDER);
      SetWindowLongPtrW(info.hwndItem, GWL_EXSTYLE, extended & ~WS_EX_CLIENTEDGE);
      SetWindowPos(info.hwndItem, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
  }
  SetWindowSubclass(window, procedure, 1, 0);
}
} // namespace gdv
