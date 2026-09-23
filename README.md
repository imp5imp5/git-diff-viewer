# GitDiffViewer

GitDiffViewer is a native Windows application for reviewing local Git changes. It builds as **`gfd.exe`** and provides unified and side-by-side diffs in a lightweight Win32 interface.

![GitDiffViewer screenshot](assets/screenshot.png)

The application uses the installed Git command-line client to read repository data. It does not edit files, stage changes, create commits, or run fetch, pull, or push.

## Features

- Review staged changes, unstaged changes, all local changes, repository history, individual commits, and comparisons between refs.
- Inspect local commits relative to an upstream or an explicitly selected base branch.
- Switch between unified and side-by-side diffs and between the classic and Wide Diff window layouts without reloading Git data, while preserving the current viewing position.
- Display line numbers, added and removed lines, file statuses, renames, and binary-file notices.
- Add, edit, delete, and review inline comments on changed lines; copy all comments as review text.
- Read full commit messages in Wide Diff, or preview them by hovering over entries in the local commit dropdown.
- Browse Unicode paths and text, with virtualized diff rendering and background Git loading.
- Adjust the diff font size and switch between dark and light themes.
- Remember window placement and viewing preferences between sessions.

## Requirements

To run:

- Windows 10 or later.
- `git.exe` available on `PATH`.
- A local Git working tree.

To build:

- CMake 3.15 or later.
- Visual Studio 2019 with the **Desktop development with C++** workload and a Windows SDK.
- A C++17 compiler. The build below uses MSVC and targets x64.

The project uses Win32, GDI, Windows common controls, and Windows Imaging Component. It has no third-party GUI framework or runtime library dependency. MSVC builds link the C/C++ runtime statically; Git must still be installed separately.

## Build

