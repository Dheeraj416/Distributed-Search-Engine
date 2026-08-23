#include "dse/redis_cache.hpp"

#include <hiredis/hiredis.h>

#include <spdlog/spdlog.h>

namespace dse {

RedisCache::RedisCache(RedisConfig config) : config_(std::move(config)) {
  pool_.reserve(config_.connection_pool_size);
  for (size_t i = 0; i < config_.connection_pool_size; ++i) {
    auto conn = std::make_unique<PooledConnection>();
    conn->ctx = Connect();
    pool_.push_back(std::move(conn));
  }

  if (!IsAvailable()) {
    spdlog::warn(
        "RedisCache: could not connect to redis at {}:{} — search will run uncached "
        "(this is not a hard failure; see docs/DESIGN_DECISIONS.md)",
        config_.host, config_.port);
  } else {
    spdlog::info("RedisCache: connected to {}:{} ({} pooled connections)", config_.host,
                 config_.port, config_.connection_pool_size);
  }
}

RedisCache::~RedisCache() {
  for (auto& conn : pool_) {
    if (conn->ctx != nullptr) {
      redisFree(conn->ctx);
    }
  }
}

redisContext* RedisCache::Connect() const {
  struct timeval timeout;
  timeout.tv_sec = config_.connect_timeout_ms / 1000;
  timeout.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;

  redisContext* ctx = redisConnectWithTimeout(config_.host.c_str(), config_.port, timeout);
  if (ctx == nullptr || ctx->err) {
    if (ctx != nullptr) {
      spdlog::debug("RedisCache: connection error: {}", ctx->errstr);
      redisFree(ctx);
    }
    return nullptr;
  }
  return ctx;
}

bool RedisCache::IsAvailable() const {
  for (const auto& conn : pool_) {
    if (conn->ctx != nullptr) return true;
  }
  return false;
}

RedisCache::PooledConnection& RedisCache::PickConnection() {
  size_t index = round_robin_counter_.fetch_add(1, std::memory_order_relaxed) % pool_.size();
  return *pool_[index];
}

std::string RedisCache::BuildKey(const std::string& normalized_query, uint32_t top_k) {
  return "dse:search:" + normalized_query + ":" + std::to_string(top_k);
}

std::optional<std::string> RedisCache::Get(const std::string& key) {
  PooledConnection& conn = PickConnection();
  std::lock_guard<std::mutex> lock(conn.mutex);
  if (conn.ctx == nullptr) return std::nullopt;

  redisReply* reply = static_cast<redisReply*>(redisCommand(conn.ctx, "GET %s", key.c_str()));
  if (reply == nullptr) {
    // The connection dropped — hiredis reports this by returning nullptr
    // and setting ctx->err. Treat as a cache miss rather than crashing;
    // a future call may reconnect (not implemented for this project's
    // scope, see docs/DESIGN_DECISIONS.md).
    return std::nullopt;
  }

  std::optional<std::string> result;
  if (reply->type == REDIS_REPLY_STRING) {
    result = std::string(reply->str, reply->len);
  }
  freeReplyObject(reply);
  return result;
}

void RedisCache::Set(const std::string& key, const std::string& value) {
  PooledConnection& conn = PickConnection();
  std::lock_guard<std::mutex> lock(conn.mutex);
  if (conn.ctx == nullptr) return;

  redisReply* reply = static_cast<redisReply*>(
      redisCommand(conn.ctx, "SETEX %s %d %s", key.c_str(), config_.ttl_seconds, value.c_str()));
  if (reply != nullptr) {
    freeReplyObject(reply);
  }
}

}  // namespace dse
