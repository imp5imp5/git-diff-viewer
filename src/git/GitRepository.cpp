#include "GitRepository.h"
#include "diff/UnifiedDiffParser.h"
#include <stdexcept>
namespace gdv
{
namespace
{
std::wstring trim(std::string s)
{
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  return fromUtf8(s);
}
} // namespace
RepositorySnapshot GitRepository::load(const CompareRequest &request, const std::atomic_bool &cancel) const
{
  GitClient git;
  RepositorySnapshot snapshot;
  std::wstring cwd = request.directory;
  auto run = [&](const std::vector<std::wstring> &args, bool optional = false) {
    auto r = git.run(cwd, args, cancel);
    if (r.cancelled || cancel)
      throw std::runtime_error("Cancelled");
    if (r.exitCode && !optional)
      throw std::runtime_error(r.err.empty() ? "Git command failed" : r.err);
    return r;
  };
  snapshot.root = trim(run({L"rev-parse", L"--show-toplevel"}).out);
  cwd = snapshot.root;
  snapshot.branch = trim(run({L"symbolic-ref", L"--quiet", L"--short", L"HEAD"}, true).out);
  if (snapshot.branch.empty())
    snapshot.branch = L"Detached HEAD";
  snapshot.upstream = trim(run({L"rev-parse", L"--abbrev-ref", L"--symbolic-full-name", L"@{upstream}"}, true).out);
  auto resolve = [&](const std::wstring &ref) {
    if (ref.empty())
      throw std::runtime_error("Enter a commit or branch in the comparison fields, then click Compare.");
    return trim(run({L"rev-parse", L"--verify", L"--end-of-options", ref + L"^{commit}"}).out);
  };
  std::vector<std::wstring> args = {L"diff", L"--no-color", L"--no-ext-diff", L"--no-textconv", L"--no-relative", L"--src-prefix=a/",
    L"--dst-prefix=b/", L"--find-renames", L"--submodule=short", L"--ignore-submodules=none", L"--unified=3",
    L"--output-indicator-new=+", L"--output-indicator-old=-", L"--output-indicator-context= "};
  switch (request.source)
  {
    case ChangeSource::Staged:
      args.push_back(L"--cached");
      snapshot.base = L"Index vs HEAD";
      break;
    case ChangeSource::Unstaged: snapshot.base = L"Working tree vs index"; break;
    case ChangeSource::Head:
    {
      auto head = run({L"rev-parse", L"--verify", L"HEAD"}, true);
      if (head.exitCode)
      {
        // Compute the repository's empty-tree ID (also works for SHA-256 repositories).
        // stdin is NUL and hash-object has no -w, so this does not write an object.
        args.push_back(trim(run({L"hash-object", L"-t", L"tree", L"--stdin"}).out));
        snapshot.notice = L"No HEAD yet — comparing tracked files with the empty tree.";
      }
      else
        args.push_back(trim(head.out));
      snapshot.base = L"Working tree vs HEAD";
      break;
    }
    case ChangeSource::ReadyToPush:
    {
      auto base = request.base.empty() ? snapshot.upstream : request.base;
      if (base.empty())
      {
        snapshot.notice = L"No upstream configured. Enter a base branch (for example main) and click Compare.";
        return snapshot;
      }
      auto baseId = resolve(base), head = resolve(L"HEAD");
      snapshot.base = base;
      auto log = run({L"log", L"--encoding=UTF-8", L"--format=%H%x00%s%x00%an%x00%H%nAuthor: %an <%ae>%nDate: %aI%n%n%B%x00",
                       baseId + L".." + head, L"--"})
                   .out;
      size_t p = 0;
      while (p < log.size())
      {
        while (p < log.size() && (log[p] == '\n' || log[p] == '\r'))
          ++p;
        size_t a = log.find('\0', p);
        if (a == std::string::npos)
          break;
        size_t b = log.find('\0', a + 1);
        if (b == std::string::npos)
          break;
        size_t c = log.find('\0', b + 1);
        if (c == std::string::npos)
          break;
        size_t d = log.find('\0', c + 1);
        if (d == std::string::npos)
          break;
        snapshot.commits.push_back(
          {fromUtf8(std::string_view(log).substr(p, a - p)), fromUtf8(std::string_view(log).substr(a + 1, b - a - 1)),
            fromUtf8(std::string_view(log).substr(b + 1, c - b - 1)), fromUtf8(std::string_view(log).substr(c + 1, d - c - 1))});
        p = d + 1;
      }
      args.push_back(baseId + L"..." + head);
      break;
    }
    case ChangeSource::Commit:
    {
      auto id = resolve(request.target);
      snapshot.commitId = id;
      snapshot.commitMessage =
        trim(run({L"log", L"-1", L"--encoding=UTF-8", L"--format=%H%nAuthor: %an <%ae>%nDate: %aI%n%n%B", id, L"--"}).out);
      args[0] = L"show";
      args.insert(args.end(), {L"--format=", L"--root", L"--first-parent", id});
      snapshot.base = L"Commit " + request.target;
      break;
    }
    case ChangeSource::Range:
    {
      auto base = resolve(request.base), target = resolve(request.target);
      args.push_back(base);
      args.push_back(target);
      snapshot.base = request.base + L" .. " + request.target;
      break;
    }
  }
  args.push_back(L"--");
  snapshot.document = UnifiedDiffParser{}.parse(run(args).out);
  if (!snapshot.document.warnings.empty())
    snapshot.notice = snapshot.document.warnings.front();
  return snapshot;
}
} // namespace gdv
