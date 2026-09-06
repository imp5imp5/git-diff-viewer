#include "RepositoryController.h"
#include "diff/UnifiedDiffParser.h"
namespace gdv
{
RepositoryController::RepositoryController(HWND window) : window_(window), worker_([this] { work(); }) {}
RepositoryController::~RepositoryController() { shutdown(); }
void RepositoryController::request(CompareRequest request)
{
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopping_)
    return;
  ++generation_;
  cancel_ = true;
  pending_ = std::move(request);
  result_.reset();
  wake_.notify_one();
}
std::optional<LoadResult> RepositoryController::takeResult()
{
  std::lock_guard<std::mutex> lock(mutex_);
  auto result = std::move(result_);
  result_.reset();
  return result;
}
void RepositoryController::shutdown()
{
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    cancel_ = true;
    pending_.reset();
    wake_.notify_one();
  }
  if (worker_.joinable())
    worker_.join();
}
void RepositoryController::work()
{
  for (;;)
  {
    LoadResult result;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wake_.wait(lock, [this] { return stopping_ || pending_.has_value(); });
      if (stopping_)
        return;
      result.request = std::move(*pending_);
      pending_.reset();
      result.generation = generation_;
      cancel_ = false;
    }
    try
    {
      result.snapshot = GitRepository{}.load(result.request, cancel_);
    }
    catch (const std::exception &e)
    {
      result.error = fromUtf8(e.what());
    }
    catch (...)
    {
      result.error = L"Unexpected error while loading the repository.";
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_)
        return;
      if (result.generation == generation_ && !cancel_)
      {
        result_ = std::move(result);
        // Posting and shutdown share the lock. The owner joins before destroying its HWND.
        if (window_)
          PostMessageW(window_, repositoryReady, 0, 0);
      }
    }
  }
}
} // namespace gdv
