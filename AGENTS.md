@README.md

Preferences are saved on close under:

```text
HKEY_CURRENT_USER\Software\gaijin\git_diff_viewer
```

Saved values include normal window position and size, maximized state, comparison source, diff layout, font size, base and target refs, the selected local commit SHA, and theme. The repository itself is chosen from the launch directory or command-line argument.

## Tests

Build first, then run:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

The suite includes diff parser/presentation tests, Git integration tests, and application interface tests. Repository tests use temporary fixtures. Interface tests launch the application through its opt-in automation interface and normally write PNG artifacts beneath `build\interface-Release`.

To skip PNG capture in interface tests:

```powershell
$env:GFD_SKIP_SCREENSHOTS = '1'
ctest --test-dir build -C Release --output-on-failure
Remove-Item Env:\GFD_SKIP_SCREENSHOTS
```

Automated interface tests do not replace visual checks of native controls, clipboard interaction, or DPI changes between monitors.

## Development

| Directory | Contents |
| --- | --- |
| `src/diff` | Unified diff model, parser, and presentation builder. |
| `src/git` | Git process execution and repository comparisons. |
| `src/app` | Background repository loading and cancellation. |
| `src/ui` | Win32 window, diff rendering, theme palettes, screenshots, and automation. |
| `resources` | Windows application resources and manifest. |
| `tests` | Core, integration, and interface tests. |
| `tools` | Temporary repository generator and automation client. |

Theme palettes are indexed by the `ThemeColor` enum in `src/ui/Theme.h`.

### Formatting

Use **clang-format 18.1.8** with the repository's `.clang-format`:

```powershell
$files = Get-ChildItem src, tests -Recurse -File -Include *.cpp, *.h, *.hpp
$files | ForEach-Object { & clang-format -i --style=file $_.FullName }
```

On a workstation with the Gaijin toolchain, the formatter is available at:

```powershell
$formatter = Join-Path $env:GDEVTOOL 'LLVM-18.1.8\bin\clang-format.exe'
$files | ForEach-Object { & $formatter -i --style=file $_.FullName }
```

### Application automation

Pass `--automation-dir <directory>` to enable a local file-based command interface for tests and tooling. Use a separate directory per application instance and one client per directory. Automation sessions do not read or write user registry settings.

Example, from the project directory:

```powershell
$fixture = & .\tools\create_test_repo.ps1 -Large
$session = Join-Path $env:TEMP ('gfd-session-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $session | Out-Null
$exe = (Resolve-Path .\build\Release\gfd.exe).Path
Start-Process -FilePath $exe -ArgumentList ('"' + $fixture + '" --automation-dir "' + $session + '"')

do {
    $state = & .\tools\app_command.ps1 -Directory $session -Command state
} while ($state.loading)

& .\tools\app_command.ps1 -Directory $session -Command view -Arguments 'side-by-side'
& .\tools\app_command.ps1 -Directory $session -Command close
```

Supported commands include `state`, `open`, `source`, `view`, `full-file`, `theme`, `base`, `target`, `compare`, `refresh`, `select-file`, `select-commit`, `hover-commit`, `end-hover`, `scroll`, `scrollbar`, `navigate-change`, `zoom`, `ctrl-wheel`, `key`, `resize`, `splitter`, `screenshot`, `screenshot-commits`, and `close`. See `src/ui/Automation.cpp` and `tests/InterfaceTests.ps1` for argument handling and examples.

Git loading is asynchronous. After changing the repository, source, or commit, poll `state` until `loading` is false before selecting files. Screenshot arguments are PNG filenames within the session directory.

The underlying protocol uses a UTF-8 `request.txt`: a unique request ID on the first line, the command on the second, then one argument per line. The application publishes `response.json` with `id`, `ok`, `error`, and `state`. The supplied PowerShell client handles request publication and response matching. No network port is opened.

## Current scope

GitDiffViewer is a local review tool. It does not provide file editing, staging, merge-conflict resolution, syntax highlighting, word-level diffs, image previews, or remote code-review integration.
