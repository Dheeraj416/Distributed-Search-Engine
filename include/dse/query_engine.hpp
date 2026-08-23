// QueryEngine: the single entry point the gRPC server calls for a search
// request. Ties together tokenization, the Redis result cache, BM25
// scoring (optionally parallelized via a ThreadPool), and snippet
// generation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dse/bm25.hpp"
#include "dse/inverted_index.hpp"
#include "dse/redis_cache.hpp"
#include "dse/thread_pool.hpp"
#include "dse/tokenizer.hpp"

namespace dse {

struct SearchResultItem {
  std::string doc_id;
  std::string title;
  std::string url;
  double score = 0.0;
  std::string snippet;
};

struct SearchOutcome {
  std::vector<SearchResultItem> results;
  size_t total_matches = 0;
  double query_time_ms = 0.0;
  bool cache_hit = false;
};

class QueryEngine {
 public:
  // `cache` and `scoring_pool` may both be nullptr — the engine degrades
  // gracefully to "uncached, sequential scoring" rather than requiring
  // either dependency (see docs/DESIGN_DECISIONS.md).
  QueryEngine(InvertedIndex& index, RedisCache* cache, ThreadPool* scoring_pool,
              BM25Params params = {});

  SearchOutcome Search(const std::string& raw_query, uint32_t top_k) const;

 private:
  std::string BuildSnippet(const Document& doc,
                            const std::vector<std::string>& query_terms) const;
  std::string SerializeOutcome(const SearchOutcome& outcome) const;
  bool TryDeserializeOutcome(const std::string& json, SearchOutcome* out) const;

  InvertedIndex& index_;
  RedisCache* cache_;
  ThreadPool* pool_;
  BM25Params params_;
  Tokenizer tokenizer_;

  static constexpr uint32_t kDefaultTopK = 10;
  static constexpr size_t kSnippetContextChars = 60;
};

}  // namespace dse
