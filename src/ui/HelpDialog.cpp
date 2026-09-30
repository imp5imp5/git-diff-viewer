#include "HelpDialog.h"
#include "Theme.h"
#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
namespace gdv
{
namespace
{
constexpr int listId = 100, aboutId = 101, githubId = 102;
struct Shortcut
{
  const wchar_t *keys, *description;
};
constexpr Shortcut shortcuts[] = {{L"Search", nullptr}, {L"Ctrl+D", L"Search repository commits"},
  {L"Ctrl+F / F7", L"Find text in the current diff"}, {L"F3 / Shift+F7", L"Next match"}, {L"Shift+F3", L"Previous match"},
  {L"Esc", L"Clear diff search or close a dialog"}, {L"View", nullptr}, {L"F5 / Ctrl+R", L"Refresh Git data"},
  {L"Ctrl+Shift+D", L"Toggle unified / side-by-side diff"}, {L"F", L"Toggle full file (outside text fields and dropdowns)"},
  {L"Ctrl+- / Ctrl+=", L"Change diff font size"}, {L"Navigation", nullptr}, {L"Ctrl+Up / Ctrl+Down", L"Previous / next list item"},
  {L"Ctrl+PgUp / Ctrl+PgDn", L"Previous / next change or comment"},
  {L"Space", L"Toggle commit message (diff, files, closed commit dropdown)"},
  {L"Enter / Space", L"Load more when the paging item is focused"}, {L"Arrows / PgUp / PgDn / Home / End", L"Navigate the diff"},
  {L"Selection and comments", nullptr}, {L"Ctrl+A", L"Select all in the diff or commit message"},
  {L"Ctrl+C", L"Copy selected diff rows"}, {L"C", L"Add or edit a comment for selected After lines"},
  {L"Ctrl+K", L"Toggle the current commit's comments view"}, {L"F2", L"Copy all review comments"},
  {L"Ctrl+Enter", L"Save in the comment editor"}, {L"Ctrl+Backspace", L"Delete the previous word in the comment editor"},
  {L"Other", nullptr}, {L"Ctrl+Shift+S", L"Save an application screenshot"}, {L"F1", L"Open this help"}};
struct Help
{
  HWND window{}, title{}, list{}, about{}, info{}, github{}, close{};
  HFONT font{}, bold{};
  HBRUSH background{}, surface{};
  int dpi{}, lineHeight{}, keysWidth{}, descriptionWidth{}, separator{};
  bool expanded{};
  int scale(int n) const { return MulDiv(n, dpi, 96); }
};
void layout(Help &help)
{
  if (!help.list)
    return;
  RECT client{};
  GetClientRect(help.window, &client);
  int pad = help.scale(16), gap = help.scale(8), buttonHeight = help.lineHeight + gap;
  int footer = buttonHeight + 2 * pad + (help.expanded ? 3 * help.lineHeight + gap : 0);
  help.separator = client.bottom - footer;
  MoveWindow(help.title, pad, pad, client.right - 2 * pad, help.lineHeight + gap, TRUE);
  int top = pad + help.lineHeight + 2 * gap;
  MoveWindow(help.list, pad, top, client.right - 2 * pad, std::max(1, help.separator - gap - top), TRUE);
  int bottom = client.bottom - pad - buttonHeight;
  MoveWindow(help.about, pad, bottom, help.scale(230), buttonHeight, TRUE);
  MoveWindow(help.close, client.right - pad - help.scale(90), bottom, help.scale(90), buttonHeight, TRUE);
  MoveWindow(help.info, pad, help.separator + gap, client.right - 2 * pad - help.scale(100), 3 * help.lineHeight, TRUE);
  MoveWindow(help.github, client.right - pad - help.scale(90), help.separator + gap, help.scale(90), buttonHeight, TRUE);
  RECT list{};
  GetClientRect(help.list, &list);
  int keyWidth = std::min(help.keysWidth, static_cast<int>(list.right) / 2);
  HDC dc = GetDC(help.list);
  auto oldFont = SelectObject(dc, help.font);
  for (size_t i = 0; i < std::size(shortcuts); ++i)
  {
    const auto &row = shortcuts[i];
    int height = help.lineHeight + gap;
    if (row.description)
    {
      RECT key{0, 0, keyWidth, 0}, description{0, 0, std::max(1, static_cast<int>(list.right) - keyWidth - 4 * gap), 0};
      DrawTextW(dc, row.keys, -1, &key, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
      DrawTextW(dc, row.description, -1, &description, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
      height = std::max(key.bottom, description.bottom) + help.scale(4);
    }
    SendMessageW(help.list, LB_SETITEMHEIGHT, i, height);
  }
  SelectObject(dc, oldFont);
  ReleaseDC(help.list, dc);
  InvalidateRect(help.list, nullptr, TRUE);
  InvalidateRect(help.window, nullptr, TRUE);
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
  auto help = reinterpret_cast<Help *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE)
  {
    help = static_cast<Help *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    help->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(help));
  }
  if (!help)
    return DefWindowProcW(window, message, w, l);
  switch (message)
  {
    case WM_ERASEBKGND:
    {
      RECT rect{};
      GetClientRect(window, &rect);
      FillRect(reinterpret_cast<HDC>(w), &rect, help->background);
      return 1;
    }
    case WM_PAINT:
    {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      RECT client{};
      GetClientRect(window, &client);
      RECT line{help->scale(16), help->separator, client.right - help->scale(16), help->separator + help->scale(1)};
      auto brush = CreateSolidBrush(themeColor(ThemeColor::Border));
      FillRect(dc, &line, brush);
      DeleteObject(brush);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
      SetTextColor(reinterpret_cast<HDC>(w), themeColor(ThemeColor::Text));
      SetBkColor(reinterpret_cast<HDC>(w), themeColor(ThemeColor::Window));
      return reinterpret_cast<LRESULT>(help->background);
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT *>(l)->itemHeight = help->lineHeight + help->scale(4); return TRUE;
    case WM_DRAWITEM:
    {
      auto item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
      if (item->CtlID != listId || item->itemID >= std::size(shortcuts))
        break;
      int saved = SaveDC(item->hDC);
      FillRect(item->hDC, &item->rcItem, help->background);
      SetBkMode(item->hDC, TRANSPARENT);
      const auto &row = shortcuts[item->itemID];
      SelectObject(item->hDC, row.description ? help->font : help->bold);
      SetTextColor(item->hDC, themeColor(row.description ? ThemeColor::Text : ThemeColor::Title));
      RECT rect = item->rcItem;
      int gap = help->scale(8);
      rect.top += help->scale(2);
      if (!row.description)
      {
        rect.top += help->scale(4);
        DrawTextW(item->hDC, row.keys, -1, &rect, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
      }
      else
      {
        RECT key = rect;
        key.right = key.left + std::min(help->keysWidth, static_cast<int>(rect.right - rect.left) / 2);
        DrawTextW(item->hDC, row.keys, -1, &key, DT_RIGHT | DT_WORDBREAK | DT_NOPREFIX);
        RECT dash{key.right + gap, rect.top, key.right + 3 * gap, rect.bottom};
        DrawTextW(item->hDC, L"—", -1, &dash, DT_CENTER | DT_SINGLELINE);
        rect.left = dash.right + gap;
        DrawTextW(item->hDC, row.description, -1, &rect, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
      }
      RestoreDC(item->hDC, saved);
      return TRUE;
    }
    case WM_NOTIFY:
    {
      auto header = reinterpret_cast<NMHDR *>(l);
      if (darkTheme && header->code == NM_CUSTOMDRAW)
      {
        auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
          FillRect(draw->hdc, &draw->rc, help->surface);
          auto border = CreateSolidBrush(themeColor(ThemeColor::Border));
          FrameRect(draw->hdc, &draw->rc, border);
          DeleteObject(border);
          auto oldFont = SelectObject(draw->hdc, help->font);
          SetBkMode(draw->hdc, TRANSPARENT);
          SetTextColor(draw->hdc, themeColor(ThemeColor::Text));
          wchar_t label[80]{};
          GetWindowTextW(header->hwndFrom, label, 80);
          DrawTextW(draw->hdc, label, -1, &draw->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          if (draw->uItemState & CDIS_FOCUS)
          {
            RECT focus = draw->rc;
            InflateRect(&focus, -3, -3);
            DrawFocusRect(draw->hdc, &focus);
          }
          SelectObject(draw->hdc, oldFont);
          return CDRF_SKIPDEFAULT;
        }
      }
      break;
    }
    case WM_SIZE: layout(*help); return 0;
    case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO *>(l)->ptMinTrackSize = {help->scale(620), help->scale(400)}; return 0;
    case WM_COMMAND:
      switch (LOWORD(w))
      {
        case IDOK:
        case IDCANCEL: DestroyWindow(window); return 0;
        case aboutId:
          help->expanded = !help->expanded;
          SetWindowTextW(help->about, help->expanded ? L"Hide program information" : L"About GitDiffViewer");
          ShowWindow(help->info, help->expanded ? SW_SHOW : SW_HIDE);
          ShowWindow(help->github, help->expanded ? SW_SHOW : SW_HIDE);
          layout(*help);
          return 0;
        case githubId:
          ShellExecuteW(window, L"open", L"https://github.com/imp5imp5/git-diff-viewer", nullptr, nullptr, SW_SHOWNORMAL);
          return 0;
      }
      break;
    case WM_CLOSE: DestroyWindow(window); return 0;
  }
  return DefWindowProcW(window, message, w, l);
}
} // namespace
void showHelp(HWND owner, HINSTANCE instance, HFONT font)
{
  static bool registered = false;
  if (!registered)
  {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = L"GitDiffViewer.Help";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassW(&cls) != 0;
  }
  Help help;
  help.dpi = static_cast<int>(GetDpiForWindow(owner));
  LOGFONTW description{};
  GetObjectW(font, sizeof(description), &description);
  help.font = CreateFontIndirectW(&description);
  description.lfWeight = FW_BOLD;
  help.bold = CreateFontIndirectW(&description);
  help.background = CreateSolidBrush(themeColor(ThemeColor::Window));
  help.surface = CreateSolidBrush(themeColor(ThemeColor::Surface));
  HDC dc = GetDC(owner);
  auto oldFont = SelectObject(dc, help.font);
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  help.lineHeight = metrics.tmHeight;
  for (const auto &row : shortcuts)
    if (row.description)
    {
      SIZE key{}, text{};
      GetTextExtentPoint32W(dc, row.keys, lstrlenW(row.keys), &key);
      GetTextExtentPoint32W(dc, row.description, lstrlenW(row.description), &text);
      help.keysWidth = std::max<LONG>(help.keysWidth, key.cx);
      help.descriptionWidth = std::max<LONG>(help.descriptionWidth, text.cx);
    }
  SelectObject(dc, oldFont);
  ReleaseDC(owner, dc);
  RECT ownerRect{};
  GetWindowRect(owner, &ownerRect);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
  int width = std::min<LONG>(help.keysWidth + help.descriptionWidth + help.scale(100), monitor.rcWork.right - monitor.rcWork.left);
  int height = std::min<LONG>(static_cast<int>(std::size(shortcuts)) * (help.lineHeight + help.scale(4)) + help.scale(170),
    monitor.rcWork.bottom - monitor.rcWork.top);
  int x = std::clamp<LONG>((ownerRect.left + ownerRect.right - width) / 2, monitor.rcWork.left, monitor.rcWork.right - width);
  int y = std::clamp<LONG>((ownerRect.top + ownerRect.bottom - height) / 2, monitor.rcWork.top, monitor.rcWork.bottom - height);
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"GitDiffViewer.Help", L"GitDiffViewer Help",
    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, width, height, owner, nullptr, instance, &help);
  if (dialog)
  {
    BOOL dark = darkTheme;
    if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark))))
      DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
    auto create = [&](const wchar_t *type, const wchar_t *label, DWORD style, int id) {
      HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(help.font), TRUE);
      SetWindowTheme(control, darkTheme ? L"DarkMode_Explorer" : L"Explorer", nullptr);
      return control;
    };
    help.title = create(L"STATIC", L"Keyboard shortcuts", SS_NOPREFIX, 0);
    SendMessageW(help.title, WM_SETFONT, reinterpret_cast<WPARAM>(help.bold), TRUE);
    help.list = create(L"LISTBOX", L"",
      WS_TABSTOP | WS_VSCROLL | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS | LBS_NOSEL | LBS_NOINTEGRALHEIGHT, listId);
    for (const auto &row : shortcuts)
      SendMessageW(help.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.keys));
    help.about = create(L"BUTTON", L"About GitDiffViewer", WS_TABSTOP, aboutId);
    help.info =
      create(L"STATIC", L"GitDiffViewer — Version 7\nAuthor: Aleksei Borisov · 2026\nLicensed under the MIT License", SS_NOPREFIX, 0);
    help.github = create(L"BUTTON", L"GitHub", WS_TABSTOP, githubId);
    help.close = create(L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    ShowWindow(help.info, SW_HIDE);
    ShowWindow(help.github, SW_HIDE);
    layout(help);
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(help.close);
    MSG message{};
    while (IsWindow(dialog))
    {
      BOOL received = GetMessageW(&message, nullptr, 0, 0);
      if (received <= 0)
      {
        if (!received)
          PostQuitMessage(static_cast<int>(message.wParam));
        break;
      }
      if (!IsDialogMessageW(dialog, &message))
      {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    if (IsWindow(dialog))
      DestroyWindow(dialog);
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
  }
  DeleteObject(help.font);
  DeleteObject(help.bold);
  DeleteObject(help.background);
  DeleteObject(help.surface);
}
} // namespace gdv
