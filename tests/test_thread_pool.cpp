#include "dse/thread_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace dse {
namespace {

TEST(ThreadPoolTest, ThreadCountMatchesConstructorArgument) {
  ThreadPool pool(3);
  EXPECT_EQ(pool.ThreadCount(), 3u);
}

TEST(ThreadPoolTest, ZeroThreadsIsClampedToOne) {
  ThreadPool pool(0);
  EXPECT_EQ(pool.ThreadCount(), 1u);
}

TEST(ThreadPoolTest, SubmitReturnsCorrectResultThroughFuture) {
  ThreadPool pool(2);
  auto future = pool.Submit([]() -> int { return 21 * 2; });
  EXPECT_EQ(future.get(), 42);
}

TEST(ThreadPoolTest, ManyTasksAllCompleteExactlyOnce) {
  ThreadPool pool(4);
  constexpr int kNumTasks = 500;
  std::atomic<int> counter{0};

  std::vector<std::future<void>> futures;
  futures.reserve(kNumTasks);
  for (int i = 0; i < kNumTasks; ++i) {
    futures.push_back(pool.Submit([&counter]() -> void { counter.fetch_add(1); }));
  }
  for (auto& f : futures) f.get();

  EXPECT_EQ(counter.load(), kNumTasks);
}

TEST(ThreadPoolTest, TasksActuallyRunConcurrentlyAcrossMultipleThreads) {
  // Submit tasks that each record which worker they ran on (via a
  // thread-local id) — if the pool has more than one worker, distinct
  // task invocations should observe more than one unique thread id.
  ThreadPool pool(4);
  std::mutex ids_mutex;
  std::vector<std::thread::id> observed_ids;

  std::vector<std::future<void>> futures;
  for (int i = 0; i < 50; ++i) {
    futures.push_back(pool.Submit([&]() -> void {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      std::lock_guard<std::mutex> lock(ids_mutex);
      observed_ids.push_back(std::this_thread::get_id());
    }));
  }
  for (auto& f : futures) f.get();

  std::vector<std::thread::id> unique_ids = observed_ids;
  std::sort(unique_ids.begin(), unique_ids.end());
  unique_ids.erase(std::unique(unique_ids.begin(), unique_ids.end()), unique_ids.end());

  EXPECT_GT(unique_ids.size(), 1u);
}

TEST(ThreadPoolTest, ExceptionsPropagateThroughTheFuture) {
  ThreadPool pool(2);
  auto future = pool.Submit([]() -> int { throw std::runtime_error("boom"); });
  EXPECT_THROW(future.get(), std::runtime_error);
}

}  // namespace
}  // namespace dse
