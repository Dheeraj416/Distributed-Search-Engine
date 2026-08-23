// A small, fixed-size thread pool used in two places:
//   1. QueryEngine parallelizes BM25 scoring of candidate documents across
//      worker threads for large candidate sets.
//   2. The gRPC server's IndexBatch handler fans out indexing of a bulk
//      upload across workers instead of indexing strictly one document at
//      a time on the RPC thread.
//
// Deliberately hand-rolled rather than reaching for a library: it's ~80
// lines of standard <thread>/<mutex>/<condition_variable>, and being able
// to point at exactly what synchronizes with what is more valuable here
// than a dependency. See docs/DESIGN_DECISIONS.md.
#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace dse {

class ThreadPool {
 public:
  explicit ThreadPool(size_t num_threads);
  ~ThreadPool();

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  // Submits a callable and returns a future for its result. Throws
  // std::runtime_error if called after Shutdown()/destruction has begun.
  template <typename F, typename R = std::invoke_result_t<F>>
  std::future<R> Submit(F&& task) {
    auto packaged = std::make_shared<std::packaged_task<R()>>(std::forward<F>(task));
    std::future<R> future = packaged->get_future();
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) {
        throw std::runtime_error("ThreadPool::Submit called after shutdown");
      }
      tasks_.emplace([packaged]() { (*packaged)(); });
    }
    condition_.notify_one();
    return future;
  }

  size_t ThreadCount() const { return workers_.size(); }

 private:
  void WorkerLoop();

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable condition_;
  bool stopping_ = false;
};

}  // namespace dse