Run these commands from the project directory in PowerShell:

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
```

The executable is written to:

```text
build\Release\gfd.exe
```

For a debug build:

```powershell
cmake --build build --config Debug
```

The debug executable is `build\Debug\gfd.exe`. Close the running executable before rebuilding it to avoid a linker file-lock error.

## Usage

Launch `gfd.exe` from a repository directory or any directory beneath it. Git locates the working-tree root by walking up from the current directory. If the repository cannot be opened, the application displays an error dialog.

For example, with this project built in `T:\differ`:

```powershell
Set-Location D:\dagor\prog
& T:\differ\build\Release\gfd.exe
```

To filter the displayed changes and commit history to a file or folder inside a repository, pass its relative path:

```powershell
& .\build\Release\gfd.exe src/git
```

This opens in **History** mode. The filter stays active when you switch sources, refresh, or load more history. Paths are literal, not wildcards. `-- <path>` remains available, including when you supply a repository directory explicitly:

```powershell
& .\build\Release\gfd.exe "D:\dagor" -- prog
```

You can also launch from outside the repository by passing a relative path into it (for example, `gfd git-diff-viewer/src/git` from its parent directory). An absolute directory or a path to the repository root still selects the repository to open.

You can also supply a directory explicitly:

```powershell
& .\build\Release\gfd.exe "D:\dagor"
```

To open a commit by the first four or more hexadecimal characters of its SHA, pass `--hash:<prefix>`:

```powershell
& .\build\Release\gfd.exe "D:\dagor" --hash:a1b2
```

The lookup searches commits reachable from local and remote branches. A single match opens directly in **Single commit** mode. When several commits match, a themed dialog lists their short hashes, first commit-message lines, and containing branches so you can choose one; the selected branch is shown in the top **Branch** field. If none match, the dialog reports that and the application opens with its usual source.

When launched without command-line arguments, the application opens in **History** mode. Launches with arguments restore the saved comparison source; on the first such launch, the source is **Unstaged** with the dark theme and **Wide Diff** layout.

```text
%LOCALAPPDATA%\Gaijin\GitDiffViewer\settings.ini
```

The UTF-8 file is replaced atomically when the application closes. On the first launch of this version, existing preferences are imported from `HKEY_CURRENT_USER\Software\gaijin\git_diff_viewer`. The old registry key is removed only after the new file has been written successfully.

History initially loads 10 commits. To choose a different initial page size, close the application and set `HistoryCommitCount` under `[GitDiffViewer]` in `settings.ini` (values from 1 through 1000 are accepted). **Load more** still appends 10 commits at a time.

### Choose a comparison

Use the source dropdown to choose what to review:

| Source | Comparison |
| --- | --- |
| **Staged** | Index against `HEAD`: changes prepared for the next commit. |
| **Unstaged** | Working tree against the index: tracked edits not yet staged. |
| **All local · HEAD** | Working tree against `HEAD`, combining staged and unstaged changes. |
| **Ready to push** | Local commits reachable from `HEAD` but not from the base ref; the combined diff compares the merge base with `HEAD`. |
| **Single commit** | Up to five descendants appear above the target commit, followed by the target and up to ten first-parent ancestors in the same branch. When more context is available, **Load more** above or below the commit list adds ten descendants or ancestors respectively. Merge commits use the first parent. |
| **Commit range** | Direct comparison between the base and target refs. |
| **History** | Working-tree sections, outgoing commits, and recent commits from `HEAD` in one list. |

The **Branch** and **Upstream** values in the repository information line are clickable. Hover to underline a value, then click it to choose a local or remote branch from a searchable, themed dialog. **Branch** selects the ref used in **History** and as the target of **Ready to push**; **Upstream** selects the default base for **Ready to push**. Choose **Current HEAD (default)** or **Configured upstream (default)** to reset. These selections last for the current session and never run `git checkout` or change Git configuration. **Staged** and **Unstaged** still show the actual working tree.

For **Ready to push**, leave the base field empty to use the configured upstream, or enter a branch/ref such as `main`. This mode uses locally available refs and does not fetch remote updates. If no upstream exists, enter a base explicitly. Its base value is stored separately from **Commit range**, so a range ref from another repository does not override the upstream default.

For **Single commit**, enter a SHA or ref in **Target / commit**. For **Commit range**, fill in both comparison fields. Click **Compare** after editing the fields.

In **Ready to push** and **Commit range**, **Changed files** groups files by commit. A horizontal line introduces each commit subject. For comparisons with multiple commits, another line after one empty row introduces **Summary**, which contains the combined diff. Selecting **Summary** shows the compared base and a one-line list of commit subjects with short hashes. A single-commit comparison omits this redundant summary. The status bar shows line and file counts for the selected commit group, or totals while **Summary** and its files are selected.

New untracked files are not included in the working-tree diff. They appear after you stage them using Git outside the application.

### Wide Diff layout

Click **Wide Diff** in the toolbar to show **Commits | Files | Commit message** above a full-width **Diff**. Click it again to return to the original layout. **Side-by-side** is a toggle button; its pressed state shows that Before and After are in separate columns. The square U+25D0 button switches themes, and the square U+29C9 button copies review comments. Hover over a toolbar button to see its purpose and any available hotkey. Switching layouts keeps the selected file and diff position without reloading Git data.

In History, the Commits panel starts with **Unstaged**, **Staged**, and **Ready to push**, followed by outgoing commits, a **History** divider, older commits in order, and **Load more** at the bottom. Selecting the History divider shows the number of loaded commits. Selecting a change section or commit fills the message and file panels and opens its first file, or restores the previously selected file and Diff position for that section or commit. Selecting a file updates Diff. The message panel shows the full commit message or section summary, followed by the file count and `-N +M` line totals. The Files panel uses the configured FileStatsMode. Each upper panel has its own scrollbar. **Load more** keeps the Commits panel at its current scroll position. **Refresh** clears the remembered per-commit file and Diff positions.

Drag either vertical divider between the upper panels or the horizontal divider above Diff to resize them. The chosen layout and the three divider sizes are saved in settings.ini as ExplorerLayout, ExplorerCommitWidth, ExplorerMessageWidth (the legacy name for the middle Files panel), and ExplorerTopHeight. The original FilePaneWidth is saved separately.

### Browse files and commits

Select a file under **Changed files** to open its diff. Long paths are aligned to the right so the filename remains visible. Hover over a file to see its full absolute path and `-N +M` line totals.

Right-click a file to select it and open **Copy File Name**. This copies the path relative to the repository root.

Unresolved conflicts in staged and unstaged comparisons appear with a `?` status and an explanatory message. Combined conflict diffs are not rendered; resolve the conflict using Git or a merge tool, then refresh.

Set `FileStatsMode` under `[GitDiffViewer]` in `settings.ini` to `none`, `bars`, `numbers`, or `auto` (the default). `auto` shows numbers when the file-list row is wider than forty `0` characters in the list font, and bars otherwise. Number columns show removed lines as red `-N` and added lines as green `+M`; their widths are calculated separately for each commit, section, and summary.

Bars have eight heights representing 1, 2, 3, 4–5, 6–9, 10–30, 31–100, and more than 100 lines. Zero-count bars are invisible; binary files have no textual line counts.

In the classic layout, select a commit heading to display its SHA, author, date, and full message without diff headers or a side-by-side divider. Message colors follow the selected theme. In **Single commit**, the same information appears in the first file-list entry, **`<<Commit Message>>`**.

In **History**, **Changed files** contains bold **Unstaged**, **Staged**, **Ready to push**, and **History** headings, with commits shown newest first. Section headings open a summary. Unstaged, Staged, and Ready to push headings show total removed and added lines on the right. Commit subjects align after the short hash and show commit totals on the right when the file list is wider than 60 `0` characters. Ready to push and History summaries include the full messages of their commits, separated by 80 underscore characters. Empty sections remain visible with an explanation, including repositories without an upstream. Commits listed under **Ready to push** are excluded from the general **History** section. The initial page contains 10 commits by default. Choose **Load more** (or focus it and press `Enter` or `Space`) to append 10 more; refresh starts from the current `HEAD` while retaining the expanded in-session limit. Untracked files are not included.

Use **Refresh** to reload repository changes. Refresh is manual. Review comments stay in memory until Refresh or application exit; they are not saved to settings. The export includes the file and line range, a representative source line, and the comment. For commit comparisons it also includes Branch, Hash, and Change-Id when present in the commit message.

While a Git operation is running, a moving highlight in the status-line background indicates activity.

Use **Full file** or press `F` to show every line of the selected changed text file instead of only the changed hunks and their surrounding context. Full context is requested lazily for that file and cached for the current refresh, so enabling the mode does not reload every file and commit. The vertical scrollbar shows removed changes in red and added changes in green. Toggle the mode again to return to the compact diff.

### Keyboard and mouse controls

| Input | Action |
| --- | --- |
| `F5` or `Ctrl+R` | Refresh Git data. |
| `Ctrl+Shift+D` | Toggle unified / side-by-side layout. |
| `F` while not editing a ref or using a dropdown | Toggle full-file context. |
| `Ctrl+Page Up` / `Ctrl+Page Down` | Go to the previous / next changed block or comment. |
| `C` with After lines selected in the diff, or double-click a comment | Add, edit, or delete a review comment. `Ctrl+Backspace` in its editor deletes the preceding word. |
| `F2` or **Copy comments** | Copy all session comments to the clipboard. |
| `Ctrl+Down` / `Ctrl+Up` | Select the next / previous item in **Changed files**, or in the active Commits or Files panel in Wide Diff. |
| `Space` with the classic file list, diff, or closed commit dropdown focused | Toggle the current commit message, restoring the previous file and scroll position on return. |
| `Enter` or `Space` on **Load more** | Append the next History page. |
| Middle click in the diff, then move up/down | Enable autoscroll. Distance from the click point controls speed. Click again or press Escape to stop. |
| `Ctrl+mouse wheel` over the diff | Change diff font size. |
| `Ctrl+-` / `Ctrl+=` | Decrease / increase diff font size. Numpad plus and minus also work. |
| Mouse drag in the diff | Select rows. |
| Mouse drag between the file list and diff | Resize the file list. |
| `Ctrl+K` or **U+1F4AC** | Toggle the current commit's comments view. |
| `F1` | Open the About dialog. |
| `Shift+click` | Extend the row selection. |
| `Ctrl+A` with the diff focused | Select all diff rows. |
| `Ctrl+A` with **Commit message** focused | Select the full message text. |
| `Ctrl+C` with the diff focused | Copy selected rows. A single row is copied without a trailing line break. |
| Arrow keys, `Page Up`, `Page Down`, `Home`, `End` | Navigate within the focused diff. |
| `Ctrl+Shift+S` | Save an application PNG through a file dialog. |

### Automation interface

The automation command `comment-add <first> <last> <text>` adds a session comment to the selected file for interface testing. State reports `commentCount`, `commentRows`, and visible comment text.

The automation command `layout panels` selects Wide Diff, while `layout classic` selects the original layout. Commands `select-explorer-group`, `select-explorer-file`, `explorer-splitter`, and `explorer-wheel` control the upper panels. State reports their rows, bounds, and message text.

Launch with `--automation-dir <directory>` to enable the local file-based automation protocol used by the interface tests. The `source` command accepts `history`, History rows report `section`, `notice`, `commit`, `file`, `spacer`, or `load-more` in `fileList`, and the `load-more` command activates the next page. `list-key enter` and `list-key space` exercise keyboard activation; `load-more-input mouse|enter|space` targets the paging row directly. Pagination keeps the selected item, diff position, and list scroll position while appending rows. Panel automation also exposes explorer-scrollbar, explorer-wheel, explorer-drag, explorer-hover-file, and explorer-message-select-all.
