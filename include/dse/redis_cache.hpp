// RedisCache: a thin, thread-safe wrapper around hiredis for caching
// serialized search-result payloads keyed by normalized query string.
//
// Thread-safety note: a single hiredis `redisContext` is **not** safe to
// share across threads making concurrent calls. Rather than put a mutex
// around every call (which would serialize all cache access — defeating
// the point of a multithreaded query engine), this class keeps a small
// pool of connections (one per pool worker thread, roughly) behind a
// simple thread-local-ish round-robin, each with its own mutex. See
// docs/DESIGN_DECISIONS.md for the fuller reasoning and the alternative
// (hiredis's async API) that was considered.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C" {
struct redisContext;
}

namespace dse {

struct RedisConfig {
  std::string host = "localhost";
  int port = 6379;
  int connect_timeout_ms = 200;
  int ttl_seconds = 60;          // how long a cached search result stays valid
  size_t connection_pool_size = 4;
};

class RedisCache {
 public:
  explicit RedisCache(RedisConfig config);
  ~RedisCache();

  RedisCache(const RedisCache&) = delete;
  RedisCache& operator=(const RedisCache&) = delete;

  // True if at least one connection in the pool is live. Search still
  // works with caching disabled (Get always misses, Set is a no-op) when
  // Redis is unreachable — see docs/DESIGN_DECISIONS.md "cache is
  // optional, never a hard dependency".
  bool IsAvailable() const;

  std::optional<std::string> Get(const std::string& key);
  void Set(const std::string& key, const std::string& value);

  // Builds the cache key from a normalized query string + top_k, so
  // "same query, different top_k" doesn't collide.
  static std::string BuildKey(const std::string& normalized_query, uint32_t top_k);

 private:
  struct PooledConnection {
    redisContext* ctx = nullptr;
    std::mutex mutex;
  };

  redisContext* Connect() const;
  PooledConnection& PickConnection();

  RedisConfig config_;
  std::vector<std::unique_ptr<PooledConnection>> pool_;
  std::atomic<size_t> round_robin_counter_{0};
};

}  // namespace dse
