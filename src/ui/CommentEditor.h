#pragma once
#include <string>
#include <windows.h>
namespace gdv
{
enum class CommentEditAction
{
  Cancel,
  Save,
  Delete
};
struct CommentEditResult
{
  CommentEditAction action{CommentEditAction::Cancel};
  std::wstring text;
};
CommentEditResult editReviewComment(HWND owner, HINSTANCE instance, const std::wstring *existing);
} // namespace gdv
