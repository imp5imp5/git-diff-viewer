#include "SearchDialog.h"
#include "Theme.h"
#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
namespace gdv
{
namespace
{
constexpr int findId = 1, cancelId = 2, textId = 3;
struct Dialog
{
  HWND window{}, combo{};
  HBRUSH background{}, field{};
  HFONT font{};
  FindDialogResult result;
};
LRESULT CALLBACK comboProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR)
{
  if (darkTheme && (message == WM_PAINT || message == WM_PRINTCLIENT))
  {
    PAINTSTRUCT paint{};
    HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(w);
    RECT bounds{};
    GetClientRect(window, &bounds);
    auto background = CreateSolidBrush(themeColor(ThemeColor::Surface));
    FillRect(dc, &bounds, background);
    DeleteObject(background);
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(window, &info))
    {
      int x = (info.rcButton.left + info.rcButton.right) / 2;
      int y = (bounds.top + bounds.bottom) / 2;
      int size = std::max(3, MulDiv(3, static_cast<int>(GetDpiForWindow(window)), 96));
      POINT arrow[] = {{x - size, y - 1}, {x, y + size - 1}, {x + size, y - 1}};
      auto pen = CreatePen(PS_SOLID, 1, themeColor(ThemeColor::Text));
      auto oldPen = SelectObject(dc, pen);
      Polyline(dc, arrow, 3);
      SelectObject(dc, oldPen);
      DeleteObject(pen);
    }
    auto border = CreateSolidBrush(themeColor(ThemeColor::Border));
    FrameRect(dc, &bounds, border);
    DeleteObject(border);
    if (message == WM_PAINT)
      EndPaint(window, &paint);
    return 0;
  }
  auto result = DefSubclassProc(window, message, w, l);
  if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == CB_SHOWDROPDOWN || message == WM_ENABLE)
    InvalidateRect(window, nullptr, FALSE);
  if (message == WM_NCDESTROY)
    RemoveWindowSubclass(window, comboProcedure, id);
  return result;
}
std::wstring text(HWND window)
{
  int length = GetWindowTextLengthW(window);
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(window, result.data(), length + 1);
  result.resize(length);
  return result;
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
  auto dialog = reinterpret_cast<Dialog *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE)
  {
    dialog = static_cast<Dialog *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    dialog->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
  }
  if (!dialog)
    return DefWindowProcW(window, message, w, l);
  switch (message)
  {
    case WM_ERASEBKGND:
    {
      RECT area{};
      GetClientRect(window, &area);
      FillRect(reinterpret_cast<HDC>(w), &area, dialog->background);
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
      auto dc = reinterpret_cast<HDC>(w);
      bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
      SetTextColor(dc, themeColor(ThemeColor::Text));
      SetBkColor(dc, themeColor(field ? ThemeColor::Surface : ThemeColor::Window));
      return reinterpret_cast<LRESULT>(field ? dialog->field : dialog->background);
    }
    case WM_COMMAND:
      if (LOWORD(w) == findId)
      {
        dialog->result = {true, text(dialog->combo)};
        DestroyWindow(window);
        return 0;
      }
      if (LOWORD(w) == cancelId)
      {
        DestroyWindow(window);
        return 0;
      }
      break;
    case WM_CLOSE: DestroyWindow(window); return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
} // namespace
FindDialogResult findDiffText(HWND owner, HINSTANCE instance, const std::wstring &current, const std::vector<std::wstring> &history)
{
  static bool registered = false;
  if (!registered)
  {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = L"GitDiffViewer.Find";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassW(&cls) != 0;
  }
  Dialog state;
  state.background = CreateSolidBrush(themeColor(ThemeColor::Window));
  state.field = CreateSolidBrush(themeColor(ThemeColor::Surface));
  int dpi = static_cast<int>(GetDpiForWindow(owner));
  state.font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
  int width = scale(460), height = scale(176);
  RECT ownerRect{};
  GetWindowRect(owner, &ownerRect);
  HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"GitDiffViewer.Find", L"Find in diff",
    WS_POPUP | WS_CAPTION | WS_SYSMENU, (ownerRect.left + ownerRect.right - width) / 2,
    (ownerRect.top + ownerRect.bottom - height) / 2, width, height, owner, nullptr, instance, &state);
  if (!window)
  {
    if (state.font)
      DeleteObject(state.font);
    DeleteObject(state.field);
    DeleteObject(state.background);
    return {};
  }
  BOOL dark = darkTheme;
  if (FAILED(DwmSetWindowAttribute(window, 20, &dark, sizeof(dark))))
    DwmSetWindowAttribute(window, 19, &dark, sizeof(dark));
  COLORREF caption = themeColor(ThemeColor::Window), captionText = themeColor(ThemeColor::Text);
  DwmSetWindowAttribute(window, 35, &caption, sizeof(caption));
  DwmSetWindowAttribute(window, 36, &captionText, sizeof(captionText));
  RECT client{};
  GetClientRect(window, &client);
  int pad = scale(16), buttonWidth = scale(90), buttonHeight = scale(30);
  auto create = [&](const wchar_t *type, const wchar_t *label, DWORD style, int id, int left, int top, int controlWidth,
                  int controlHeight) {
    HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, left, top, controlWidth, controlHeight, window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    HFONT font = state.font ? state.font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SetWindowTheme(control, darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    return control;
  };
  create(L"STATIC", L"Text to find:", 0, 0, pad, pad, client.right - 2 * pad, scale(20));
  state.combo = create(L"COMBOBOX", L"", WS_TABSTOP | WS_CLIPCHILDREN | CBS_DROPDOWN | CBS_AUTOHSCROLL, textId, pad, pad + scale(23),
    client.right - 2 * pad, scale(180));
  for (const auto &entry : history)
    SendMessageW(state.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.c_str()));
  SetWindowTextW(state.combo, current.c_str());
  if (darkTheme)
  {
    COMBOBOXINFO info{sizeof(info)};
    if (GetComboBoxInfo(state.combo, &info))
    {
      auto style = GetWindowLongPtrW(info.hwndItem, GWL_STYLE);
      auto extended = GetWindowLongPtrW(info.hwndItem, GWL_EXSTYLE);
      SetWindowLongPtrW(info.hwndItem, GWL_STYLE, style & ~WS_BORDER);
      SetWindowLongPtrW(info.hwndItem, GWL_EXSTYLE, extended & ~WS_EX_CLIENTEDGE);
      SetWindowPos(info.hwndItem, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
      SetWindowTheme(info.hwndList, L"DarkMode_Explorer", nullptr);
    }
  }
  SetWindowSubclass(state.combo, comboProcedure, 1, 0);
  create(L"BUTTON", L"Cancel", WS_TABSTOP, cancelId, client.right - pad - buttonWidth * 2 - scale(8),
    client.bottom - pad - buttonHeight, buttonWidth, buttonHeight);
  create(L"BUTTON", L"Find", WS_TABSTOP | BS_DEFPUSHBUTTON, findId, client.right - pad - buttonWidth,
    client.bottom - pad - buttonHeight, buttonWidth, buttonHeight);
  EnableWindow(owner, FALSE);
  ShowWindow(window, SW_SHOW);
  SetFocus(state.combo);
  SendMessageW(state.combo, CB_SETEDITSEL, 0, -1);
  MSG message{};
  while (IsWindow(window))
  {
    BOOL received = GetMessageW(&message, nullptr, 0, 0);
    if (received <= 0)
      break;
    if (!IsDialogMessageW(window, &message))
    {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (IsWindow(window))
    DestroyWindow(window);
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  if (state.font)
    DeleteObject(state.font);
  DeleteObject(state.field);
  DeleteObject(state.background);
  return state.result;
}
} // namespace gdv