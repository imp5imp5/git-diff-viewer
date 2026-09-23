#include "RefPicker.h"
#include "Theme.h"
#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <dwmapi.h>
#include <uxtheme.h>
namespace gdv
{
namespace
{
constexpr int filterId = 101, listId = 102, chooseId = 103, cancelId = 104;
struct Picker
{
  HWND window{}, filter{}, list{}, choose{};
  HBRUSH background{}, surface{};
  HFONT font{};
  RECT filterBorder{};
  std::vector<std::pair<std::wstring, std::wstring>> entries;
  std::vector<size_t> visible;
  std::wstring current;
  std::optional<std::wstring> result;
};
std::wstring text(HWND control)
{
  int length = GetWindowTextLengthW(control);
  std::wstring value(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(control, value.data(), length + 1);
  value.resize(length);
  return value;
}
bool containsIgnoreCase(const std::wstring &value, const std::wstring &needle)
{
  auto lower = [](std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return towlower(c); });
    return value;
  };
  return lower(value).find(lower(needle)) != std::wstring::npos;
}
void updateList(Picker &picker)
{
  auto filter = text(picker.filter);
  SendMessageW(picker.list, WM_SETREDRAW, FALSE, 0);
  SendMessageW(picker.list, LB_RESETCONTENT, 0, 0);
  picker.visible.clear();
  int selection = 0;
  for (size_t i = 0; i < picker.entries.size(); ++i)
  {
    const auto &entry = picker.entries[i];
    if (!containsIgnoreCase(entry.first, filter))
      continue;
    if (entry.second == picker.current)
      selection = static_cast<int>(picker.visible.size());
    picker.visible.push_back(i);
    SendMessageW(picker.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.first.c_str()));
  }
  if (!picker.visible.empty())
    SendMessageW(picker.list, LB_SETCURSEL, selection, 0);
  EnableWindow(picker.choose, !picker.visible.empty());
  SendMessageW(picker.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(picker.list, nullptr, TRUE);
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
  auto picker = reinterpret_cast<Picker *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE)
  {
    picker = static_cast<Picker *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    picker->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(picker));
  }
  if (!picker)
    return DefWindowProcW(window, message, w, l);
  switch (message)
  {
    case WM_ERASEBKGND:
    {
      RECT area{};
      GetClientRect(window, &area);
      FillRect(reinterpret_cast<HDC>(w), &area, picker->background);
      return 1;
    }
    case WM_PAINT:
    {
      PAINTSTRUCT paint{};
      auto dc = BeginPaint(window, &paint);
      auto border = CreateSolidBrush(themeColor(ThemeColor::Border));
      FrameRect(dc, &picker->filterBorder, border);
      DeleteObject(border);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    {
      auto dc = reinterpret_cast<HDC>(w);
      bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
      SetTextColor(dc, themeColor(ThemeColor::Text));
      SetBkColor(dc, themeColor(field ? ThemeColor::Surface : ThemeColor::Window));
      return reinterpret_cast<LRESULT>(field ? picker->surface : picker->background);
    }
    case WM_DRAWITEM:
    {
      auto item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
      if (item->CtlID != listId || item->itemID == static_cast<UINT>(-1) || item->itemID >= picker->visible.size())
        break;
      bool selected = (item->itemState & ODS_SELECTED) != 0;
      auto brush = CreateSolidBrush(themeColor(selected ? ThemeColor::ListSelection : ThemeColor::Surface));
      FillRect(item->hDC, &item->rcItem, brush);
      DeleteObject(brush);
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Text));
      auto oldFont = SelectObject(item->hDC, picker->font);
      RECT rect = item->rcItem;
      rect.left += 8;
      rect.right -= 8;
      const auto &label = picker->entries[picker->visible[item->itemID]].first;
      DrawTextW(item->hDC, label.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
      SelectObject(item->hDC, oldFont);
      if (item->itemState & ODS_FOCUS)
        DrawFocusRect(item->hDC, &item->rcItem);
      return TRUE;
    }
    case WM_NOTIFY:
    {
      auto header = reinterpret_cast<NMHDR *>(l);
      if (darkTheme && header->code == NM_CUSTOMDRAW && (header->idFrom == chooseId || header->idFrom == cancelId))
      {
        auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
          auto surface = CreateSolidBrush(themeColor(ThemeColor::Surface));
          FillRect(draw->hdc, &draw->rc, surface);
          DeleteObject(surface);
          auto border = CreateSolidBrush(themeColor(ThemeColor::Border));
          FrameRect(draw->hdc, &draw->rc, border);
          DeleteObject(border);
          SetBkMode(draw->hdc, TRANSPARENT);
          SetTextColor(draw->hdc, themeColor(ThemeColor::Text));
          auto label = text(header->hwndFrom);
          DrawTextW(draw->hdc, label.c_str(), -1, &draw->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          if (draw->uItemState & CDIS_FOCUS)
          {
            RECT focus = draw->rc;
            InflateRect(&focus, -3, -3);
            DrawFocusRect(draw->hdc, &focus);
          }
          return CDRF_SKIPDEFAULT;
        }
      }
      break;
    }
    case WM_COMMAND:
      if (LOWORD(w) == filterId && HIWORD(w) == EN_CHANGE)
      {
        updateList(*picker);
        return 0;
      }
      if (LOWORD(w) == chooseId || (LOWORD(w) == listId && HIWORD(w) == LBN_DBLCLK))
      {
        int index = static_cast<int>(SendMessageW(picker->list, LB_GETCURSEL, 0, 0));
        if (index >= 0 && static_cast<size_t>(index) < picker->visible.size())
        {
          picker->result = picker->entries[picker->visible[static_cast<size_t>(index)]].second;
          DestroyWindow(window);
        }
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
std::optional<std::wstring> chooseRef(HWND owner, HINSTANCE instance, HFONT font, const std::wstring &title,
  const std::wstring &resetLabel, const std::wstring &current, const std::vector<std::wstring> &refs)
{
  static bool registered = false;
  if (!registered)
  {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = L"GitDiffViewer.RefPicker";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassW(&cls) != 0;
  }
  Picker picker;
  picker.font = font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
  picker.current = current;
  picker.background = CreateSolidBrush(themeColor(ThemeColor::Window));
  picker.surface = CreateSolidBrush(themeColor(ThemeColor::Surface));
  picker.entries.emplace_back(resetLabel, L"");
  for (const auto &ref : refs)
    picker.entries.emplace_back(ref, ref);
  int dpi = static_cast<int>(GetDpiForWindow(owner));
  auto scale = [dpi](int value) { return MulDiv(value, dpi, 96); };
  int width = scale(560), height = scale(440);
  RECT ownerRect{};
  GetWindowRect(owner, &ownerRect);
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"GitDiffViewer.RefPicker", title.c_str(),
    WS_POPUP | WS_CAPTION | WS_SYSMENU, (ownerRect.left + ownerRect.right - width) / 2,
    (ownerRect.top + ownerRect.bottom - height) / 2, width, height, owner, nullptr, instance, &picker);
  if (!dialog)
  {
    DeleteObject(picker.surface);
    DeleteObject(picker.background);
    return {};
  }
  BOOL dark = darkTheme;
  if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark))))
    DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
  COLORREF caption = themeColor(ThemeColor::Window), captionText = themeColor(ThemeColor::Text);
  DwmSetWindowAttribute(dialog, 35, &caption, sizeof(caption));
  DwmSetWindowAttribute(dialog, 36, &captionText, sizeof(captionText));
  RECT client{};
  GetClientRect(dialog, &client);
  int pad = scale(16), buttonWidth = scale(100), buttonHeight = scale(30);
  auto create = [&](const wchar_t *type, const wchar_t *label, DWORD style, int id, int x, int y, int w, int h) {
    HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, x, y, w, h, dialog,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(picker.font), TRUE);
    SetWindowTheme(control, darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    return control;
  };
  create(L"STATIC", L"Filter branches:", SS_NOPREFIX, 0, pad, pad, client.right - 2 * pad, scale(21));
  picker.filterBorder = {pad, pad + scale(23), client.right - pad, pad + scale(51)};
  int frame = std::max(1, scale(1));
  picker.filter = create(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, filterId, pad + frame, pad + scale(23) + frame,
    client.right - 2 * pad - 2 * frame, scale(28) - 2 * frame);
  picker.list = create(L"LISTBOX", L"",
    WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, listId, pad,
    pad + scale(62), client.right - 2 * pad, client.bottom - 3 * pad - scale(62) - buttonHeight);
  SendMessageW(picker.list, LB_SETITEMHEIGHT, 0, scale(26));
  int bottom = client.bottom - pad - buttonHeight;
  create(L"BUTTON", L"Cancel", WS_TABSTOP, cancelId, client.right - pad - 2 * buttonWidth - scale(8), bottom, buttonWidth,
    buttonHeight);
  picker.choose = create(L"BUTTON", L"Choose", WS_TABSTOP | BS_DEFPUSHBUTTON, chooseId, client.right - pad - buttonWidth, bottom,
    buttonWidth, buttonHeight);
  updateList(picker);
  EnableWindow(owner, FALSE);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(picker.filter);
  MSG message{};
  while (IsWindow(dialog))
  {
    BOOL received = GetMessageW(&message, nullptr, 0, 0);
    if (received <= 0)
    {
      if (received == 0)
        PostQuitMessage(static_cast<int>(message.wParam));
      break;
    }
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
      SendMessageW(dialog, WM_COMMAND, cancelId, 0);
    else if (
      message.message == WM_KEYDOWN && message.wParam == VK_RETURN && (GetFocus() == picker.list || GetFocus() == picker.filter))
      SendMessageW(dialog, WM_COMMAND, chooseId, 0);
    else if (!IsDialogMessageW(dialog, &message))
    {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  if (IsWindow(dialog))
    DestroyWindow(dialog);
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  DeleteObject(picker.surface);
  DeleteObject(picker.background);
  return picker.result;
}
} // namespace gdv