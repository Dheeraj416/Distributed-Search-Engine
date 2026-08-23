#include "dse/query_engine.hpp"

#include <gtest/gtest.h>

#include "dse/inverted_index.hpp"
#include "dse/redis_cache.hpp"
#include "dse/thread_pool.hpp"
#include "dse/tokenizer.hpp"

namespace dse {
namespace {

Document MakeDoc(const std::string& id, const std::string& title, const std::string& body) {
  Document doc;
  doc.doc_id = id;
  doc.title = title;
  doc.url = "https://example.test/" + id;
  doc.body = body;
  return doc;
}

class QueryEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Tokenizer tokenizer;
    index_.AddDocument(
        MakeDoc("d1", "Distributed Search Engine",
                "A distributed search engine indexes documents and ranks them with BM25."),
        tokenizer);
    index_.AddDocument(
        MakeDoc("d2", "Cooking With Vegetables",
                "This recipe covers roasting vegetables for a simple dinner."),
        tokenizer);
    index_.AddDocument(
        MakeDoc("d3", "Search Ranking Algorithms",
                "BM25 is a ranking algorithm used by many search engine implementations."),
        tokenizer);
  }

  InvertedIndex index_;
};

TEST_F(QueryEngineTest, SearchReturnsRelevantDocumentsRankedByScore) {
  // No RedisCache/ThreadPool provided at all — QueryEngine must run fully
  // uncached and sequentially.
  QueryEngine engine(index_, /*cache=*/nullptr, /*scoring_pool=*/nullptr);

  SearchOutcome outcome = engine.Search("search engine ranking", 10);

  EXPECT_GE(outcome.total_matches, 2u);  // d1 and d3 both mention search/ranking
  ASSERT_FALSE(outcome.results.empty());
  EXPECT_FALSE(outcome.cache_hit);
  // Every returned result should be sorted by descending score.
  for (size_t i = 1; i < outcome.results.size(); ++i) {
    EXPECT_GE(outcome.results[i - 1].score, outcome.results[i].score);
  }
}

TEST_F(QueryEngineTest, EmptyQueryReturnsNoResults) {
  QueryEngine engine(index_, nullptr, nullptr);
  SearchOutcome outcome = engine.Search("", 10);
  EXPECT_TRUE(outcome.results.empty());
  EXPECT_EQ(outcome.total_matches, 0u);
}

TEST_F(QueryEngineTest, QueryWithNoMatchingTermsReturnsNoResults) {
  QueryEngine engine(index_, nullptr, nullptr);
  SearchOutcome outcome = engine.Search("zzznotarealword", 10);
  EXPECT_TRUE(outcome.results.empty());
  EXPECT_EQ(outcome.total_matches, 0u);
}

TEST_F(QueryEngineTest, TopKLimitsResultCountButNotTotalMatches) {
  QueryEngine engine(index_, nullptr, nullptr);
  SearchOutcome outcome = engine.Search("search engine ranking algorithm", 1);
  EXPECT_LE(outcome.results.size(), 1u);
  EXPECT_GE(outcome.total_matches, outcome.results.size());
}

TEST_F(QueryEngineTest, ResultsIncludeNonEmptySnippet) {
  QueryEngine engine(index_, nullptr, nullptr);
  SearchOutcome outcome = engine.Search("ranking", 10);
  ASSERT_FALSE(outcome.results.empty());
  for (const auto& result : outcome.results) {
    EXPECT_FALSE(result.snippet.empty());
  }
}

TEST_F(QueryEngineTest, WorksWithScoringPoolProvided) {
  ThreadPool pool(2);
  QueryEngine engine(index_, nullptr, &pool);
  SearchOutcome outcome = engine.Search("search engine ranking", 10);
  ASSERT_FALSE(outcome.results.empty());
}

TEST_F(QueryEngineTest, DegradesGracefullyWhenRedisIsUnreachable) {
  // Default RedisConfig points at localhost:6379 with a short connect
  // timeout. In this test environment no Redis server is listening, so
  // the connection fails fast (ECONNREFUSED) and RedisCache falls back to
  // "always miss, no-op set" rather than throwing or blocking.
  RedisConfig config;
  config.connect_timeout_ms = 100;
  RedisCache cache(config);
  EXPECT_FALSE(cache.IsAvailable());

  QueryEngine engine(index_, &cache, nullptr);
  SearchOutcome first = engine.Search("search engine", 5);
  SearchOutcome second = engine.Search("search engine", 5);

  EXPECT_FALSE(first.cache_hit);
  EXPECT_FALSE(second.cache_hit);  // still a miss — cache is unavailable, not just cold
  ASSERT_FALSE(first.results.empty());
  ASSERT_FALSE(second.results.empty());
}

}  // namespace
}  // namespace dse
