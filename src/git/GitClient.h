#pragma once
#include <atomic>
#include <string>
#include <vector>
namespace gdv
{
struct GitResult
{
  unsigned long exitCode{};
  std::string out, err;
  bool cancelled{};
};
class GitClient
{
public:
  explicit GitClient(std::wstring executable = L"git.exe") : executable_(std::move(executable)) {}
  GitResult run(const std::wstring &directory, const std::vector<std::wstring> &args, const std::atomic_bool &cancel) const;
  static std::wstring quote(const std::wstring &arg);

private:
  std::wstring executable_;
};
} // namespace gdv
