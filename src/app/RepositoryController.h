#pragma once
#include "git/GitRepository.h"
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <windows.h>
namespace gdv
{
constexpr UINT repositoryReady = WM_APP + 1;
struct LoadResult
{
  std::uint64_t generation{};
  CompareRequest request;
  RepositorySnapshot snapshot;
  std::wstring error;
};
class RepositoryController
{
public:
  explicit RepositoryController(HWND window);
  ~RepositoryController();
  void request(CompareRequest request);
  std::optional<LoadResult> takeResult();
  void shutdown();

private:
  void work();
  HWND window_{};
  std::mutex mutex_;
  std::condition_variable wake_;
  std::optional<CompareRequest> pending_;
  std::optional<LoadResult> result_;
  std::uint64_t generation_{};
  bool stopping_{};
  std::atomic_bool cancel_{false};
  std::thread worker_;
};
} // namespace gdv
