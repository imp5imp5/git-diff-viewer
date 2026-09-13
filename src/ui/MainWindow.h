#pragma once
#include "DiffView.h"
#include "app/RepositoryController.h"
#include <commctrl.h>
#include <memory>
#include <unordered_map>
namespace gdv
{
class MainWindow
{
public:
  int run(HINSTANCE instance, int show, std::wstring directory, std::wstring automationDirectory = L"");
  ~MainWindow();

private:
  static LRESULT CALLBACK procedure(HWND, UINT, WPARAM, LPARAM);
  LRESULT message(UINT, WPARAM, LPARAM);
  void createControls();
  void layout();
  void updateFonts();
  void refresh(bool seriesSelection = false);
  void loaded();
  void selectFile();
  void rememberFileScroll();
  std::wstring fileScrollKey(const std::wstring &path) const;
  void navigateFile(int direction);
  void toggleCommitMessage();
  int messageReturnIndex_{-1}, messageReturnTop_{};
  std::vector<std::pair<size_t, size_t>> fileChanges_;
  void sourceChanged();
  void toggle();
  void toggleFullFile();
  void previewCommit(int index);
  void endPreview();
  void drawListItem(const DRAWITEMSTRUCT &item);
  void drawSplitter(HDC dc) const;
  void moveSplitter(int x);
  static LRESULT CALLBACK comboProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  static LRESULT CALLBACK commitListProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  void screenshot();
  void saveSettings();
  void applyTheme();
  std::wstring filePathAt(int index) const;
  static LRESULT CALLBACK filesProcedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  HWND tooltip_{}, themeButton_{}, fullFileButton_{};
  std::wstring tooltipText_, savedCommit_, infoTooltipText_;
  int tooltipIndex_{-1};
  HBRUSH backgroundBrush_{}, fieldBrush_{};
  void automationTick();
  void publishAutomationResponse();
  std::string stateJson() const;
  HWND control(const wchar_t *cls, const wchar_t *text, DWORD style, int id);
  HWND hwnd_{}, refresh_{}, source_{}, view_{}, info_{}, baseLabel_{}, base_{}, targetLabel_{}, target_{}, compare_{}, files_{},
    fileLabel_{}, commits_{}, commitLabel_{}, status_{};
  HINSTANCE instance_{};
  HFONT font_{};
  UINT dpi_{96};
  std::wstring directory_, selectedPath_;
  std::wstring scrollContext_;
  std::wstring automationDirectory_;
  std::unordered_map<std::wstring, int> fileScrollPositions_;
  std::string pendingResponse_;
  bool pendingClose_{};
  RepositorySnapshot snapshot_;
  FileDiff commitMessageFile_;
  FileDiff hoverMessageFile_;
  const FileDiff *previewPreviousFile_{};
  bool previewPreviousPlain_{};
  int previewIndex_{-1}, previewTop_{};
  int filePaneWidth_{}, splitterDragOffset_{};
  RECT splitter_{};
  bool draggingSplitter_{};
  bool automationHover_{};
  HWND commitPopup_{};
  std::vector<Commit> series_;
  std::unique_ptr<RepositoryController> controller_;
  DiffView diff_;
  bool side_{}, fullFile_{}, loading_{}, initialLoad_{true};
};
} // namespace gdv
