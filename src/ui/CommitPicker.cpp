#include "CommitPicker.h"
#include "Theme.h"
#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
namespace gdv
{
namespace
{
constexpr int listId = 101, openId = 102, cancelId = 103;
struct Picker
{
  HWND window{}, list{};
  HBRUSH background{}, surface{}, listBackground{};
  HFONT font{};
  COLORREF text{}, selection{}, selectedText{}, border{}, listColor{};
  const std::vector<Commit> *matches{};
  std::optional<std::wstring> result;
  bool dark{};
};
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
      RECT rect{};
      GetClientRect(window, &rect);
      FillRect(reinterpret_cast<HDC>(w), &rect, picker->background);
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    {
      auto dc = reinterpret_cast<HDC>(w);
      SetTextColor(dc, picker->text);
      SetBkColor(dc, themeColor(ThemeColor::Window));
      return reinterpret_cast<LRESULT>(picker->background);
    }
    case WM_CTLCOLORLISTBOX:
    {
      auto dc = reinterpret_cast<HDC>(w);
      SetTextColor(dc, picker->text);
      SetBkColor(dc, picker->listColor);
      return reinterpret_cast<LRESULT>(picker->listBackground);
    }
    case WM_DRAWITEM:
    {
      auto item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
      if (item->CtlID != listId || item->itemID == static_cast<UINT>(-1))
        break;
      bool selected = (item->itemState & ODS_SELECTED) != 0;
      FillRect(item->hDC, &item->rcItem, picker->listBackground);
      if (selected)
      {
        auto brush = CreateSolidBrush(picker->selection);
        FillRect(item->hDC, &item->rcItem, brush);
        DeleteObject(brush);
      }
      const auto &commit = (*picker->matches)[item->itemID];
      RECT rect = item->rcItem;
      rect.left += 8;
      rect.right -= 8;
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, selected ? picker->selectedText : picker->text);
      auto label = commit.id.substr(0, 12) + L"  " + commit.subject;
      DrawTextW(item->hDC, label.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
      if (item->itemState & ODS_FOCUS)
        DrawFocusRect(item->hDC, &item->rcItem);
      return TRUE;
    }
    case WM_NOTIFY:
    {
      auto header = reinterpret_cast<NMHDR *>(l);
      if (picker->dark && header->code == NM_CUSTOMDRAW && (header->idFrom == openId || header->idFrom == cancelId))
      {
        auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
          FillRect(draw->hdc, &draw->rc, picker->surface);
          auto brush = CreateSolidBrush(picker->border);
          FrameRect(draw->hdc, &draw->rc, brush);
          DeleteObject(brush);
          SetBkMode(draw->hdc, TRANSPARENT);
          SetTextColor(draw->hdc, picker->text);
          wchar_t label[80]{};
          GetWindowTextW(header->hwndFrom, label, 80);
          DrawTextW(draw->hdc, label, -1, &draw->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
      if (LOWORD(w) == openId || (LOWORD(w) == listId && HIWORD(w) == LBN_DBLCLK))
      {
        int index = static_cast<int>(SendMessageW(picker->list, LB_GETCURSEL, 0, 0));
        if (index >= 0 && static_cast<size_t>(index) < picker->matches->size())
        {
          picker->result = (*picker->matches)[static_cast<size_t>(index)].id;
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
std::optional<std::wstring> chooseCommit(HWND owner, HINSTANCE instance, const std::wstring &prefix,
  const std::vector<Commit> &matches)
{
  static bool registered = false;
  if (!registered)
  {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = L"GitDiffViewer.CommitPicker";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    registered = RegisterClassW(&cls) != 0;
  }
  Picker picker;
  picker.matches = &matches;
  picker.dark = darkTheme;
  picker.text = themeColor(ThemeColor::Text);
  picker.selection = themeColor(ThemeColor::ListSelection);
  picker.selectedText = themeColor(ThemeColor::SelectionText);
  picker.border = themeColor(ThemeColor::Border);
  picker.background = CreateSolidBrush(themeColor(ThemeColor::Window));
  picker.surface = CreateSolidBrush(themeColor(ThemeColor::Surface));
  picker.listColor = picker.dark ? RGB(0, 0, 0) : themeColor(ThemeColor::Surface);
  picker.listBackground = CreateSolidBrush(picker.listColor);
  int dpi = static_cast<int>(GetDpiForWindow(owner));
  auto scale = [&](int value) { return MulDiv(value, dpi, 96); };
  picker.font = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  int width = scale(680), height = scale(410);
  RECT ownerRect{};
  GetWindowRect(owner, &ownerRect);
  int x = (ownerRect.left + ownerRect.right - width) / 2;
  int y = (ownerRect.top + ownerRect.bottom - height) / 2;
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"GitDiffViewer.CommitPicker", L"Choose commit",
    WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr, instance, &picker);
  if (!dialog)
  {
    DeleteObject(picker.font);
    DeleteObject(picker.surface);
    DeleteObject(picker.listBackground);
    DeleteObject(picker.background);
    return {};
  }
  BOOL dark = picker.dark;
  if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark))))
    DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
  COLORREF caption = themeColor(ThemeColor::Window), captionText = picker.text;
  DwmSetWindowAttribute(dialog, 35, &caption, sizeof(caption));
  DwmSetWindowAttribute(dialog, 36, &captionText, sizeof(captionText));
  RECT client{};
  GetClientRect(dialog, &client);
  int pad = scale(16), buttonWidth = scale(100), buttonHeight = scale(30);
  auto create = [&](const wchar_t *type, const wchar_t *label, DWORD style, int id, int left, int top, int w, int h) {
    auto control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, left, top, w, h, dialog,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(picker.font), TRUE);
    SetWindowTheme(control, picker.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    return control;
  };
  auto prompt = matches.empty() ? L"No commits match " + prefix + L"."
                                : std::to_wstring(matches.size()) + L" commits match " + prefix + L". Choose one:";
  create(L"STATIC", prompt.c_str(), SS_NOPREFIX, 0, pad, pad, client.right - 2 * pad, scale(25));
  picker.list = create(L"LISTBOX", L"",
    WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, listId, pad,
    pad + scale(30), client.right - 2 * pad, client.bottom - 3 * pad - scale(30) - buttonHeight);
  SendMessageW(picker.list, LB_SETITEMHEIGHT, 0, scale(26));
  for (const auto &commit : matches)
  {
    auto label = commit.id.substr(0, 12) + L"  " + commit.subject;
    SendMessageW(picker.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  }
  if (!matches.empty())
    SendMessageW(picker.list, LB_SETCURSEL, 0, 0);
  int bottom = client.bottom - pad - buttonHeight;
  create(L"BUTTON", L"Cancel", WS_TABSTOP, cancelId, client.right - pad - 2 * buttonWidth - scale(8), bottom, buttonWidth,
    buttonHeight);
  auto open = create(L"BUTTON", matches.empty() ? L"Close" : L"Open", WS_TABSTOP | BS_DEFPUSHBUTTON, openId,
    client.right - pad - buttonWidth, bottom, buttonWidth, buttonHeight);
  if (matches.empty())
    EnableWindow(open, FALSE);
  EnableWindow(owner, FALSE);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(matches.empty() ? GetDlgItem(dialog, cancelId) : picker.list);
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
    if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && GetFocus() == picker.list)
      SendMessageW(dialog, WM_COMMAND, openId, 0);
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
  DeleteObject(picker.font);
  DeleteObject(picker.surface);
  DeleteObject(picker.listBackground);
  DeleteObject(picker.background);
  return picker.result;
}
} // namespace gdv
