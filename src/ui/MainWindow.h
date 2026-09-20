#pragma once
#include "DiffView.h"
#include "Settings.h"
#include "app/RepositoryController.h"
#include <commctrl.h>
#include <memory>
#include <unordered_map>
namespace gdv
{
class MainWindow
{
public:
  int run(HINSTANCE instance, int show, std::wstring directory, std::wstring automationDirectory = L"", std::wstring hashPrefix = L"",
    bool startHistory = false);
  ~MainWindow();

private:
  enum class FileListItemKind
  {
    File,
    CommitMessage,
    Commit,
    Spacer,
    Summary,
    Section,
    Notice,
    LoadMore
  };
  enum class FileListGroup
  {
    None,
    Unstaged,
    Staged,
    Outgoing,
    History
  };
  enum class FileStatsMode
  {
    None,
    Bars,
    Numbers,
    Auto
  };
  struct FileListItem
  {
    FileListItemKind kind{FileListItemKind::File};
    const FileDiff *file{};
    size_t commitIndex{};
    size_t added{}, removed{};
    std::wstring label, key;
    FileListGroup group{FileListGroup::None};
    const DiffDocument *document{};
    const Commit *commit{};
    size_t maxAdded{}, maxRemoved{};
  };
  static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
  LRESULT message(UINT, WPARAM, LPARAM);
  void createControls();
  void layout();
  void updateFonts();
  void refresh(bool seriesSelection = false, bool keepCommitContext = false);
  void loaded();
  void selectFile();
  void requestSelectedFullFile(const FileListItem &item);
  void updateStatus();
  void rememberFileScroll();
  std::wstring fileScrollKey(const std::wstring &path) const;
  void navigateList(int direction, bool focusDiff = true);
  void toggleCommitMessage();
  void syncComments();
  void openCommentEditor();
  void copyComments();
  void loadMoreHistory();
  void loadMoreCommitContext(bool descendants);
  void loadMore(const FileListItem &item);
  int messageReturnIndex_{-1}, messageReturnTop_{};
  std::vector<FileListItem> fileListItems_, explorerGroups_, explorerFileItems_;
  std::vector<ReviewComment> comments_;
  std::unordered_map<const FileDiff *, int> fileItemIndex_;
  FileStatsMode fileStatsMode_{FileStatsMode::Auto};
  void sourceChanged();
  void toggle();
  void toggleFullFile();
  void previewCommit(int index);
  void endPreview();
  void drawListItem(const DRAWITEMSTRUCT &item);
  void drawStatus(const DRAWITEMSTRUCT &item) const;
  void drawSplitter(HDC dc) const;
  void moveSplitter(int x);
  void rebuildExplorer();
  void selectExplorerGroup(int index, bool preserveFile = false, bool selectLastFile = false);
  void selectExplorerFile(int index);
  void updateExplorerBar(int index);
  void scrollExplorerBar(int index, int top);
  RECT explorerThumb(int index) const;
  static LRESULT CALLBACK explorerBarProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  static LRESULT CALLBACK explorerMessageProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  void setExplorerLayout(bool enabled);
  void moveExplorerSplitter(int index, int position);
  void redrawExplorerPanels();
  static LRESULT CALLBACK comboProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  static LRESULT CALLBACK commitListProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  void screenshot();
  void saveSettings();
  void applyTheme();
  void startStatusAnimation();
  void stopStatusAnimation();
  std::wstring filePathAt(int index) const;
  static LRESULT CALLBACK filesProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  static LRESULT CALLBACK explorerListProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  HWND tooltip_{}, themeButton_{}, fullFileButton_{}, layoutButton_{}, copyCommentsButton_{}, explorerCommits_{}, explorerMessage_{},
    explorerFiles_{}, explorerBars_[3]{}, explorerCommitLabel_{}, explorerMessageLabel_{}, explorerFilesLabel_{};
  std::wstring tooltipText_, savedCommit_, infoTooltipText_;
  int tooltipIndex_{-1};
  HWND tooltipOwner_{};
  HBRUSH backgroundBrush_{}, fieldBrush_{};
  void automationTick();
  void publishAutomationResponse();
  std::string stateJson() const;
  HWND control(const wchar_t *cls, const wchar_t *text, DWORD style, int id);
  HWND hwnd_{}, refresh_{}, source_{}, view_{}, info_{}, baseLabel_{}, base_{}, targetLabel_{}, target_{}, compare_{}, files_{},
    fileLabel_{}, commits_{}, commitLabel_{}, status_{};
  HINSTANCE instance_{};
  HFONT font_{}, boldFont_{};
  UINT dpi_{96};
  std::wstring directory_, selectedPath_, selectedListKey_;
  std::wstring readyBase_, rangeBase_;
  ChangeSource baseMode_{ChangeSource::Unstaged};
  std::wstring commitBranch_, commitBranchCommit_;
  std::wstring scrollContext_;
  std::wstring automationDirectory_;
  std::unordered_map<std::wstring, int> fileScrollPositions_;
  std::unordered_map<std::wstring, std::wstring> explorerFileSelections_;
  std::wstring activeExplorerGroupKey_;
  std::unordered_map<std::wstring, DiffDocument> fullFileDocuments_;
  std::string pendingResponse_;
  bool pendingClose_{};
  bool startHistory_{};
  RepositorySnapshot snapshot_;
  FileDiff commitMessageFile_;
  FileDiff listMessageFile_;
  FileDiff hoverMessageFile_;
  const FileDiff *previewPreviousFile_{};
  bool previewPreviousPlain_{};
  int previewIndex_{-1}, previewTop_{};
  size_t explorerResizeRepaints_{};
  int filePaneWidth_{}, splitterDragOffset_{}, explorerCommitWidth_{300}, explorerMessageWidth_{360}, explorerTopHeight_{280},
    explorerDragIndex_{-1};
  RECT splitter_{}, explorerSplitters_[3]{};
  bool draggingSplitter_{}, explorerLayout_{};
  bool automationHover_{};
  HWND commitPopup_{};
  std::vector<Commit> series_;
  std::unique_ptr<RepositoryController> controller_;
  Settings settings_;
  DiffView diff_;
  bool side_{}, fullFile_{}, loading_{}, initialLoad_{true};
  bool loadMoreLoading_{};
  bool statusAnimationActive_{};
  size_t historyInitialLimit_{10}, historyLimit_{10};
  size_t commitAncestorLimit_{10}, commitDescendantLimit_{5};
  std::wstring fullFileLoadingKey_;
  int historyListTop_{}, historyExplorerTop_{}, historyDiffTop_{}, statusAnimationPhase_{};
  int fileListWheel_{}, explorerWheel_[2]{}, explorerMessageWheel_{}, explorerBarDrag_[3]{-1, -1, -1};
};
} // namespace gdv
