#include "CommitSearch.h"
#include "Theme.h"
#include "ThemedCombo.h"
#include "diff/UnifiedDiffParser.h"
#include <algorithm>
#include <commctrl.h>
#include <dwmapi.h>
#include <thread>
#include <uxtheme.h>
namespace gdv
{
namespace
{
constexpr int findId = IDOK, cancelId = IDCANCEL, openId = 103, moreId = 104;
constexpr int branchId = 110, messageId = 111, authorId = 112, pathId = 113, listId = 120, statusId = 121;
constexpr UINT finished = WM_APP + 10;
constexpr const wchar_t *allBranchesLabel = L"All branches";
struct Search
{
  HWND window{}, branch{}, message{}, author{}, path{}, list{}, status{}, find{}, open{}, more{}, tooltip{};
  HWND labels[4]{};
  HBRUSH background{}, surface{};
  HFONT font{};
  int dpi{}, lineHeight{}, labelWidth{}, buttonWidth{}, hovered{-1};
  int lastSelected{-1};
  bool dark{}, running{}, loadingRefs{}, hasMore{}, showBranches{true};
  std::atomic_bool cancel{false};
  std::thread worker;
  CommitSearchRequest request;
  CommitSearchPage page;
  std::vector<CommitSearchMatch> matches;
  std::vector<std::wstring> refs;
  std::wstring error, tooltipText, resultBranch, restoredStatus;
  CommitSearchState *state{};
  std::optional<Commit> result;
  int scale(int n) const { return MulDiv(n, dpi, 96); }
};
std::wstring text(HWND window)
{
  int length = GetWindowTextLengthW(window);
  std::wstring value(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(window, value.data(), length + 1);
  value.resize(length);
  return value;
}
std::wstring columnText(const CommitSearchMatch &match, int column)
{
  if (column == 0)
    return match.commit.author;
  if (column == 2)
    return match.commit.subject;
  if (column == 3)
    return match.commit.branch;
  auto value = match.date.substr(0, 16);
  if (value.size() > 10)
    value[10] = L' ';
  return value;
}
void layout(Search &search)
{
  if (!search.list)
    return;
  RECT client{};
  GetClientRect(search.window, &client);
  int pad = search.scale(16), gap = search.scale(8), fieldHeight = search.lineHeight + gap;
  int top = pad;
  HWND fields[] = {search.branch, search.message, search.author, search.path};
  for (int i = 0; i < 4; ++i)
  {
    MoveWindow(search.labels[i], pad, top + gap / 2, search.labelWidth, search.lineHeight, TRUE);
    MoveWindow(fields[i], pad + search.labelWidth, top, client.right - 2 * pad - search.labelWidth,
      i == 0 ? fieldHeight + 8 * search.lineHeight : fieldHeight, TRUE);
    top += fieldHeight + gap;
  }
  int bottom = client.bottom - pad - fieldHeight;
  MoveWindow(search.status, pad, top, client.right - 2 * pad, search.lineHeight, TRUE);
  top += search.lineHeight + gap;
  MoveWindow(search.list, pad, top, client.right - 2 * pad, std::max(1, bottom - gap - top), TRUE);
  MoveWindow(search.more, pad, bottom, search.buttonWidth, fieldHeight, TRUE);
  HWND buttons[] = {search.open, search.find, GetDlgItem(search.window, cancelId)};
  for (int i = 0; i < 3; ++i)
    MoveWindow(buttons[i], client.right - pad - (3 - i) * search.buttonWidth - (2 - i) * gap, bottom, search.buttonWidth, fieldHeight,
      TRUE);
  int width = client.right - 2 * pad;
  int authorWidth = std::max(search.scale(180), width / 4);
  int dateWidth = std::max(search.scale(150), search.lineHeight * 8);
  ListView_SetColumnWidth(search.list, 0, authorWidth);
  ListView_SetColumnWidth(search.list, 1, dateWidth);
  int branchWidth = search.showBranches ? std::max(search.scale(170), width / 5) : 0;
  ListView_SetColumnWidth(search.list, 2,
    std::max(search.scale(200), width - authorWidth - dateWidth - branchWidth - search.scale(20)));
  if (search.showBranches)
    ListView_SetColumnWidth(search.list, 3, branchWidth);
}
void updateBranchColumn(Search &search)
{
  bool all = text(search.branch) == allBranchesLabel;
  if (all == search.showBranches)
    return;
  search.showBranches = all;
  if (all)
  {
    LVCOLUMNW column{};
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = const_cast<wchar_t *>(L"Branch");
    column.cx = search.scale(170);
    ListView_InsertColumn(search.list, 3, &column);
  }
  else
    ListView_DeleteColumn(search.list, 3);
  layout(search);
  InvalidateRect(search.list, nullptr, TRUE);
}
void updateSelection(Search &search)
{
  int selected = ListView_GetNextItem(search.list, -1, LVNI_SELECTED);
  if (selected >= 0)
    search.lastSelected = selected;
  ShowWindow(search.open, selected >= 0 ? SW_SHOW : SW_HIDE);
  EnableWindow(search.open, !search.running);
}
void saveState(Search &search)
{
  if (!search.state || !search.list)
    return;
  auto &state = *search.state;
  state.request = search.request;
  state.branch = text(search.branch);
  state.message = text(search.message);
  state.author = text(search.author);
  state.path = text(search.path);
  state.resultBranch = search.resultBranch;
  state.matches = search.matches;
  state.selected = search.lastSelected;
  state.topIndex = ListView_GetTopIndex(search.list);
  state.hasMore = search.hasMore;
  state.status = search.loadingRefs ? search.restoredStatus : search.running ? L"Search cancelled." : text(search.status);
}
void startSearch(Search &search, bool more)
{
  if (search.running)
    return;
  if (!more)
  {
    search.request.branch = text(search.branch);
    search.request.allBranches = search.request.branch == allBranchesLabel;
    search.request.branches.clear();
    if (search.request.allBranches)
      search.request.branch.clear();
    if (search.request.branch.empty())
      search.request.branch = L"HEAD";
    search.resultBranch = search.request.branch;
    search.request.message = text(search.message);
    search.request.author = text(search.author);
    search.request.path = text(search.path);
    search.request.skip = 0;
    search.hasMore = false;
    ShowWindow(search.more, SW_HIDE);
    SendMessageW(search.tooltip, TTM_POP, 0, 0);
    search.matches.clear();
    search.lastSelected = -1;
    ListView_SetItemCount(search.list, 0);
  }
  else
    search.request.skip = search.matches.size();
  search.error.clear();
  search.cancel = false;
  search.running = true;
  EnableWindow(search.find, FALSE);
  EnableWindow(search.more, FALSE);
  updateSelection(search);
  SetWindowTextW(search.status, L"Searching...");
  search.worker = std::thread([&search] {
    try
    {
      search.page = GitRepository{}.searchCommits(search.request, search.cancel);
    }
    catch (const std::exception &error)
    {
      search.error = fromUtf8(error.what());
    }
    if (!search.cancel)
      PostMessageW(search.window, finished, 0, 0);
  });
}
void complete(Search &search)
{
  if (search.worker.joinable())
    search.worker.join();
  search.running = false;
  if (search.loadingRefs)
  {
    search.loadingRefs = false;
    for (const auto &ref : search.refs)
      SendMessageW(search.branch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(ref.c_str()));
    auto branch = text(search.branch);
    auto selected = SendMessageW(search.branch, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(branch.c_str()));
    if (selected != CB_ERR)
      SendMessageW(search.branch, CB_SETCURSEL, selected, 0);
    SetWindowTextW(search.status,
      search.error.empty() ? (search.restoredStatus.empty() ? L"Enter filters and click Find." : search.restoredStatus.c_str())
                           : search.error.c_str());
    EnableWindow(search.find, TRUE);
    EnableWindow(search.more, search.hasMore);
    updateSelection(search);
    return;
  }
  if (!search.error.empty())
  {
    SetWindowTextW(search.status, search.error.c_str());
    EnableWindow(search.more, search.hasMore);
    updateSelection(search);
    EnableWindow(search.find, TRUE);
    return;
  }
  if (search.request.allBranches)
    search.request.branches = search.page.branches;
  else
    search.request.branch = search.page.head;
  search.hasMore = search.page.hasMore;
  for (auto &match : search.page.matches)
  {
    if (!search.request.allBranches)
      match.commit.branch = search.resultBranch;
    search.matches.push_back(std::move(match));
  }
  ListView_SetItemCount(search.list, static_cast<int>(search.matches.size()));
  auto status =
    L"Found " + std::to_wstring(search.matches.size()) + L" commits" + (search.hasMore ? L". More results available." : L".");
  SetWindowTextW(search.status, status.c_str());
  ShowWindow(search.more, search.hasMore ? SW_SHOW : SW_HIDE);
  EnableWindow(search.more, search.hasMore);
  updateSelection(search);
  InvalidateRect(search.list, nullptr, TRUE);
  EnableWindow(search.find, TRUE);
}
void openSelected(Search &search)
{
  if (search.running)
    return;
  int selected = ListView_GetNextItem(search.list, -1, LVNI_SELECTED);
  if (selected >= 0 && static_cast<size_t>(selected) < search.matches.size())
  {
    search.result = search.matches[static_cast<size_t>(selected)].commit;
    DestroyWindow(search.window);
  }
}
LRESULT CALLBACK listProcedure(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data)
{
  auto &search = *reinterpret_cast<Search *>(data);
  if (message == WM_MOUSEMOVE)
  {
    LVHITTESTINFO hit{};
    hit.pt = {static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
    int hovered = ListView_SubItemHitTest(window, &hit);
    TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
    TrackMouseEvent(&tracking);
    if (hovered != search.hovered)
    {
      search.hovered = hovered;
      search.tooltipText = hovered >= 0 && static_cast<size_t>(hovered) < search.matches.size()
                             ? search.matches[static_cast<size_t>(hovered)].commit.message
                             : L"";
      SendMessageW(search.tooltip, TTM_POP, 0, 0);
    }
    POINT point = hit.pt;
    ClientToScreen(window, &point);
    MSG event{window, message, w, l, static_cast<DWORD>(GetMessageTime()), point};
    SendMessageW(search.tooltip, TTM_RELAYEVENT, 0, reinterpret_cast<LPARAM>(&event));
  }
  if (message == WM_MOUSELEAVE || message == WM_MOUSEWHEEL || message == WM_VSCROLL || message == WM_HSCROLL)
  {
    search.hovered = -1;
    SendMessageW(search.tooltip, TTM_POP, 0, 0);
  }
  if (message == WM_NOTIFY)
  {
    auto header = reinterpret_cast<NMHDR *>(l);
    if (header->hwndFrom == ListView_GetHeader(window) && header->code == NM_CUSTOMDRAW)
    {
      auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
      if (draw->dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
      if (draw->dwDrawStage == CDDS_POSTPAINT)
      {
        RECT remainder{}, last{};
        GetClientRect(header->hwndFrom, &remainder);
        int count = Header_GetItemCount(header->hwndFrom);
        if (count > 0 && Header_GetItemRect(header->hwndFrom, count - 1, &last))
          remainder.left = std::clamp(last.right, remainder.left, remainder.right);
        FillRect(draw->hdc, &remainder, search.background);
        return CDRF_DODEFAULT;
      }
      if (draw->dwDrawStage == CDDS_ITEMPREPAINT)
      {
        FillRect(draw->hdc, &draw->rc, search.background);
        SetTextColor(draw->hdc, themeColor(ThemeColor::HeaderText));
        SetBkMode(draw->hdc, TRANSPARENT);
        auto oldFont = SelectObject(draw->hdc, search.font);
        wchar_t label[80]{};
        HDITEMW item{};
        item.mask = HDI_TEXT;
        item.pszText = label;
        item.cchTextMax = 80;
        Header_GetItem(header->hwndFrom, static_cast<int>(draw->dwItemSpec), &item);
        RECT rect = draw->rc;
        rect.left += search.scale(8);
        DrawTextW(draw->hdc, label, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(draw->hdc, oldFont);
        return CDRF_SKIPDEFAULT;
      }
    }
  }
  return DefSubclassProc(window, message, w, l);
}
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l)
{
  auto search = reinterpret_cast<Search *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE)
  {
    search = static_cast<Search *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
    search->window = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(search));
  }
  if (!search)
    return DefWindowProcW(window, message, w, l);
  switch (message)
  {
    case WM_ERASEBKGND:
    {
      RECT rect{};
      GetClientRect(window, &rect);
      FillRect(reinterpret_cast<HDC>(w), &rect, search->background);
      return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    {
      bool field = message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX;
      HDC dc = reinterpret_cast<HDC>(w);
      SetTextColor(dc, themeColor(ThemeColor::Text));
      SetBkColor(dc, themeColor(field ? ThemeColor::Surface : ThemeColor::Window));
      return reinterpret_cast<LRESULT>(field ? search->surface : search->background);
    }
    case WM_SIZE: layout(*search); return 0;
    case WM_MEASUREITEM:
    {
      auto item = reinterpret_cast<MEASUREITEMSTRUCT *>(l);
      if (item->CtlID == branchId)
      {
        item->itemHeight = search->lineHeight + search->scale(8);
        return TRUE;
      }
      break;
    }
    case WM_DRAWITEM:
    {
      auto item = reinterpret_cast<DRAWITEMSTRUCT *>(l);
      if (item->CtlID != branchId)
        break;
      int saved = SaveDC(item->hDC);
      bool selected = (item->itemState & ODS_SELECTED) != 0;
      auto brush = CreateSolidBrush(themeColor(selected ? ThemeColor::ListSelection : ThemeColor::Surface));
      FillRect(item->hDC, &item->rcItem, brush);
      DeleteObject(brush);
      std::wstring value;
      if (item->itemID != static_cast<UINT>(-1))
      {
        auto length = SendMessageW(item->hwndItem, CB_GETLBTEXTLEN, item->itemID, 0);
        if (length >= 0)
        {
          value.resize(static_cast<size_t>(length) + 1);
          SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(value.data()));
          value.resize(static_cast<size_t>(length));
        }
      }
      SetBkMode(item->hDC, TRANSPARENT);
      SetTextColor(item->hDC, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Text));
      SelectObject(item->hDC, search->font);
      RECT rect = item->rcItem;
      rect.left += search->scale(6);
      rect.right -= search->scale(6);
      DrawTextW(item->hDC, value.c_str(), -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
      if (item->itemState & ODS_FOCUS)
        DrawFocusRect(item->hDC, &item->rcItem);
      RestoreDC(item->hDC, saved);
      return TRUE;
    }
    case WM_GETMINMAXINFO:
    {
      auto limits = reinterpret_cast<MINMAXINFO *>(l);
      limits->ptMinTrackSize = {4 * search->buttonWidth + search->scale(64), 9 * search->lineHeight + search->scale(150)};
      return 0;
    }
    case finished: complete(*search); return 0;
    case WM_COMMAND:
      switch (LOWORD(w))
      {
        case branchId:
          if (HIWORD(w) == CBN_SELCHANGE)
          {
            // The edit text is updated after CBN_SELCHANGE returns.
            auto selected = SendMessageW(search->branch, CB_GETCURSEL, 0, 0);
            if (selected >= 0)
            {
              int length = static_cast<int>(SendMessageW(search->branch, CB_GETLBTEXTLEN, selected, 0));
              std::wstring value(static_cast<size_t>(length) + 1, L'\0');
              SendMessageW(search->branch, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(value.data()));
              SetWindowTextW(search->branch, value.c_str());
            }
          }
          if (HIWORD(w) == CBN_SELCHANGE || HIWORD(w) == CBN_EDITCHANGE)
            updateBranchColumn(*search);
          return 0;
        case findId: startSearch(*search, false); return 0;
        case moreId: startSearch(*search, true); return 0;
        case openId: openSelected(*search); return 0;
        case cancelId:
          search->cancel = true;
          DestroyWindow(window);
          return 0;
      }
      break;
    case WM_CLOSE:
      search->cancel = true;
      DestroyWindow(window);
      return 0;
    case WM_DESTROY: saveState(*search); return 0;
    case WM_NOTIFY:
    {
      auto header = reinterpret_cast<NMHDR *>(l);
      if (header->hwndFrom == search->tooltip && header->code == TTN_GETDISPINFOW)
      {
        reinterpret_cast<NMTTDISPINFOW *>(l)->lpszText = search->tooltipText.data();
        return 0;
      }
      if (header->hwndFrom == search->list)
      {
        if (header->code == LVN_ITEMCHANGED)
          updateSelection(*search);
        else if (header->code == NM_DBLCLK)
        {
          if (reinterpret_cast<NMITEMACTIVATE *>(l)->iItem >= 0)
            openSelected(*search);
        }
        else if (header->code == LVN_GETDISPINFOW)
        {
          auto info = reinterpret_cast<NMLVDISPINFOW *>(l);
          if ((info->item.mask & LVIF_TEXT) && info->item.iItem >= 0 && static_cast<size_t>(info->item.iItem) < search->matches.size())
          {
            const auto &match = search->matches[static_cast<size_t>(info->item.iItem)];
            auto value = columnText(match, info->item.iSubItem);
            lstrcpynW(info->item.pszText, value.c_str(), info->item.cchTextMax);
          }
          return 0;
        }
        else if (header->code == NM_CUSTOMDRAW)
        {
          auto draw = reinterpret_cast<NMLVCUSTOMDRAW *>(l);
          if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
            return CDRF_NOTIFYITEMDRAW;
          if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT)
          {
            int index = static_cast<int>(draw->nmcd.dwItemSpec);
            bool selected = (ListView_GetItemState(search->list, index, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            HBRUSH brush = selected ? CreateSolidBrush(themeColor(ThemeColor::ListSelection)) : search->surface;
            RECT bounds{};
            ListView_GetItemRect(search->list, index, &bounds, LVIR_BOUNDS);
            FillRect(draw->nmcd.hdc, &bounds, brush);
            if (selected)
              DeleteObject(brush);
            auto oldFont = SelectObject(draw->nmcd.hdc, search->font);
            SetBkMode(draw->nmcd.hdc, TRANSPARENT);
            SetTextColor(draw->nmcd.hdc, themeColor(selected ? ThemeColor::SelectionText : ThemeColor::Text));
            for (int column = 0; column < (search->showBranches ? 4 : 3); ++column)
            {
              auto value = columnText(search->matches[static_cast<size_t>(index)], column);
              RECT cell{};
              ListView_GetSubItemRect(search->list, index, column, LVIR_BOUNDS, &cell);
              if (column == 0)
                cell.right = cell.left + ListView_GetColumnWidth(search->list, 0);
              cell.left += search->scale(8);
              cell.right -= search->scale(8);
              DrawTextW(draw->nmcd.hdc, value.c_str(), -1, &cell, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            if ((ListView_GetItemState(search->list, index, LVIS_FOCUSED) & LVIS_FOCUSED) && GetFocus() == search->list)
              DrawFocusRect(draw->nmcd.hdc, &bounds);
            SelectObject(draw->nmcd.hdc, oldFont);
            return CDRF_SKIPDEFAULT;
          }
        }
      }
      if (search->dark && header->code == NM_CUSTOMDRAW &&
          (header->idFrom == findId || header->idFrom == cancelId || header->idFrom == openId || header->idFrom == moreId))
      {
        auto draw = reinterpret_cast<NMCUSTOMDRAW *>(l);
        if (draw->dwDrawStage == CDDS_PREPAINT)
        {
          FillRect(draw->hdc, &draw->rc, search->surface);
          HBRUSH border = CreateSolidBrush(themeColor(ThemeColor::Border));
          FrameRect(draw->hdc, &draw->rc, border);
          DeleteObject(border);
          auto oldFont = SelectObject(draw->hdc, search->font);
          SetBkMode(draw->hdc, TRANSPARENT);
          SetTextColor(draw->hdc, themeColor(IsWindowEnabled(header->hwndFrom) ? ThemeColor::Text : ThemeColor::MutedText));
          auto value = text(header->hwndFrom);
          DrawTextW(draw->hdc, value.c_str(), -1, &draw->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
  }
  return DefWindowProcW(window, message, w, l);
}
} // namespace
std::optional<Commit> searchRepositoryCommits(HWND owner, HINSTANCE instance, const std::wstring &directory, int fontPoints,
  CommitSearchState &state)
{
  static bool registered = false;
  if (!registered)
  {
    WNDCLASSW cls{};
    cls.hInstance = instance;
    cls.lpfnWndProc = procedure;
    cls.lpszClassName = L"GitDiffViewer.CommitSearch";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    registered = RegisterClassW(&cls) != 0;
  }
  Search search;
  if (state.request.directory != directory)
    state = {};
  search.state = &state;
  search.request = state.request;
  search.matches = state.matches;
  search.resultBranch = state.resultBranch;
  search.hasMore = state.hasMore;
  search.lastSelected = state.selected;
  search.restoredStatus = state.status;
  search.request.directory = directory;
  search.dark = darkTheme;
  search.dpi = static_cast<int>(GetDpiForWindow(owner));
  search.background = CreateSolidBrush(themeColor(ThemeColor::Window));
  search.surface = CreateSolidBrush(themeColor(ThemeColor::Surface));
  search.font = CreateFontW(-MulDiv(fontPoints, search.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  HDC dc = GetDC(owner);
  auto oldFont = SelectObject(dc, search.font);
  TEXTMETRICW metrics{};
  GetTextMetricsW(dc, &metrics);
  search.lineHeight = metrics.tmHeight;
  SIZE label{}, button{};
  GetTextExtentPoint32W(dc, L"File / folder:", 14, &label);
  GetTextExtentPoint32W(dc, L"Open commit", 11, &button);
  search.labelWidth = label.cx + search.scale(16);
  search.buttonWidth = std::max<LONG>(search.scale(100), button.cx + search.scale(24));
  SelectObject(dc, oldFont);
  ReleaseDC(owner, dc);
  RECT ownerRect{};
  GetWindowRect(owner, &ownerRect);
  MONITORINFO monitor{sizeof(monitor)};
  GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
  int width =
    std::min<LONG>(std::max(search.scale(960), 4 * search.buttonWidth + search.scale(64)), monitor.rcWork.right - monitor.rcWork.left);
  int height = std::min<LONG>(std::max(search.scale(600), 15 * search.lineHeight + search.scale(150)),
    monitor.rcWork.bottom - monitor.rcWork.top);
  int x = std::clamp<LONG>((ownerRect.left + ownerRect.right - width) / 2, monitor.rcWork.left, monitor.rcWork.right - width);
  int y = std::clamp<LONG>((ownerRect.top + ownerRect.bottom - height) / 2, monitor.rcWork.top, monitor.rcWork.bottom - height);
  HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"GitDiffViewer.CommitSearch", L"Search commits",
    WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX, x, y, width, height, owner, nullptr, instance, &search);
  if (dialog)
  {
    BOOL dark = search.dark;
    if (FAILED(DwmSetWindowAttribute(dialog, 20, &dark, sizeof(dark))))
      DwmSetWindowAttribute(dialog, 19, &dark, sizeof(dark));
    COLORREF caption = themeColor(ThemeColor::Window), captionText = themeColor(ThemeColor::Text);
    DwmSetWindowAttribute(dialog, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(dialog, 36, &captionText, sizeof(captionText));
    auto create = [&](const wchar_t *type, const wchar_t *label, DWORD style, int id) {
      HWND control = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
      SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(search.font), TRUE);
      SetWindowTheme(control, search.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
      return control;
    };
    const wchar_t *labels[] = {L"Branch:", L"Message:", L"Author:", L"File / folder:"};
    for (int i = 0; i < 4; ++i)
      search.labels[i] = create(L"STATIC", labels[i], SS_NOPREFIX, 0);
    search.branch = create(L"COMBOBOX", L"",
      WS_TABSTOP | WS_VSCROLL | WS_CLIPCHILDREN | CBS_DROPDOWN | CBS_AUTOHSCROLL | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, branchId);
    installThemedCombo(search.branch);
    SendMessageW(search.branch, CB_SETITEMHEIGHT, 0, search.lineHeight + search.scale(8));
    SendMessageW(search.branch, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), search.lineHeight + search.scale(4));
    SendMessageW(search.branch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(allBranchesLabel));
    SendMessageW(search.branch, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"HEAD"));
    SendMessageW(search.branch, CB_SETMINVISIBLE, 8, 0);
    SendMessageW(search.branch, CB_SETCURSEL, 0, 0);
    search.message = create(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, messageId);
    search.author = create(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, authorId);
    search.path = create(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, pathId);
    SetWindowTextW(search.branch, state.branch.c_str());
    SetWindowTextW(search.message, state.message.c_str());
    SetWindowTextW(search.author, state.author.c_str());
    SetWindowTextW(search.path, state.path.c_str());
    SendMessageW(search.path, EM_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"Relative path, folder or mask (e.g. *.cpp)"));
    SendMessageW(search.author, EM_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"Name or email"));
    search.status = create(L"STATIC", L"Loading branches...", SS_NOPREFIX, statusId);
    search.list = create(WC_LISTVIEWW, L"",
      WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA | LVS_NOSORTHEADER, listId);
    ListView_SetExtendedListViewStyle(search.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(search.list, themeColor(ThemeColor::Surface));
    ListView_SetTextBkColor(search.list, themeColor(ThemeColor::Surface));
    ListView_SetTextColor(search.list, themeColor(ThemeColor::Text));
    SetWindowSubclass(search.list, listProcedure, 1, reinterpret_cast<DWORD_PTR>(&search));
    const wchar_t *columns[] = {L"Author", L"Date", L"Commit message", L"Branch"};
    for (int i = 0; i < 4; ++i)
    {
      LVCOLUMNW column{};
      column.mask = LVCF_TEXT | LVCF_WIDTH;
      column.pszText = const_cast<wchar_t *>(columns[i]);
      column.cx = search.scale(200);
      ListView_InsertColumn(search.list, i, &column);
    }
    search.more = create(L"BUTTON", L"Load more", WS_TABSTOP, moreId);
    search.open = create(L"BUTTON", L"Open commit", WS_TABSTOP, openId);
    search.find = create(L"BUTTON", L"Find", WS_TABSTOP | WS_DISABLED | BS_DEFPUSHBUTTON, findId);
    create(L"BUTTON", L"Cancel", WS_TABSTOP, cancelId);
    search.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
      dialog, nullptr, instance, nullptr);
    SetWindowTheme(search.tooltip, L"", L"");
    SendMessageW(search.tooltip, WM_SETFONT, reinterpret_cast<WPARAM>(search.font), FALSE);
    SendMessageW(search.tooltip, TTM_SETTIPBKCOLOR, themeColor(ThemeColor::Surface), 0);
    SendMessageW(search.tooltip, TTM_SETTIPTEXTCOLOR, themeColor(ThemeColor::Text), 0);
    SendMessageW(search.tooltip, TTM_SETMAXTIPWIDTH, 0, search.scale(800));
    SendMessageW(search.tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    TOOLINFOW tool{TTTOOLINFOW_V2_SIZE};
    tool.uFlags = TTF_IDISHWND;
    tool.hwnd = dialog;
    tool.uId = reinterpret_cast<UINT_PTR>(search.list);
    tool.lpszText = LPSTR_TEXTCALLBACKW;
    SendMessageW(search.tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    ShowWindow(search.open, SW_HIDE);
    ShowWindow(search.more, SW_HIDE);
    EnableWindow(search.find, FALSE);
    updateBranchColumn(search);
    layout(search);
    search.running = search.loadingRefs = true;
    ListView_SetItemCount(search.list, static_cast<int>(search.matches.size()));
    if (!search.matches.empty())
    {
      RECT row{};
      ListView_GetItemRect(search.list, 0, &row, LVIR_BOUNDS);
      int top = std::clamp(state.topIndex, 0, static_cast<int>(search.matches.size()) - 1);
      ListView_Scroll(search.list, 0, (top - ListView_GetTopIndex(search.list)) * (row.bottom - row.top));
      if (state.selected >= 0 && static_cast<size_t>(state.selected) < search.matches.size())
      {
        ListView_SetItemState(search.list, state.selected, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(search.list, state.selected, FALSE);
      }
    }
    ShowWindow(search.more, search.hasMore ? SW_SHOW : SW_HIDE);
    EnableWindow(search.more, FALSE);
    updateSelection(search);
    if (!search.restoredStatus.empty())
      SetWindowTextW(search.status, search.restoredStatus.c_str());
    search.worker = std::thread([&search] {
      try
      {
        search.refs = GitRepository{}.listRefs(search.request.directory, search.cancel);
      }
      catch (const std::exception &error)
      {
        search.error = fromUtf8(error.what());
      }
      if (!search.cancel)
        PostMessageW(search.window, finished, 0, 0);
    });
    EnableWindow(owner, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetFocus(search.message);
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
      if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN && GetFocus() == search.list)
        openSelected(search);
      else if (!IsDialogMessageW(dialog, &message))
      {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
    }
    search.cancel = true;
    if (IsWindow(dialog))
      DestroyWindow(dialog);
    if (search.worker.joinable())
      search.worker.join();
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
  }
  DeleteObject(search.font);
  DeleteObject(search.surface);
  DeleteObject(search.background);
  return search.result;
}
} // namespace gdv
