// server_main.cpp — the gRPC server entrypoint.
//
// Wires together the whole stack:
//   InvertedIndex  (the data)
//   RedisCache     (optional result cache, degrades gracefully if absent)
//   ThreadPool x2  (one for query-time BM25 scoring, one for bulk indexing —
//                   kept separate so a large IndexBatch upload can't starve
//                   concurrent search traffic of worker threads, and vice
//                   versa)
//   QueryEngine    (ties index + cache + scoring pool together)
//   SearchServiceImpl (the gRPC-facing adapter)
//
// Configuration is read from environment variables with sensible
// defaults, so the same binary runs unmodified in Docker Compose (where
// REDIS_HOST=redis) and on a bare-metal dev machine (REDIS_HOST=localhost).
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "dse/inverted_index.hpp"
#include "dse/query_engine.hpp"
#include "dse/redis_cache.hpp"
#include "dse/search_service_impl.hpp"
#include "dse/thread_pool.hpp"

namespace {

std::string GetEnvOrDefault(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value != nullptr ? std::string(value) : fallback;
}

int GetEnvIntOrDefault(const char* name, int fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr) return fallback;
  try {
    return std::stoi(value);
  } catch (const std::exception&) {
    spdlog::warn("Invalid integer for env var {}='{}' — using default {}", name, value, fallback);
    return fallback;
  }
}

}  // namespace

int main(int argc, char** argv) {
  spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
  spdlog::set_level(spdlog::level::info);

  const std::string bind_address =
      GetEnvOrDefault("DSE_BIND_ADDRESS", "0.0.0.0:" + GetEnvOrDefault("DSE_PORT", "50051"));
  const std::string redis_host = GetEnvOrDefault("REDIS_HOST", "localhost");
  const int redis_port = GetEnvIntOrDefault("REDIS_PORT", 6379);
  const int scoring_threads = GetEnvIntOrDefault("DSE_SCORING_THREADS", 4);
  const int indexing_threads = GetEnvIntOrDefault("DSE_INDEXING_THREADS", 4);

  spdlog::info("Distributed Search Engine — starting up");
  spdlog::info("  bind address:     {}", bind_address);
  spdlog::info("  redis:            {}:{}", redis_host, redis_port);
  spdlog::info("  scoring threads:  {}", scoring_threads);
  spdlog::info("  indexing threads: {}", indexing_threads);

  dse::InvertedIndex index;

  dse::RedisConfig redis_config;
  redis_config.host = redis_host;
  redis_config.port = redis_port;
  dse::RedisCache cache(redis_config);

  dse::ThreadPool scoring_pool(static_cast<size_t>(scoring_threads));
  dse::ThreadPool indexing_pool(static_cast<size_t>(indexing_threads));

  dse::QueryEngine query_engine(index, &cache, &scoring_pool);
  dse::SearchServiceImpl service(index, query_engine, indexing_pool);

  grpc::ServerBuilder builder;
  builder.AddListeningPort(bind_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);

  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (server == nullptr) {
    spdlog::error("Failed to start gRPC server on {} — is the port already in use?",
                  bind_address);
    return 1;
  }

  spdlog::info("Server listening on {}", bind_address);
  server->Wait();
  return 0;
}
