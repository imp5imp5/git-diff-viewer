#include "CommentEditor.h"
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
constexpr int saveId = 1, cancelId = 2, deleteId = 3, textId = 4;
struct Editor
{
  HWND window{}, edit{};
  HBRUSH backgroundBrush{}, fieldBrush{};
  HFONT editFont{};
  COLORREF windowColor{}, fieldColor{}, textColor{}, borderColor{};
  CommentEditResult result;
  bool existing{}, dark{};
};
std::wstring value(HWND window)
{
  int length = GetWindowTextLengthW(window);
  std::wstring result(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(window, result.data(), length + 1);
  result.resize(length);
  return result;
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
  auto editor = reinterpret_cast<Editor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE)
  {
    editor = static_cast<Editor *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    editor->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
  }
  if (!editor)
    return DefWindowProcW(window, message, w, l);
  switch (message)
  {
    case WM_ERASEBKGND:
    {
      RECT area{};
      GetClientRect(window, &area);
      FillRect(reinterpret_cast<HDC>(w), &area, editor->backgroundBrush);
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    {
      HDC dc = reinterpret_cast<HDC>(w);
      bool field = message == WM_CTLCOLOREDIT;
      SetTextColor(dc, editor->textColor);
      SetBkColor(dc, field ? editor->fieldColor : editor->windowColor);
      return reinterpret_cast<LRESULT>(field ? editor->fieldBrush : editor->backgroundBrush);
    }
    case WM_NOTIFY:
    {
      auto header = reinterpret_cast<NMHDR *>(l);
      if (editor->dark && header->code == NM_CUSTOMDRAW &&
          (header->idFrom == saveId || header->idFrom == cancelId || header->idFrom == deleteId))
      {
        auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
          FillRect(draw->hdc, &draw->rc, editor->fieldBrush);
          HBRUSH border = CreateSolidBrush(editor->borderColor);
          FrameRect(draw->hdc, &draw->rc, border);
          DeleteObject(border);
          SetBkMode(draw->hdc, TRANSPARENT);
          SetTextColor(draw->hdc, editor->textColor);
          wchar_t label[80]{};
          GetWindowTextW(header->hwndFrom, label, 80);
          auto oldFont = SelectObject(draw->hdc, editor->editFont);
          DrawTextW(draw->hdc, label, -1, &draw->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          SelectObject(draw->hdc, oldFont);
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
      if (LOWORD(w) == saveId)
      {
        auto text = value(editor->edit);
        bool nonempty = false;
        for (wchar_t c : text)
          nonempty |= !iswspace(c);
        if (!nonempty)
        {
          MessageBeep(MB_ICONWARNING);
          SetFocus(editor->edit);
          return 0;
        }
        editor->result = {CommentEditAction::Save, std::move(text)};
        DestroyWindow(window);
        return 0;
      }
      if (LOWORD(w) == deleteId && editor->existing)
      {
        editor->result.action = CommentEditAction::Delete;
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
CommentEditResult editReviewComment(HWND owner, HINSTANCE instance, const std::wstring *existing, int fontPoints)
{
  static bool registered = false;
  if (!registered)
  {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = L"GitDiffViewer.CommentEditor";
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassW(&cls) != 0;
  }
  Editor editor;
  editor.existing = existing != nullptr;
  editor.dark = darkTheme;
  editor.windowColor = themeColor(ThemeColor::Window);
  editor.fieldColor = themeColor(ThemeColor::Surface);
  editor.textColor = themeColor(ThemeColor::Text);
  editor.borderColor = themeColor(ThemeColor::Border);
  editor.backgroundBrush = CreateSolidBrush(editor.windowColor);
  editor.fieldBrush = CreateSolidBrush(editor.fieldColor);
  int dpi = static_cast<int>(GetDpiForWindow(owner));
  LOGFONTW fontDescription{};
  GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(fontDescription), &fontDescription);
  fontDescription.lfHeight = -MulDiv(fontPoints, dpi, 72);
  editor.editFont = CreateFontIndirectW(&fontDescription);
  auto scale = [&](int n) { return MulDiv(n, dpi, 96); };
  int pad = scale(16), gap = scale(8);
  HDC dc = GetDC(owner);
  auto oldFont = SelectObject(dc, editor.editFont);
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  SIZE buttonText{};
  GetTextExtentPoint32W(dc, L"Cancel", 6, &buttonText);
  int buttonWidth = std::max<LONG>(scale(90), buttonText.cx + 2 * pad);
  int buttonHeight = std::max<LONG>(scale(30), metrics.tmHeight + gap);
  int width = std::max(scale(560), 3 * buttonWidth + 2 * gap + 2 * pad);
  constexpr auto label = L"Comment for selected After lines:";
  RECT labelRect{0, 0, width - 2 * pad, 0};
  DrawTextW(dc, label, -1, &labelRect, DT_CALCRECT | DT_WORDBREAK);
  int labelHeight = labelRect.bottom;
  SelectObject(dc, oldFont);
  ReleaseDC(owner, dc);
  RECT windowRect{0, 0, width, std::max<LONG>(scale(290), 3 * pad + labelHeight + gap + 6 * metrics.tmHeight + buttonHeight)};
  AdjustWindowRectExForDpi(&windowRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
    static_cast<UINT>(dpi));
  width = windowRect.right - windowRect.left;
  int height = windowRect.bottom - windowRect.top;
  RECT ownerRect{};
  GetWindowRect(owner, &ownerRect);
  int x = (ownerRect.left + ownerRect.right - width) / 2;
  int y = (ownerRect.top + ownerRect.bottom - height) / 2;
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"GitDiffViewer.CommentEditor",
    existing ? L"Edit comment" : L"New comment", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr, instance,
    &editor);
  if (!dialog)
  {
    if (editor.editFont)
      DeleteObject(editor.editFont);
    DeleteObject(editor.fieldBrush);
    DeleteObject(editor.backgroundBrush);
    return {};
  }
  BOOL dark = editor.dark;
  if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark))))
    DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
  COLORREF caption = editor.windowColor, captionText = editor.textColor;
  DwmSetWindowAttribute(dialog, 35, &caption, sizeof(caption));
  DwmSetWindowAttribute(dialog, 36, &captionText, sizeof(captionText));
  RECT client{};
  GetClientRect(dialog, &client);
  int bottom = client.bottom - pad - buttonHeight;
  auto create = [&](const wchar_t *type, const wchar_t *label, DWORD style, int id, int left, int top, int w, int h) {
    HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, left, top, w, h, dialog,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(editor.editFont), TRUE);
    SetWindowTheme(control, editor.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    return control;
  };
  create(L"STATIC", label, 0, 0, pad, pad, client.right - 2 * pad, labelHeight);
  int editTop = pad + labelHeight + gap;
  editor.edit = create(L"EDIT", existing ? existing->c_str() : L"",
    WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL, textId, pad, editTop, client.right - 2 * pad,
    bottom - gap - editTop);
  if (existing)
    create(L"BUTTON", L"Delete", WS_TABSTOP, deleteId, pad, bottom, buttonWidth, buttonHeight);
  create(L"BUTTON", L"Cancel", WS_TABSTOP, cancelId, client.right - pad - buttonWidth * 2 - scale(8), bottom, buttonWidth,
    buttonHeight);
  create(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, saveId, client.right - pad - buttonWidth, bottom, buttonWidth,
    buttonHeight);
  EnableWindow(owner, FALSE);
  ShowWindow(dialog, SW_SHOW);
  SetFocus(editor.edit);
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
    if (
      message.message == WM_KEYDOWN && message.wParam == VK_RETURN && (GetKeyState(VK_CONTROL) & 0x8000) && GetFocus() == editor.edit)
      SendMessageW(dialog, WM_COMMAND, saveId, 0);
    else if (
      message.message == WM_KEYDOWN && message.wParam == VK_BACK && (GetKeyState(VK_CONTROL) & 0x8000) && GetFocus() == editor.edit)
    {
      DWORD start{}, end{};
      SendMessageW(editor.edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
      if (start == end)
      {
        auto text = value(editor.edit);
        while (start && iswspace(text[start - 1]))
          --start;
        while (start && !iswspace(text[start - 1]))
          --start;
      }
      SendMessageW(editor.edit, EM_SETSEL, start, end);
      SendMessageW(editor.edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    }
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
  if (editor.editFont)
    DeleteObject(editor.editFont);
  DeleteObject(editor.fieldBrush);
  DeleteObject(editor.backgroundBrush);
  return editor.result;
}
} // namespace gdv
