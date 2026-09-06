#include "UnifiedDiffParser.h"
#include <limits>
namespace gdv
{
std::wstring fromUtf8(std::string_view text)
{
  std::wstring out;
  for (size_t i = 0; i < text.size();)
  {
    auto c = static_cast<unsigned char>(text[i++]);
    unsigned cp = c;
    int extra = 0;
    unsigned minimum = 0;
    if (c >= 0xc2 && c <= 0xdf)
    {
      cp = c & 31;
      extra = 1;
      minimum = 0x80;
    }
    else if (c >= 0xe0 && c <= 0xef)
    {
      cp = c & 15;
      extra = 2;
      minimum = 0x800;
    }
    else if (c >= 0xf0 && c <= 0xf4)
    {
      cp = c & 7;
      extra = 3;
      minimum = 0x10000;
    }
    else if (c >= 0x80)
    {
      out += wchar_t(0xfffd);
      continue;
    }
    size_t start = i;
    bool valid = true;
    for (int n = 0; n < extra; ++n)
    {
      if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xc0) != 0x80)
      {
        valid = false;
        break;
      }
      cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 63);
    }
    if (!valid || cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
    {
      i = start;
      out += wchar_t(0xfffd);
      continue;
    }
    if constexpr (sizeof(wchar_t) == 2)
    {
      if (cp > 0xffff)
      {
        cp -= 0x10000;
        out += wchar_t(0xd800 + (cp >> 10));
        out += wchar_t(0xdc00 + (cp & 1023));
      }
      else
        out += static_cast<wchar_t>(cp);
    }
    else
      out += static_cast<wchar_t>(cp);
  }
  return out;
}
std::string toUtf8(const std::wstring &text)
{
  std::string out;
  for (size_t i = 0; i < text.size(); ++i)
  {
    unsigned cp = static_cast<unsigned>(text[i]);
    if constexpr (sizeof(wchar_t) == 2)
    {
      if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < text.size() && text[i + 1] >= 0xdc00 && text[i + 1] <= 0xdfff)
      {
        cp = 0x10000 + ((cp - 0xd800) << 10) + (static_cast<unsigned>(text[++i]) - 0xdc00);
      }
    }
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      cp = 0xfffd;
    if (cp < 0x80)
      out += static_cast<char>(cp);
    else if (cp < 0x800)
    {
      out += static_cast<char>(0xc0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 63));
    }
    else if (cp < 0x10000)
    {
      out += static_cast<char>(0xe0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 63));
      out += static_cast<char>(0x80 | (cp & 63));
    }
    else
    {
      out += static_cast<char>(0xf0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 63));
      out += static_cast<char>(0x80 | ((cp >> 6) & 63));
      out += static_cast<char>(0x80 | (cp & 63));
    }
  }
  return out;
}
namespace
{
bool starts(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }
std::string unquote(std::string_view s, size_t *consumed = nullptr)
{
  if (s.empty() || s.front() != '"')
  {
    if (consumed)
      *consumed = s.size();
    return std::string(s);
  }
  std::string out;
  size_t i = 1;
  while (i < s.size())
  {
    char c = s[i++];
    if (c == '"')
      break;
    if (c == '\\' && i < s.size())
    {
      c = s[i++];
      if (c >= '0' && c <= '7')
      {
        unsigned v = c - '0';
        for (int n = 1; n < 3 && i < s.size() && s[i] >= '0' && s[i] <= '7'; ++n)
          v = v * 8 + (s[i++] - '0');
        c = static_cast<char>(v);
      }
      else
      {
        switch (c)
        {
          case 't': c = '\t'; break;
          case 'n': c = '\n'; break;
          case 'r': c = '\r'; break;
          case 'b': c = '\b'; break;
          case 'f': c = '\f'; break;
          case 'v': c = '\v'; break;
          case 'a': c = '\a'; break;
        }
      }
    }
    out += c;
  }
  if (consumed)
    *consumed = i;
  return out;
}
std::wstring path(std::string_view s, bool prefix)
{
  auto raw = unquote(s);
  if (raw == "/dev/null")
    return {};
  if (prefix && raw.size() > 2 && raw[1] == '/' && (raw[0] == 'a' || raw[0] == 'b'))
    raw.erase(0, 2);
  if (!s.empty() && s.front() != '"' && !raw.empty() && raw.back() == '\t')
    raw.pop_back();
  return fromUtf8(raw);
}
bool number(std::string_view s, size_t &pos, int &value)
{
  if (pos >= s.size() || s[pos] < '0' || s[pos] > '9')
    return false;
  value = 0;
  while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9')
  {
    int digit = s[pos++] - '0';
    if (value > (std::numeric_limits<int>::max() - digit) / 10)
      return false;
    value = value * 10 + digit;
  }
  return true;
}
bool range(std::string_view s, size_t &pos, char sign, int &start, int &count)
{
  if (pos >= s.size() || s[pos++] != sign || !number(s, pos, start))
    return false;
  count = 1;
  if (pos < s.size() && s[pos] == ',')
  {
    ++pos;
    if (!number(s, pos, count))
      return false;
  }
  return start <= std::numeric_limits<int>::max() - count;
}
} // namespace
DiffDocument UnifiedDiffParser::parse(std::string_view patch) const
{
  DiffDocument doc;
  FileDiff *file = nullptr;
  DiffHunk *hunk = nullptr;
  int old = 0, next = 0, oldRemaining = 0, newRemaining = 0;
  bool combined = false;
  auto finish = [&] {
    if (hunk && (oldRemaining || newRemaining))
      doc.warnings.push_back(L"Incomplete hunk in " + file->path());
    hunk = nullptr;
  };
  size_t cursor = 0;
  while (cursor < patch.size())
  {
    size_t end = patch.find('\n', cursor);
    if (end == std::string_view::npos)
      end = patch.size();
    auto s = patch.substr(cursor, end - cursor);
    cursor = end + 1;
    if (starts(s, "diff --cc ") || starts(s, "diff --combined ") || starts(s, "* Unmerged path "))
    {
      finish();
      doc.files.emplace_back();
      file = &doc.files.back();
      file->newPath = path(s.substr(starts(s, "diff --cc ") ? 10 : 16), false);
      file->status = FileStatus::Unknown;
      file->metadata.push_back(L"Unresolved merge conflict. Conflict diff is not supported yet.");
      doc.warnings.push_back(L"Unresolved merge conflict in " + file->path());
      combined = true;
      continue;
    }
    if (starts(s, "diff --git "))
    {
      finish();
      combined = false;
      doc.files.emplace_back();
      file = &doc.files.back();
      auto names = s.substr(11);
      size_t split = std::string_view::npos;
      if (!names.empty() && names.front() == '"')
      {
        size_t length = 0;
        unquote(names, &length);
        split = length;
      }
      else
      {
        split = names.find(" b/");
        if (split == std::string_view::npos)
          split = names.find(" \"b/");
        for (size_t p = names.find(" b/"); p != std::string_view::npos; p = names.find(" b/", p + 1))
        {
          if (p >= 2 && names.substr(2, p - 2) == names.substr(p + 3))
          {
            split = p;
            break;
          }
        }
      }
      if (split != std::string_view::npos && split < names.size())
      {
        file->oldPath = path(names.substr(0, split), true);
        file->newPath = path(names.substr(split + 1), true);
      }
      else
      {
        file->newPath = L"(unrecognized path)";
        doc.warnings.push_back(L"Malformed file header");
      }
      continue;
    }
    if (!file || combined)
      continue;
    if (starts(s, "@@ "))
    {
      finish();
      DiffHunk candidate;
      size_t p = 3;
      bool ok = range(s, p, '-', candidate.oldStart, candidate.oldCount);
      ok = ok && p < s.size() && s[p++] == ' ' && range(s, p, '+', candidate.newStart, candidate.newCount);
      ok = ok && s.substr(p, 3) == " @@";
      if (!ok)
      {
        doc.warnings.push_back(L"Malformed hunk in " + file->path());
        continue;
      }
      candidate.header = fromUtf8(s);
      file->hunks.push_back(std::move(candidate));
      hunk = &file->hunks.back();
      old = hunk->oldStart;
      next = hunk->newStart;
      oldRemaining = hunk->oldCount;
      newRemaining = hunk->newCount;
      continue;
    }
    if (hunk && starts(s, "\\ No newline"))
    {
      hunk->lines.push_back({DiffLineType::Meta, {}, {}, fromUtf8(s)});
      continue;
    }
    if (hunk && (oldRemaining || newRemaining))
    {
      if (s.empty())
      {
        doc.warnings.push_back(L"Malformed empty diff line");
        continue;
      }
      DiffLine line;
      line.text = fromUtf8(s.substr(1));
      if (s[0] == ' ' && oldRemaining > 0 && newRemaining > 0)
      {
        line.type = DiffLineType::Context;
        line.oldLine = old++;
        line.newLine = next++;
        --oldRemaining;
        --newRemaining;
      }
      else if (s[0] == '-' && oldRemaining > 0)
      {
        line.type = DiffLineType::Removed;
        line.oldLine = old++;
        --oldRemaining;
      }
      else if (s[0] == '+' && newRemaining > 0)
      {
        line.type = DiffLineType::Added;
        line.newLine = next++;
        --newRemaining;
      }
      else
      {
        doc.warnings.push_back(L"Unexpected hunk content in " + file->path());
        finish();
        continue;
      }
      hunk->lines.push_back(std::move(line));
      continue;
    }
    if (starts(s, "--- "))
      file->oldPath = path(s.substr(4), true);
    else if (starts(s, "+++ "))
      file->newPath = path(s.substr(4), true);
    else if (starts(s, "new file mode "))
    {
      file->status = FileStatus::Added;
      file->metadata.push_back(fromUtf8(s));
    }
    else if (starts(s, "deleted file mode "))
    {
      file->status = FileStatus::Deleted;
      file->metadata.push_back(fromUtf8(s));
    }
    else if (starts(s, "rename from "))
    {
      file->oldPath = path(s.substr(12), false);
      file->status = FileStatus::Renamed;
      file->metadata.push_back(fromUtf8(s));
    }
    else if (starts(s, "rename to "))
    {
      file->newPath = path(s.substr(10), false);
      file->status = FileStatus::Renamed;
      file->metadata.push_back(fromUtf8(s));
    }
    else if (starts(s, "copy from "))
    {
      file->oldPath = path(s.substr(10), false);
      file->status = FileStatus::Copied;
      file->metadata.push_back(fromUtf8(s));
    }
    else if (starts(s, "copy to "))
    {
      file->newPath = path(s.substr(8), false);
      file->status = FileStatus::Copied;
      file->metadata.push_back(fromUtf8(s));
    }
    else if (starts(s, "Binary files ") || starts(s, "GIT binary patch"))
    {
      file->binary = true;
      if (file->status == FileStatus::Modified)
        file->status = FileStatus::Binary;
      file->metadata.push_back(L"Binary file — text diff is unavailable.");
    }
    else if (starts(s, "old mode ") || starts(s, "new mode ") || starts(s, "similarity index ") || starts(s, "dissimilarity index "))
      file->metadata.push_back(fromUtf8(s));
  }
  finish();
  return doc;
}
} // namespace gdv
