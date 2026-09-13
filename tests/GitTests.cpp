#include "app/RepositoryController.h"
#include "diff/UnifiedDiffParser.h"
#include "git/GitRepository.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <windows.h>
using namespace gdv;
namespace fs = std::filesystem;
void check(bool b, const char *text)
{
  if (!b)
    throw std::runtime_error(text);
}
int wmain(int argc, wchar_t **argv)
try
{
  if (fs::path(argv[0]).filename() == L"git.exe")
  {
    std::cout << "repository executable was launched";
    return 0;
  }
  // The process runner invokes this executable as a controlled pipe/cancellation fixture.
  for (int i = 1; i < argc; ++i)
  {
    if (std::wstring(argv[i]) == L"--fixture-flood")
    {
      std::cout << std::string(1024 * 1024, 'o') << std::flush;
      std::cerr << std::string(1024 * 1024, 'e') << std::flush;
      return 7;
    }
    if (std::wstring(argv[i]) == L"--fixture-slow")
    {
      Sleep(10000);
      return 0;
    }
    if (std::wstring(argv[i]) == L"--fixture-echo" && i + 1 < argc)
    {
      std::cout << toUtf8(argv[i + 1]);
      return 0;
    }
  }
  auto dir = fs::temp_directory_path() /
             (L"GitDiffViewer-test-Юникод " + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
  check(fs::create_directory(dir), "fixture directory must be newly created");
  struct Cleanup
  {
    fs::path p;
    ~Cleanup()
    {
      std::error_code e;
      fs::remove_all(p, e);
    }
  } cleanup{dir};
  std::atomic_bool cancel{false};
  GitClient git;
  auto run = [&](std::vector<std::wstring> args) {
    auto r = git.run(dir.wstring(), args, cancel);
    check(!r.exitCode, r.err.c_str());
    return r.out;
  };
  auto write = [&](const wchar_t *name, const std::string &data) {
    std::ofstream f(dir / name, std::ios::binary);
    f << data;
  };
  run({L"init", L"-b", L"main"});
  run({L"config", L"user.name", L"Fixture"});
  run({L"config", L"user.email", L"fixture@example.invalid"});
  run({L"config", L"core.autocrlf", L"false"});
  run({L"config", L"commit.gpgsign", L"false"});
  run({L"config", L"core.hooksPath", L".no-hooks"});
  write(L"staged.txt", "old\n");
  write(L"unstaged.txt", "old\n");
  write(L"удалить.txt", u8"Привет\n");
  write(L"rename old.txt", "rename\n");
  run({L"add", L"."});
  run({L"commit", L"-m", L"base"});
  run({L"checkout", L"-b", L"feature"});
  write(L"one.txt", "one\n");
  run({L"add", L"."});
  run({L"commit", L"-m", L"one"});
  write(L"two.txt", "two\n");
  run({L"add", L"."});
  run({L"commit", L"-m", L"two"});
  GitRepository repo;
  CompareRequest q{dir.wstring(), ChangeSource::ReadyToPush, {}, {}};
  check(!repo.load(q, cancel).notice.empty(), "no upstream message");
  q.base = L"main";
  auto ready = repo.load(q, cancel);
  check(ready.commits.size() == 2 && ready.document.files.size() == 2, "manual ready to push");
  run({L"branch", L"--set-upstream-to=main"});
  q.base.clear();
  check(repo.load(q, cancel).commits.size() == 2, "upstream");
  q.source = ChangeSource::Commit;
  q.target = ready.commits[0].id;
  check(repo.load(q, cancel).document.files.size() == 1, "single commit");
  auto commitDetails = repo.load(q, cancel);
  check(commitDetails.commitId == q.target && commitDetails.commitMessage.find(L"two") != std::wstring::npos,
    "single commit message loaded separately from patch");
  q.target = L"main";
  check(repo.load(q, cancel).document.files.size() == 4, "root commit");
  run({L"config", L"log.showRoot", L"false"});
  check(repo.load(q, cancel).document.files.size() == 4, "root commit ignores log.showRoot");
  q.source = ChangeSource::Range;
  q.base = L"main";
  q.target = L"HEAD";
  check(repo.load(q, cancel).document.files.size() == 2, "range");
  write(L"staged.txt", "staged\n");
  write(L"новый файл.txt", u8"Привет мир\n");
  write(L"empty.txt", "");
  write(L"binary.dat", std::string("a\0b", 3));
  run({L"mv", L"rename old.txt", L"renamed file.txt"});
  run({L"rm", L"удалить.txt"});
  run({L"add", L"."});
  write(L"unstaged.txt", "unstaged\n");
  write(L"untracked.txt", "not in git diff\n");
  q.source = ChangeSource::Staged;
  auto staged = repo.load(q, cancel);
  check(staged.document.files.size() == 6, "staged files");
  bool unicode = false, binary = false, rename = false, deleted = false;
  for (auto &f : staged.document.files)
  {
    unicode |= f.path() == L"новый файл.txt";
    binary |= f.binary;
    rename |= f.status == FileStatus::Renamed;
    deleted |= f.status == FileStatus::Deleted;
  }
  check(unicode && binary && rename && deleted, "special files");
  q.source = ChangeSource::Unstaged;
  auto unstaged = repo.load(q, cancel);
  check(unstaged.document.files.size() == 1 && unstaged.document.files[0].path() == L"unstaged.txt", "unstaged isolation");
  q.source = ChangeSource::Head;
  check(repo.load(q, cancel).document.files.size() == 7, "HEAD union");
  run({L"config", L"diff.outputIndicatorNew", L">"});
  run({L"config", L"diff.outputIndicatorOld", L"<"});
  check(repo.load(q, cancel).document.warnings.empty(), "custom diff indicators overridden");
  write(L"unstaged.txt", "old\n\nlast\n");
  run({L"add", L"unstaged.txt"});
  write(L"unstaged.txt", "old\n\nchanged\n");
  run({L"config", L"diff.suppressBlankEmpty", L"true"});
  auto blank = repo.load({dir.wstring(), ChangeSource::Unstaged, {}, {}}, cancel);
  check(blank.document.warnings.empty() && blank.document.files.size() == 1 && blank.document.files[0].hunks[0].lines.size() == 4 &&
          blank.document.files[0].hunks[0].lines[1].text.empty(),
    "empty context lines survive custom Git configuration");
  std::string fullContents;
  for (int i = 1; i <= 20; ++i)
    fullContents += "line " + std::to_string(i) + "\n";
  write(L"unstaged.txt", fullContents);
  run({L"add", L"unstaged.txt"});
  auto changedContents = fullContents;
  changedContents.replace(changedContents.find("line 2\n"), 7, "changed 2\n");
  changedContents.replace(changedContents.find("line 11\n"), 8, "changed\n");
  changedContents.replace(changedContents.find("line 19\n"), 8, "changed 19\n");
  write(L"unstaged.txt", changedContents);
  CompareRequest contextRequest{dir.wstring(), ChangeSource::Unstaged, {}, {}};
  auto compactContext = repo.load(contextRequest, cancel);
  contextRequest.fullFile = true;
  auto fullContext = repo.load(contextRequest, cancel);
  const auto &compactLines = compactContext.document.files[0].hunks[0].lines;
  const auto &fullLines = fullContext.document.files[0].hunks[0].lines;
  check(fullContext.document.warnings.empty() && fullContext.document.files[0].hunks.size() == 1 &&
          fullLines.size() > compactLines.size() && fullLines.front().oldLine.value_or(0) == 1 &&
          fullLines.back().oldLine.value_or(0) == 20,
    "full-file mode merges distant changes and includes the first through the last line");
  {
    RepositoryController controller(nullptr);
    for (int i = 0; i < 30; ++i)
    {
      auto request = q;
      request.source = i % 2 ? ChangeSource::Staged : ChangeSource::Unstaged;
      controller.request(request);
    }
    controller.request(q);
    std::optional<LoadResult> completed;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completed && std::chrono::steady_clock::now() < deadline)
    {
      completed = controller.takeResult();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(completed && completed->error.empty() && completed->request.source == ChangeSource::Head &&
            completed->snapshot.document.files.size() == 7,
      "controller latest request wins");
    controller.request(q);
    controller.shutdown();
    check(!controller.takeResult(), "controller shutdown discards active work");
  }
  {
    auto unborn = dir / L"unborn";
    fs::create_directory(unborn);
    check(git.run(unborn.wstring(), {L"init", L"-b", L"main"}, cancel).exitCode == 0, "unborn init");
    std::ofstream(unborn / L"initial.txt") << "staged\n";
    check(git.run(unborn.wstring(), {L"add", L"."}, cancel).exitCode == 0, "unborn add");
    std::ofstream(unborn / L"initial.txt") << "working\n";
    auto initial = repo.load({unborn.wstring(), ChangeSource::Head, {}, {}}, cancel);
    check(initial.document.files.size() == 1 && initial.document.files[0].hunks[0].lines[0].text == L"working",
      "unborn HEAD includes working tree edits");
  }
  {
    auto conflict = dir / L"conflict";
    fs::create_directory(conflict);
    auto local = [&](std::vector<std::wstring> args) {
      auto result = git.run(conflict.wstring(), args, cancel);
      check(result.exitCode == 0, result.err.c_str());
    };
    local({L"init", L"-b", L"main"});
    local({L"config", L"user.name", L"Fixture"});
    local({L"config", L"user.email", L"fixture@example.invalid"});
    local({L"config", L"commit.gpgsign", L"false"});
    local({L"config", L"core.hooksPath", L".no-hooks"});
    auto commit = [&](const char *content) {
      std::ofstream(conflict / L"conflict file.txt") << content << '\n';
      local({L"add", L"."});
      local({L"commit", L"-m", L"fixture"});
    };
    commit("base");
    local({L"checkout", L"-b", L"other"});
    commit("other");
    local({L"checkout", L"main"});
    commit("main");
    check(git.run(conflict.wstring(), {L"merge", L"other"}, cancel).exitCode != 0, "fixture has a merge conflict");
    for (auto mode : {ChangeSource::Staged, ChangeSource::Unstaged})
    {
      auto result = repo.load({conflict.wstring(), mode, {}, {}}, cancel);
      check(result.document.files.size() == 1 && result.document.files[0].path() == L"conflict file.txt" &&
              result.document.files[0].status == FileStatus::Unknown && !result.document.files[0].metadata.empty() &&
              !result.notice.empty(),
        "real merge conflicts are visible in staged and unstaged comparisons");
    }
  }
  auto quoted = run({L"-c", L"test.quoted=spaces \"quotes\" end\\", L"config", L"--get", L"test.quoted"});
  check(quoted == "spaces \"quotes\" end\\\n", "argument quoting");
  check(git.run(dir.wstring(), {L"invalid-command"}, cancel).exitCode != 0, "stderr exit code");
  bool missing = false;
  try
  {
    GitClient(L"missing-git-fixture.exe").run(dir.wstring(), {}, cancel);
  }
  catch (...)
  {
    missing = true;
  }
  check(missing, "missing Git");
  wchar_t self[MAX_PATH]{};
  GetModuleFileNameW(nullptr, self, MAX_PATH);
  {
    struct RestoreDirectory
    {
      fs::path original{fs::current_path()};
      ~RestoreDirectory() { fs::current_path(original); }
    } restore;
    fs::copy_file(self, dir / L"git.exe");
    fs::current_path(dir);
    auto version = git.run(dir.wstring(), {L"--version"}, cancel);
    check(version.exitCode == 0 && version.out.find("git version ") == 0, "repository git.exe must not shadow Git from PATH");
  }
  GitClient fixture(self);
  auto flooded = fixture.run(dir.wstring(), {L"--fixture-flood"}, cancel);
  check(flooded.exitCode == 7 && flooded.out.size() == 1024 * 1024 && flooded.err.size() == 1024 * 1024,
    "stdout/stderr drain without deadlock");
  for (auto arg : {L"", L"Юникод and spaces", L"a\"b", L"ends with \\", L"a\\\\\"b", L"& | % ;"})
  {
    auto echo = fixture.run(dir.wstring(), {L"--fixture-echo", arg}, cancel);
    check(echo.out == toUtf8(arg), "roundtrip process arguments");
  }
  auto started = std::chrono::steady_clock::now();
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    cancel = true;
  });
  GitResult cancelled;
  try
  {
    cancelled = fixture.run(dir.wstring(), {L"--fixture-slow"}, cancel);
  }
  catch (...)
  {
    canceller.join();
    throw;
  }
  canceller.join();
  check(cancelled.cancelled && std::chrono::steady_clock::now() - started < std::chrono::seconds(3), "active process cancellation");
  check(git.run(dir.wstring(), {L"status"}, cancel).cancelled, "pre-cancellation");
  std::cout << "Git integration: sources, root commit, Unicode, statuses, errors and quoting passed\n";
  return 0;
}
catch (const std::exception &e)
{
  std::cerr << e.what() << '\n';
  return 1;
}
