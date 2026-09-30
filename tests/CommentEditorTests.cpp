#include "ui/CommentEditor.h"
#include "ui/Theme.h"
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <windows.h>
using namespace gdv;
namespace
{
void check(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}
HWND waitForEditor()
{
  for (int attempt = 0; attempt < 200; ++attempt)
  {
    HWND editor = FindWindowW(L"GitDiffViewer.CommentEditor", nullptr);
    if (editor)
      return editor;
    Sleep(10);
  }
  return nullptr;
}
HWND button(HWND editor, const wchar_t *name)
{
  for (HWND child = FindWindowExW(editor, nullptr, L"BUTTON", nullptr); child;
       child = FindWindowExW(editor, child, L"BUTTON", nullptr))
  {
    wchar_t text[80]{};
    GetWindowTextW(child, text, 80);
    if (wcscmp(text, name) == 0)
      return child;
  }
  return nullptr;
}
CommentEditResult run(const std::wstring *existing, const wchar_t *action, const wchar_t *text = nullptr, int fontPoints = 11)
{
  CommentEditResult result;
  std::atomic<DWORD> workerId{};
  std::thread worker([&] {
    workerId = GetCurrentThreadId();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND owner =
      CreateWindowExW(0, L"STATIC", L"Comment owner", WS_OVERLAPPEDWINDOW, 100, 100, 700, 500, nullptr, nullptr, instance, nullptr);
    result = editReviewComment(owner, instance, existing, fontPoints);
    DestroyWindow(owner);
  });
  HWND editor = waitForEditor();
  if (!editor)
  {
    if (workerId)
      PostThreadMessageW(workerId, WM_QUIT, 0, 0);
    worker.join();
    throw std::runtime_error("comment dialog did not open");
  }
  HWND edit = FindWindowExW(editor, nullptr, L"EDIT", nullptr);
  const char *validationError = nullptr;
  if (!edit)
    validationError = "comment text field";
  else
  {
    LOGFONTW actual{};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0)), sizeof(actual), &actual);
    if (actual.lfHeight != -MulDiv(fontPoints, static_cast<int>(GetDpiForWindow(editor)), 72))
      validationError = "comment edit font matches the parent diff size";
    for (HWND control = GetWindow(editor, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT))
    {
      if (SendMessageW(control, WM_GETFONT, 0, 0) != SendMessageW(edit, WM_GETFONT, 0, 0))
        validationError = "comment controls use the same font size";
      RECT bounds{}, client{};
      GetWindowRect(control, &bounds);
      MapWindowPoints(nullptr, editor, reinterpret_cast<POINT *>(&bounds), 2);
      GetClientRect(editor, &client);
      if (bounds.left < 0 || bounds.top < 0 || bounds.right > client.right || bounds.bottom > client.bottom ||
          bounds.bottom <= bounds.top)
        validationError = "comment controls fit inside the dialog";
    }
    HDC dc = GetDC(edit);
    SendMessageW(editor, WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(edit));
    if (GetBkColor(dc) != themeColor(ThemeColor::Surface) || GetTextColor(dc) != themeColor(ThemeColor::Text))
      validationError = "comment editor follows the selected theme";
    ReleaseDC(edit, dc);
  }
  if (validationError)
  {
    SendMessageW(editor, WM_CLOSE, 0, 0);
    worker.join();
    throw std::runtime_error(validationError);
  }
  if (text)
    SetWindowTextW(edit, text);
  HWND target = button(editor, action);
  if (!target)
  {
    SendMessageW(editor, WM_CLOSE, 0, 0);
    worker.join();
    throw std::runtime_error("dialog action button");
  }
  SendMessageW(target, BM_CLICK, 0, 0);
  worker.join();
  return result;
}
} // namespace
int main()
try
{
  darkTheme = true;
  auto created = run(nullptr, L"Save", L"Review line", 18);
  check(created.action == CommentEditAction::Save && created.text == L"Review line", "create comment");
  darkTheme = false;
  auto edited = run(&created.text, L"Save", L"Updated review", 24);
  check(edited.action == CommentEditAction::Save && edited.text == L"Updated review", "edit comment");
  auto removed = run(&edited.text, L"Delete", nullptr, 40);
  check(removed.action == CommentEditAction::Delete, "delete comment");
  auto cancelled = run(nullptr, L"Cancel");
  check(cancelled.action == CommentEditAction::Cancel, "cancel comment");
  return 0;
}
catch (const std::exception &e)
{
  OutputDebugStringA(e.what());
  return 1;
}
