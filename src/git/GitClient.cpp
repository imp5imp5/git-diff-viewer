#include "GitClient.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <windows.h>
namespace gdv
{
namespace
{
struct Handle
{
  HANDLE h{};
  ~Handle()
  {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  void close()
  {
    if (h && h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
    h = nullptr;
  }
};
std::runtime_error error(const char *what)
{
  return std::runtime_error(std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")");
}
std::wstring executablePath(const std::wstring &name)
{
  namespace fs = std::filesystem;
  if (fs::path(name).is_absolute())
    return name;
  // CreateProcess's implicit search includes the working tree. Only search
  // absolute PATH entries, so opening a repository cannot execute its git.exe.
  if (!fs::path(name).has_parent_path())
  {
    DWORD size = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    std::wstring value(size, L'\0');
    DWORD read = size ? GetEnvironmentVariableW(L"PATH", value.data(), size) : 0;
    if (read && read < size)
    {
      value.resize(read);
      std::wistringstream entries(value);
      std::wstring entry;
      while (std::getline(entries, entry, L';'))
      {
        if (entry.size() >= 2 && entry.front() == L'"' && entry.back() == L'"')
          entry = entry.substr(1, entry.size() - 2);
        if (!fs::path(entry).is_absolute())
          continue;
        auto candidate = fs::path(entry) / name;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec))
          return candidate.wstring();
      }
    }
  }
  throw std::runtime_error("Cannot find Git. Install Git for Windows and ensure git.exe is on an absolute PATH entry.");
}
} // namespace
std::wstring GitClient::quote(const std::wstring &arg)
{
  std::wstring out = L"\"";
  size_t slashes = 0;
  for (wchar_t c : arg)
  {
    if (c == L'\\')
    {
      ++slashes;
      continue;
    }
    out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    out += c;
  }
  out.append(slashes * 2, L'\\');
  out += L'"';
  return out;
}
GitResult GitClient::run(const std::wstring &directory, const std::vector<std::wstring> &args, const std::atomic_bool &cancel) const
{
  GitResult result;
  if (cancel)
  {
    result.cancelled = true;
    return result;
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  Handle outRead, outWrite, errRead, errWrite, input;
  if (!CreatePipe(&outRead.h, &outWrite.h, &sa, 0) || !CreatePipe(&errRead.h, &errWrite.h, &sa, 0))
    throw error("Cannot create Git pipes");
  if (!SetHandleInformation(outRead.h, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(errRead.h, HANDLE_FLAG_INHERIT, 0))
    throw error("Cannot protect Git pipes");
  input.h = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
  if (input.h == INVALID_HANDLE_VALUE)
    throw error("Cannot open Git stdin");
  STARTUPINFOEXW si{};
  si.StartupInfo.cb = sizeof(si);
  si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  si.StartupInfo.hStdOutput = outWrite.h;
  si.StartupInfo.hStdError = errWrite.h;
  si.StartupInfo.hStdInput = input.h;
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<unsigned char> storage(bytes);
  si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &bytes))
    throw error("Cannot initialize Git process attributes");
  struct Attributes
  {
    LPPROC_THREAD_ATTRIBUTE_LIST p;
    ~Attributes() { DeleteProcThreadAttributeList(p); }
  } attributes{si.lpAttributeList};
  HANDLE inherited[] = {outWrite.h, errWrite.h, input.h};
  if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr,
        nullptr))
    throw error("Cannot limit Git handle inheritance");
  const auto executable = executablePath(executable_);
  std::wstring command = quote(executable) + L" --no-pager -c color.ui=false -c core.quotepath=false -c core.pager=cat -c "
                                             L"core.fsmonitor=false -c log.showSignature=false -c diff.suppressBlankEmpty=false";
  for (const auto &arg : args)
    command += L" " + quote(arg);
  Handle job;
  job.h = CreateJobObjectW(nullptr, nullptr);
  if (!job.h)
    throw error("Cannot create Git job");
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job.h, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    throw error("Cannot configure Git cancellation");
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.empty() ? nullptr : directory.c_str(),
        &si.StartupInfo, &pi))
    throw error("Cannot start Git. Install Git for Windows and ensure git.exe is on PATH");
  Handle process{pi.hProcess}, thread{pi.hThread};
  if (!AssignProcessToJobObject(job.h, process.h))
  {
    TerminateProcess(process.h, 1);
    throw error("Cannot attach Git cancellation job");
  }
  if (ResumeThread(thread.h) == DWORD(-1))
    throw error("Cannot resume Git");
  outWrite.close();
  errWrite.close();
  input.close();
  auto start = std::chrono::steady_clock::now();
  auto drain = [&](Handle &pipe, std::string &dest) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe.h, nullptr, 0, nullptr, &available, nullptr))
    {
      if (GetLastError() == ERROR_BROKEN_PIPE)
        return false;
      throw error("Cannot read Git output");
    }
    if (available)
    {
      char buffer[32768];
      DWORD read = 0;
      if (!ReadFile(pipe.h, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr))
        throw error("Cannot read Git pipe");
      dest.append(buffer, read);
      if (result.out.size() + result.err.size() > 256ull * 1024 * 1024)
        throw std::runtime_error("Git output exceeds 256 MiB. Select a smaller commit range.");
    }
    return available != 0;
  };
  while (true)
  {
    if (cancel)
    {
      result.cancelled = true;
      TerminateJobObject(job.h, 1);
      break;
    }
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(120))
      throw std::runtime_error("Git timed out after 120 seconds. Try a smaller comparison.");
    bool a = drain(outRead, result.out), b = drain(errRead, result.err);
    if (WaitForSingleObject(process.h, 0) == WAIT_OBJECT_0 && !a && !b)
      break;
    if (!a && !b)
      WaitForSingleObject(process.h, 10);
  }
  WaitForSingleObject(process.h, INFINITE);
  GetExitCodeProcess(process.h, &result.exitCode);
  return result;
}
} // namespace gdv
