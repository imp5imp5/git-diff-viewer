#include "MainWindow.h"
#include "Screenshot.h"
#include "Theme.h"
#include "diff/UnifiedDiffParser.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
namespace gdv
{
namespace
{
std::string json(const std::wstring &value)
{
  std::string out = "\"";
  const char hex[] = "0123456789abcdef";
  for (unsigned char c : toUtf8(value))
  {
    if (c == '"' || c == '\\')
    {
      out += '\\';
      out += static_cast<char>(c);
    }
    else if (c < 32)
    {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 15];
    }
    else
      out += static_cast<char>(c);
  }
  return out + '"';
}
std::wstring label(HWND window)
{
  int n = GetWindowTextLengthW(window);
  std::wstring value(static_cast<size_t>(n) + 1, L'\0');
  GetWindowTextW(window, value.data(), n + 1);
  value.resize(n);
  return value;
}
int integer(const std::wstring &text, int minimum, int maximum)
{
  size_t used = 0;
  int n = std::stoi(text, &used);
  if (used != text.size() || n < minimum || n > maximum)
    throw std::runtime_error("Numeric argument out of range.");
  return n;
}
} // namespace
std::string MainWindow::stateJson() const
{
  RECT r{};
  GetClientRect(hwnd_, &r);
  std::string out = "{\"loading\":" + std::string(loading_ ? "true" : "false") + ",\"repository\":" + json(directory_) +
                    ",\"statusAnimating\":" + (statusAnimationActive_ ? "true" : "false") + ",\"branch\":" + json(snapshot_.branch) +
                    ",\"previewCommit\":" + std::to_string(previewIndex_) +
                    ",\"source\":" + std::to_string(SendMessageW(source_, CB_GETCURSEL, 0, 0)) +
                    ",\"view\":" + json(side_ ? L"side-by-side" : L"unified") +
                    ",\"layout\":" + json(explorerLayout_ ? L"panels" : L"classic") +
                    ",\"fullFile\":" + (fullFile_ ? "true" : "false") + ",\"plainText\":" + (diff_.plainText() ? "true" : "false") +
                    ",\"darkTheme\":" + (darkTheme ? "true" : "false") + ",\"selectedFile\":" + json(selectedPath_) +
                    ",\"selectedListKey\":" + json(selectedListKey_) +
                    ",\"fileListTop\":" + std::to_string(SendMessageW(files_, LB_GETTOPINDEX, 0, 0)) +
                    ",\"explorerGroupSelection\":" + std::to_string(SendMessageW(explorerCommits_, LB_GETCURSEL, 0, 0)) +
                    ",\"explorerFileSelection\":" + std::to_string(SendMessageW(explorerFiles_, LB_GETCURSEL, 0, 0)) +
                    ",\"explorerGroupTop\":" + std::to_string(SendMessageW(explorerCommits_, LB_GETTOPINDEX, 0, 0)) +
                    ",\"explorerFileTop\":" + std::to_string(SendMessageW(explorerFiles_, LB_GETTOPINDEX, 0, 0)) +
                    ",\"explorerCommitThumbTop\":" + std::to_string(explorerThumb(0).top) +
                    ",\"explorerFileThumbTop\":" + std::to_string(explorerThumb(1).top) +
                    ",\"explorerTooltip\":" + json(tooltipOwner_ == explorerFiles_ ? tooltipText_ : L"") +
                    ",\"explorerMessageSelectionEnd\":" + std::to_string(HIWORD(SendMessageW(explorerMessage_, EM_GETSEL, 0, 0))) +
                    ",\"topRow\":" + std::to_string(diff_.topRow()) + ",\"rowCount\":" + std::to_string(diff_.rowCount()) +
                    ",\"visibleRowCount\":" + std::to_string(diff_.visibleRowCount()) +
                    ",\"activeChangeStart\":" + std::to_string(diff_.activeChangeStart()) +
                    ",\"activeChangeEnd\":" + std::to_string(diff_.activeChangeEnd()) +
                    ",\"changeFlashing\":" + (diff_.changeFlashing() ? "true" : "false") +
                    ",\"minimapMarkers\":" + std::to_string(diff_.minimapMarkerCount()) +
                    ",\"scrollbarHover\":" + std::string(diff_.scrollbarHovered() ? "true" : "false") +
                    ",\"scrollbarDragging\":" + std::string(diff_.scrollbarDragging() ? "true" : "false") +
                    ",\"fontSize\":" + std::to_string(diff_.fontSize()) + ",\"status\":" + json(label(status_)) +
                    ",\"width\":" + std::to_string(r.right) + ",\"height\":" + std::to_string(r.bottom) + ",\"files\":[";
  bool first = true;
  if (!snapshot_.commitId.empty())
  {
    out += "{\"path\":\"<<Commit Message>>\",\"status\":\"message\"}";
    first = false;
  }
  for (const auto &f : snapshot_.document.files)
  {
    if (!first)
      out += ',';
    first = false;
    out += "{\"path\":" + json(f.path()) + ",\"status\":" + json(std::wstring(1, statusLetter(f.status))) + "}";
  }
  out += "],\"fileList\":[";
  first = true;
  auto kind = [](FileListItemKind value) {
    switch (value)
    {
      case FileListItemKind::File: return L"file";
      case FileListItemKind::CommitMessage: return L"message";
      case FileListItemKind::Commit: return L"commit";
      case FileListItemKind::Spacer: return L"spacer";
      case FileListItemKind::Summary: return L"summary";
      case FileListItemKind::Section: return L"section";
      case FileListItemKind::Notice: return L"notice";
      case FileListItemKind::LoadMore: return L"load-more";
    }
    return L"unknown";
  };
  for (const auto &item : fileListItems_)
  {
    if (!first)
      out += ',';
    first = false;
    out += "{\"kind\":" + json(kind(item.kind)) + ",\"label\":" + json(item.label) + ",\"key\":" + json(item.key) +
           ",\"path\":" + json(item.file ? item.file->path() : L"") + "}";
  }
  out += "],\"explorerGroups\":[";
  first = true;
  for (const auto &item : explorerGroups_)
  {
    if (!first)
      out += ',';
    first = false;
    out += "{\"label\":" + json(item.label) + ",\"key\":" + json(item.key) + ",\"kind\":" + json(kind(item.kind)) + "}";
  }
  out += "],\"explorerFiles\":[";
  first = true;
  for (const auto &item : explorerFileItems_)
  {
    if (!first)
      out += ',';
    first = false;
    out += "{\"path\":" + json(item.file ? item.file->path() : L"") + ",\"key\":" + json(item.key) + "}";
  }
  out += "],\"commits\":[";
  first = true;
  for (const auto &c : series_)
  {
    if (!first)
      out += ',';
    first = false;
    out += "{\"id\":" + json(c.id) + ",\"subject\":" + json(c.subject) + "}";
  }
  out += "],\"controls\":[";
  first = true;
  const std::pair<const wchar_t *, HWND> controls[] = {{L"refresh", refresh_}, {L"source", source_}, {L"view", view_},
    {L"full-file", fullFileButton_}, {L"theme", themeButton_}, {L"info", info_}, {L"base", base_}, {L"target", target_},
    {L"compare", compare_}, {L"files", files_}, {L"commits", commits_}, {L"diff", diff_.handle()}, {L"status", status_},
    {L"layout", layoutButton_}, {L"explorer-commits", explorerCommits_}, {L"explorer-message", explorerMessage_},
    {L"explorer-files", explorerFiles_}, {L"explorer-commits-scroll", explorerBars_[0]}, {L"explorer-files-scroll", explorerBars_[1]},
    {L"explorer-message-scroll", explorerBars_[2]}};
  for (const auto &c : controls)
  {
    RECT bounds{};
    GetWindowRect(c.second, &bounds);
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT *>(&bounds), 2);
    if (!first)
      out += ',';
    first = false;
    out += "{\"id\":" + json(c.first) + ",\"text\":" + json(label(c.second)) +
           ",\"visible\":" + ((GetWindowLongPtrW(c.second, GWL_STYLE) & WS_VISIBLE) ? "true" : "false") +
           ",\"focused\":" + (GetFocus() == c.second ? "true" : "false") + ",\"x\":" + std::to_string(bounds.left) +
           ",\"y\":" + std::to_string(bounds.top) + ",\"width\":" + std::to_string(bounds.right - bounds.left) +
           ",\"height\":" + std::to_string(bounds.bottom - bounds.top) + "}";
  }
  out += "],\"explorerSplitters\":[";
  for (int i = 0; i < 3; ++i)
  {
    if (i)
      out += ',';
    const auto &rect = explorerSplitters_[i];
    out += "{\"x\":" + std::to_string(rect.left) + ",\"y\":" + std::to_string(rect.top) +
           ",\"width\":" + std::to_string(rect.right - rect.left) + ",\"height\":" + std::to_string(rect.bottom - rect.top) + "}";
  }
  out +=
    "],\"explorerMessageText\":" + json(label(explorerMessage_)) + ",\"message\":" + json(diff_.messageText()) + ",\"visibleRows\":[";
  first = true;
  const auto *file = diff_.file();
  const auto &rows = diff_.presentation();
  for (int i = diff_.topRow();
       file && i < diff_.topRow() + std::min(100, diff_.visibleRowCount()) && i < static_cast<int>(rows.size()); ++i)
  {
    const auto &row = rows[static_cast<size_t>(i)];
    auto cell = [&](size_t index) {
      if (row.hunk == noLine || index == noLine)
        return std::string("null");
      const auto &line = file->hunks[row.hunk].lines[index];
      return "{\"text\":" + json(line.text) + ",\"oldLine\":" + (line.oldLine ? std::to_string(*line.oldLine) : "null") +
             ",\"newLine\":" + (line.newLine ? std::to_string(*line.newLine) : "null") +
             ",\"type\":" + std::to_string(static_cast<int>(line.type)) + "}";
    };
    if (!first)
      out += ',';
    first = false;
    out += "{\"row\":" + std::to_string(i) + ",\"meta\":" + json(row.meta) + ",\"left\":" + cell(row.left) +
           ",\"right\":" + cell(row.right) + "}";
  }
  return out + "]}";
}
void MainWindow::automationTick()
{
  namespace fs = std::filesystem;
  if (automationDirectory_.empty())
    return;
  auto root = fs::path(automationDirectory_), request = root / L"request.txt";
  if (!pendingResponse_.empty())
  {
    publishAutomationResponse();
    return;
  }
  std::wstring id;
  std::string error;
  bool close = false;
  try
  {
    if (!fs::exists(request))
      return;
    if (fs::file_size(request) > 65536)
      throw std::runtime_error("Request exceeds 64 KiB.");
    std::ifstream file(request, std::ios::binary);
    if (!file)
      return;
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    file.close();
    std::istringstream input(bytes);
    std::string line;
    std::vector<std::wstring> fields;
    while (std::getline(input, line))
    {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      fields.push_back(fromUtf8(line));
    }
    if (fields.size() < 2)
      throw std::runtime_error("Expected request ID and command on separate lines.");
    id = fields[0];
    const auto &command = fields[1];
    auto arg = [&](size_t n) -> const std::wstring & {
      if (n + 2 >= fields.size())
        throw std::runtime_error("Missing command argument.");
      return fields[n + 2];
    };
    if (command == L"state") {}
    else if (command == L"navigate-file")
      navigateList(integer(arg(0), -1, 1));
    else if (command == L"list-key")
    {
      auto value = arg(0);
      if (value != L"up" && value != L"down" && value != L"enter" && value != L"space")
        throw std::runtime_error("List key must be up, down, enter, or space.");
      SetFocus(files_);
      SendMessageW(files_, WM_KEYDOWN,
        value == L"up"      ? VK_UP
        : value == L"down"  ? VK_DOWN
        : value == L"enter" ? VK_RETURN
                            : VK_SPACE,
        0);
    }
    else if (command == L"toggle-message")
      toggleCommitMessage();
    else if (command == L"theme")
    {
      auto value = arg(0);
      if (value != L"dark" && value != L"light")
        throw std::runtime_error("Theme must be dark or light.");
      darkTheme = value == L"dark";
      applyTheme();
    }
    else if (command == L"open")
    {
      directory_ = arg(0);
      selectedPath_.clear();
      SetWindowTextW(base_, L"");
      sourceChanged();
      refresh();
    }
    else if (command == L"source")
    {
      const std::vector<std::wstring> names = {L"staged", L"unstaged", L"head", L"ready", L"commit", L"range", L"history"};
      auto found = std::find(names.begin(), names.end(), arg(0));
      if (found == names.end())
        throw std::runtime_error("Unknown source.");
      SendMessageW(source_, CB_SETCURSEL, found - names.begin(), 0);
      sourceChanged();
      refresh();
    }
    else if (command == L"view")
    {
      auto value = arg(0);
      if (value != L"unified" && value != L"side-by-side")
        throw std::runtime_error("Unknown view.");
      if (side_ != (value == L"side-by-side"))
        toggle();
    }
    else if (command == L"layout")
    {
      auto value = arg(0);
      if (value != L"classic" && value != L"panels")
        throw std::runtime_error("Layout must be classic or panels.");
      setExplorerLayout(value == L"panels");
    }
    else if (command == L"select-explorer-group")
    {
      int index = integer(arg(0), 0, static_cast<int>(explorerGroups_.size()) - 1);
      SendMessageW(explorerCommits_, LB_SETCURSEL, index, 0);
      selectExplorerGroup(index);
    }
    else if (command == L"select-explorer-file")
    {
      int index = integer(arg(0), 0, static_cast<int>(explorerFileItems_.size()) - 1);
      SendMessageW(explorerFiles_, LB_SETCURSEL, index, 0);
      selectExplorerFile(index);
    }
    else if (command == L"explorer-splitter")
    {
      int index = integer(arg(0), 0, 2);
      moveExplorerSplitter(index, integer(arg(1), 0, 4096));
    }
    else if (command == L"full-file")
    {
      auto value = arg(0);
      if (value != L"on" && value != L"off")
        throw std::runtime_error("Full-file mode must be on or off.");
      if (fullFile_ != (value == L"on"))
        SendMessageW(fullFileButton_, BM_CLICK, 0, 0);
    }
    else if (command == L"base" || command == L"target")
    {
      SetWindowTextW(command == L"base" ? base_ : target_, arg(0).c_str());
      if (command == L"base")
      {
        if (static_cast<ChangeSource>(SendMessageW(source_, CB_GETCURSEL, 0, 0)) == ChangeSource::ReadyToPush)
          readyBase_ = arg(0);
        else
          rangeBase_ = arg(0);
      }
    }
    else if (command == L"refresh" || command == L"compare")
      refresh();
    else if (command == L"load-more")
      loadMoreHistory();
    else if (command == L"load-more-input")
    {
      auto value = arg(0);
      if (value != L"mouse" && value != L"enter" && value != L"space")
        throw std::runtime_error("Load more input must be mouse, enter or space.");
      auto found = std::find_if(fileListItems_.begin(), fileListItems_.end(),
        [](const auto &item) { return item.kind == FileListItemKind::LoadMore; });
      if (found == fileListItems_.end())
        throw std::runtime_error("Load more is not available.");
      int index = static_cast<int>(found - fileListItems_.begin());
      SendMessageW(files_, LB_SETCURSEL, index, 0);
      SendMessageW(files_, LB_SETTOPINDEX, std::max(0, index - 1), 0);
      SetFocus(files_);
      if (value == L"enter" || value == L"space")
        SendMessageW(files_, WM_KEYDOWN, value == L"enter" ? VK_RETURN : VK_SPACE, 0);
      else if (value == L"mouse")
      {
        RECT row{};
        SendMessageW(files_, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row));
        auto point = MAKELPARAM((row.left + row.right) / 2, (row.top + row.bottom) / 2);
        SendMessageW(files_, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(files_, WM_LBUTTONUP, 0, point);
      }
    }
    else if (command == L"select-file")
    {
      if (loading_)
        throw std::runtime_error("Wait until loading is false before selecting a file.");
      int found = -1;
      for (size_t i = 0; i < fileListItems_.size(); ++i)
      {
        const auto &item = fileListItems_[i];
        if (arg(0) == L"<<Commit Message>>" && item.kind == FileListItemKind::CommitMessage)
          found = static_cast<int>(i);
        if (item.file && item.file->path() == arg(0))
        {
          if (found < 0)
            found = static_cast<int>(i);
          if (item.key.rfind(L"summary\n", 0) == 0)
          {
            found = static_cast<int>(i);
            break;
          }
        }
      }
      if (found < 0)
        throw std::runtime_error("File is not in the current comparison.");
      SendMessageW(files_, LB_SETCURSEL, found, 0);
      selectFile();
      if (explorerLayout_)
        rebuildExplorer();
    }
    else if (command == L"select-list-item")
    {
      if (loading_)
        throw std::runtime_error("Wait until loading is false before selecting a list item.");
      int index = integer(arg(0), 0, static_cast<int>(fileListItems_.size()) - 1);
      SendMessageW(files_, LB_SETCURSEL, index, 0);
      selectFile();
      if (explorerLayout_)
        rebuildExplorer();
    }
    else if (command == L"hover-commit")
    {
      if (loading_ || SendMessageW(source_, CB_GETCURSEL, 0, 0) != 3)
        throw std::runtime_error("Ready to push must finish loading first.");
      int index = integer(arg(0), 0, static_cast<int>(series_.size()));
      automationHover_ = true;
      SendMessageW(commits_, CB_SHOWDROPDOWN, TRUE, 0);
      RECT row{};
      SendMessageW(commitPopup_, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row));
      SendMessageW(commitPopup_, WM_MOUSEMOVE, 0, MAKELPARAM(row.left + 8, (row.top + row.bottom) / 2));
    }
    else if (command == L"end-hover")
    {
      automationHover_ = false;
      SendMessageW(commits_, CB_SHOWDROPDOWN, FALSE, 0);
      endPreview();
    }
    else if (command == L"select-commit")
    {
      if (loading_ || SendMessageW(source_, CB_GETCURSEL, 0, 0) != 3)
        throw std::runtime_error("Select Ready to push and wait for loading first.");
      int index = integer(arg(0), 0, static_cast<int>(series_.size()));
      SendMessageW(commits_, CB_SETCURSEL, index, 0);
      refresh(true);
    }
    else if (command == L"explorer-scrollbar")
    {
      int index = integer(arg(0), 0, 2);
      auto action = arg(1);
      if (action != L"top" && action != L"bottom")
        throw std::runtime_error("Scrollbar action must be top or bottom.");
      HWND bar = explorerBars_[index];
      RECT area{}, thumb = explorerThumb(index);
      GetClientRect(bar, &area);
      int x = area.right / 2, start = (thumb.top + thumb.bottom) / 2;
      int finish = action == L"top" ? 0 : std::max(0L, area.bottom - 1);
      SendMessageW(bar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, start));
      SendMessageW(bar, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, finish));
      SendMessageW(bar, WM_LBUTTONUP, 0, MAKELPARAM(x, finish));
    }
    else if (command == L"explorer-hover-file")
    {
      int index = integer(arg(0), 0, static_cast<int>(explorerFileItems_.size()) - 1);
      RECT row{};
      SendMessageW(explorerFiles_, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&row));
      SendMessageW(explorerFiles_, WM_MOUSEMOVE, 0, MAKELPARAM(row.left + 8, (row.top + row.bottom) / 2));
    }
    else if (command == L"explorer-message-select-all")
      SendMessageW(explorerMessage_, WM_CHAR, 1, 0);
    else if (command == L"explorer-wheel")
    {
      HWND list = arg(0) == L"commits" ? explorerCommits_ : arg(0) == L"files" ? explorerFiles_ : nullptr;
      if (!list)
        throw std::runtime_error("Explorer wheel target must be commits or files.");
      auto delta = static_cast<short>(integer(arg(1), -1200, 1200));
      SendMessageW(list, WM_MOUSEWHEEL, MAKEWPARAM(0, delta), 0);
    }
    else if (command == L"scroll")
      diff_.scroll(integer(arg(0), 0, std::max(0, diff_.rowCount() - 1)));
    else if (command == L"scrollbar")
    {
      auto action = arg(0);
      RECT area{};
      GetClientRect(diff_.handle(), &area);
      int x = std::max(0L, area.right - 4);
      if (action == L"hover")
        SendMessageW(diff_.handle(), WM_MOUSEMOVE, 0, MAKELPARAM(x, area.bottom / 2));
      else if (action == L"leave")
        SendMessageW(diff_.handle(), WM_MOUSELEAVE, 0, 0);
      else if (action == L"press-middle")
        SendMessageW(diff_.handle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, area.bottom / 2));
      else if (action == L"release")
        SendMessageW(diff_.handle(), WM_LBUTTONUP, 0, MAKELPARAM(x, area.bottom / 2));
      else if (action == L"drag-bottom")
      {
        SendMessageW(diff_.handle(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, 4));
        SendMessageW(diff_.handle(), WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x, std::max(4L, area.bottom - 4)));
        SendMessageW(diff_.handle(), WM_LBUTTONUP, 0, MAKELPARAM(x, std::max(4L, area.bottom - 4)));
      }
      else
        throw std::runtime_error("Unknown scrollbar action.");
    }
    else if (command == L"navigate-change")
    {
      auto value = arg(0);
      if (value != L"previous" && value != L"next")
        throw std::runtime_error("Change navigation must be previous or next.");
      diff_.navigateChange(value == L"previous" ? -1 : 1);
    }
    else if (command == L"zoom")
      diff_.zoom(integer(arg(0), -40, 40));
    else if (command == L"ctrl-wheel")
      SendMessageW(diff_.handle(), WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, static_cast<short>(integer(arg(0), -1200, 1200))), 0);
    else if (command == L"key")
    {
      const std::pair<const wchar_t *, int> keys[] = {{L"up", VK_UP}, {L"down", VK_DOWN}, {L"left", VK_LEFT}, {L"right", VK_RIGHT},
        {L"page-up", VK_PRIOR}, {L"page-down", VK_NEXT}, {L"home", VK_HOME}, {L"end", VK_END}};
      auto found = std::find_if(std::begin(keys), std::end(keys), [&](const auto &k) { return arg(0) == k.first; });
      if (found == std::end(keys))
        throw std::runtime_error("Unknown diff key.");
      SendMessageW(diff_.handle(), WM_KEYDOWN, found->second, 0);
    }
    else if (command == L"resize")
    {
      int width = integer(arg(0), 820, 4096), height = integer(arg(1), 500, 4096);
      SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    else if (command == L"splitter")
    {
      int position = integer(arg(0), 0, 4096);
      int y = (splitter_.top + splitter_.bottom) / 2;
      SendMessageW(hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(splitter_.left, y));
      SendMessageW(hwnd_, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(position, y));
      SendMessageW(hwnd_, WM_LBUTTONUP, 0, MAKELPARAM(position, y));
    }
    else if (command == L"screenshot" || command == L"screenshot-commits")
    {
      auto name = arg(0);
      if (name.empty() || name.find_first_of(L"/\\:") != std::wstring::npos || fs::path(name).extension() != L".png")
        throw std::runtime_error("Screenshot argument must be a PNG filename, without directories.");
      if (command == L"screenshot-commits" && !SendMessageW(commits_, CB_GETDROPPEDSTATE, 0, 0))
        throw std::runtime_error("Open the commit dropdown first.");
      saveScreenshot(command == L"screenshot" ? hwnd_ : commitPopup_, (root / name).wstring());
    }
    else if (command == L"close")
      close = true;
    else
      throw std::runtime_error("Unknown command.");
  }
  catch (const std::exception &e)
  {
    error = e.what();
  }
  pendingResponse_ = "{\"id\":" + json(id) + ",\"ok\":" + (error.empty() ? "true" : "false") + ",\"error\":" + json(fromUtf8(error)) +
                     ",\"state\":" + stateJson() + "}";
  pendingClose_ = close;
  publishAutomationResponse();
}
void MainWindow::publishAutomationResponse()
{
  namespace fs = std::filesystem;
  auto root = fs::path(automationDirectory_);
  // Retain the completed response across sharing violations. Retry on the next timer
  // without executing the command again or blocking the UI thread.
  try
  {
    fs::remove(root / L"request.txt");
    auto temp = root / L"response.tmp", destination = root / L"response.json";
    std::ofstream output(temp, std::ios::binary | std::ios::trunc);
    output << pendingResponse_;
    output.close();
    if (!output)
      throw std::runtime_error("Cannot write automation response.");
    if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      throw std::runtime_error("Cannot publish automation response.");
  }
  catch (...)
  {
    return;
  }
  pendingResponse_.clear();
  if (pendingClose_)
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
  pendingClose_ = false;
}
} // namespace gdv
