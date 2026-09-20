#include "GitRepository.h"
#include "diff/UnifiedDiffParser.h"
#include <algorithm>
#include <cwctype>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
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
std::vector<Commit> parseCommits(const std::string &log)
{
  std::vector<Commit> commits;
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
    commits.push_back({fromUtf8(std::string_view(log).substr(p, a - p)), fromUtf8(std::string_view(log).substr(a + 1, b - a - 1)),
      fromUtf8(std::string_view(log).substr(b + 1, c - b - 1)), fromUtf8(std::string_view(log).substr(c + 1, d - c - 1))});
    p = d + 1;
  }
  return commits;
}
} // namespace
std::vector<Commit> GitRepository::findCommitsByPrefix(const std::wstring &directory, const std::wstring &prefix,
  const std::atomic_bool &cancel) const
{
  if (prefix.size() < 4 || prefix.size() > 64 ||
      !std::all_of(prefix.begin(), prefix.end(), [](wchar_t c) { return c < 128 && iswxdigit(c) != 0; }))
    throw std::invalid_argument("--hash requires 4 to 64 hexadecimal characters.");
  std::wstring needle = prefix;
  std::transform(needle.begin(), needle.end(), needle.begin(), towlower);
  GitClient git;
  auto currentBranch = trim(git.run(directory, {L"symbolic-ref", L"--quiet", L"--short", L"HEAD"}, cancel).out);
  if (cancel)
    throw std::runtime_error("Cancelled");

  auto candidates = git.run(directory, {L"rev-parse", L"--disambiguate=" + needle}, cancel);
  if (candidates.cancelled || cancel)
    throw std::runtime_error("Cancelled");
  if (candidates.exitCode)
    throw std::runtime_error(candidates.err.empty() ? "Unable to resolve commit hash" : candidates.err);
  std::vector<Commit> matches;
  std::istringstream input(candidates.out);
  std::string line;
  while (std::getline(input, line))
  {
    auto id = trim(line);
    if (id.empty())
      continue;
    auto commit = git.run(directory, {L"cat-file", L"-e", id + L"^{commit}"}, cancel);
    if (commit.cancelled || cancel)
      throw std::runtime_error("Cancelled");
    if (commit.exitCode)
      continue;
    auto refs = git.run(directory, {L"for-each-ref", L"--contains=" + id, L"--format=%(refname:short)", L"refs/heads", L"refs/remotes"}, cancel);
    if (refs.cancelled || cancel)
      throw std::runtime_error("Cancelled");
    if (refs.exitCode)
      throw std::runtime_error(refs.err.empty() ? "Unable to check commit refs" : refs.err);
    std::wstring branch;
    std::istringstream branchInput(refs.out);
    while (std::getline(branchInput, line))
    {
      auto name = trim(line);
      if (name.empty() || (name.size() >= 5 && name.compare(name.size() - 5, 5, L"/HEAD") == 0))
        continue;
      if (branch.empty())
        branch = name;
      if (name == currentBranch)
      {
        branch = name;
        break;
      }
    }
    if (branch.empty())
      continue;
    auto details = git.run(directory, {L"show", L"-s", L"--format=%H%x00%s%x00%an%x00%B%x00", id}, cancel);
    if (details.cancelled || cancel)
      throw std::runtime_error("Cancelled");
    if (details.exitCode)
      throw std::runtime_error(details.err.empty() ? "Unable to read commit details" : details.err);
    auto parsed = parseCommits(details.out);
    if (!parsed.empty())
    {
      parsed.front().branch = branch;
      matches.push_back(std::move(parsed.front()));
    }
  }
  return matches;
}
RepositorySnapshot GitRepository::load(const CompareRequest &request, const std::atomic_bool &cancel) const
{
  RepositorySnapshot snapshot;
  if (request.commitLookup)
  {
    snapshot.commits = findCommitsByPrefix(request.directory, request.commitPrefix, cancel);
    return snapshot;
  }
  GitClient git;
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
  auto commits = [&](const std::wstring &range, bool reverse = false, size_t skip = 0, size_t limit = 0, bool firstParent = false, bool ancestryPath = false) {
    std::vector<std::wstring> logArgs = {
      L"log", L"--encoding=UTF-8", L"--format=%H%x00%s%x00%an%x00%H%nAuthor: %an <%ae>%nDate: %aI%n%n%B%x00"};
    if (reverse)
      logArgs.push_back(L"--reverse");
    if (firstParent)
    if (ancestryPath)
      logArgs.push_back(L"--ancestry-path");
      logArgs.push_back(L"--first-parent");
    if (skip)
      logArgs.push_back(L"--skip=" + std::to_wstring(skip));
    if (limit)
      logArgs.push_back(L"-n" + std::to_wstring(limit));
    logArgs.insert(logArgs.end(), {range, L"--"});
    return parseCommits(run(logArgs).out);
  };
  std::vector<std::wstring> args = {L"diff", L"--no-color", L"--no-ext-diff", L"--no-textconv", L"--no-relative", L"--src-prefix=a/",
    L"--dst-prefix=b/", L"--find-renames", L"--submodule=short", L"--ignore-submodules=none",
    request.fullFile ? L"--unified=1000000" : L"--unified=3", L"--output-indicator-new=+", L"--output-indicator-old=-",
    L"--output-indicator-context= "};
  auto loadCommitDocuments = [&] {
    snapshot.commitDocuments.reserve(snapshot.commits.size());
    for (const auto &commit : snapshot.commits)
    {
      auto commitArgs = args;
      commitArgs[0] = L"show";
      commitArgs.insert(commitArgs.end(), {L"--format=", L"--root", L"--first-parent", commit.id, L"--"});
      snapshot.commitDocuments.push_back(UnifiedDiffParser{}.parse(run(commitArgs).out));
    }
  };
  auto commitDocument = [&](const Commit &commit) {
    auto commitArgs = args;
    commitArgs[0] = L"show";
    commitArgs.insert(commitArgs.end(), {L"--format=", L"--root", L"--first-parent", commit.id, L"--"});
    return UnifiedDiffParser{}.parse(run(commitArgs).out);
  };
  auto diffDocument = [&](bool staged) {
    auto diffArgs = args;
    if (staged)
      diffArgs.push_back(L"--cached");
    diffArgs.push_back(L"--");
    return UnifiedDiffParser{}.parse(run(diffArgs).out);
  };
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
      if (!request.selectedOnly)
      {
        snapshot.commits = commits(baseId + L".." + head, true);
        loadCommitDocuments();
      }
      args.push_back(baseId + L"..." + head);
      break;
    }
    case ChangeSource::Commit:
    {
      auto id = resolve(request.target);
      snapshot.commitId = id;
      if (!request.branch.empty())
        snapshot.branch = request.branch;
      if (!request.selectedOnly)
        snapshot.commitMessage =
          trim(run({L"log", L"-1", L"--encoding=UTF-8", L"--format=%H%nAuthor: %an <%ae>%nDate: %aI%n%n%B", id, L"--"}).out);
      if (!request.selectedOnly)
      {
        auto ancestors = commits(id, false, 0, 11, true);
        std::vector<Commit> descendants;
        if (snapshot.branch != L"Detached HEAD")
          descendants = commits(id + L".." + snapshot.branch, false, 0, 5, true, true);
        snapshot.commits = std::move(descendants);
        snapshot.commits.insert(snapshot.commits.end(), std::make_move_iterator(ancestors.begin()),
          std::make_move_iterator(ancestors.end()));
        loadCommitDocuments();
      }
      args[0] = L"show";
      args.insert(args.end(), {L"--format=", L"--root", L"--first-parent", id});
      snapshot.base = L"Commit " + request.target;
      break;
    }
    case ChangeSource::Range:
    {
      auto base = resolve(request.base), target = resolve(request.target);
      if (!request.selectedOnly)
      {
        snapshot.commits = commits(base + L".." + target, true);
        loadCommitDocuments();
      }
      args.push_back(base);
      args.push_back(target);
      snapshot.base = request.base + L" .. " + request.target;
      break;
    }
    case ChangeSource::History:
    {
      snapshot.base = L"History";
      auto headResult = run({L"rev-parse", L"--verify", L"HEAD"}, true);
      std::wstring currentHead = headResult.exitCode ? L"" : trim(headResult.out);
      std::wstring historyHead = request.historyAppend ? request.historyHead : currentHead;
      snapshot.history.initialHead = historyHead;

      if (!request.historyAppend)
      {
        snapshot.history.unstaged = diffDocument(false);
        snapshot.history.staged = diffDocument(true);
        if (snapshot.upstream.empty())
          snapshot.history.outgoingNotice = L"No upstream configured.";
        else if (!currentHead.empty())
        {
          auto upstream = resolve(snapshot.upstream);
          snapshot.history.outgoingCommits = commits(upstream + L".." + currentHead);
          auto outgoingArgs = args;
          outgoingArgs.push_back(upstream + L"..." + currentHead);
          outgoingArgs.push_back(L"--");
          snapshot.history.outgoing = UnifiedDiffParser{}.parse(run(outgoingArgs).out);
          snapshot.history.outgoingDocuments.reserve(snapshot.history.outgoingCommits.size());
          for (const auto &commit : snapshot.history.outgoingCommits)
            snapshot.history.outgoingDocuments.push_back(commitDocument(commit));
        }
      }

      if (!historyHead.empty())
      {
        size_t count = std::max<size_t>(1, request.historyLimit);
        std::unordered_set<std::wstring> excluded(request.historyExcludedCommits.begin(), request.historyExcludedCommits.end());
        if (!request.historyAppend)
          for (const auto &commit : snapshot.history.outgoingCommits)
            excluded.insert(commit.id);
        size_t rawSkip = request.historySkip;
        size_t nextSkip = rawSkip;
        const size_t batchSize = std::max<size_t>(64, count + excluded.size() + 1);
        while (snapshot.history.commits.size() <= count)
        {
          auto page = commits(historyHead, false, rawSkip, batchSize);
          if (page.empty())
            break;
          for (auto &commit : page)
          {
            ++rawSkip;
            if (excluded.find(commit.id) != excluded.end())
              continue;
            snapshot.history.commits.push_back(std::move(commit));
            if (snapshot.history.commits.size() == count)
              nextSkip = rawSkip;
            if (snapshot.history.commits.size() > count)
              break;
          }
          if (snapshot.history.commits.size() > count || page.size() < batchSize)
            break;
        }
        snapshot.history.hasMore = snapshot.history.commits.size() > count;
        if (snapshot.history.hasMore)
          snapshot.history.commits.resize(count);
        snapshot.history.nextSkip = snapshot.history.commits.size() == count ? nextSkip : rawSkip;
        snapshot.history.commitDocuments.reserve(snapshot.history.commits.size());
        for (const auto &commit : snapshot.history.commits)
          snapshot.history.commitDocuments.push_back(commitDocument(commit));
      }
      return snapshot;
    }
  }
  args.push_back(L"--");
  if (!request.path.empty())
    args.push_back(request.path);
  snapshot.document = UnifiedDiffParser{}.parse(run(args).out);
  if (!snapshot.document.warnings.empty())
    snapshot.notice = snapshot.document.warnings.front();
  return snapshot;
}
} // namespace gdv
