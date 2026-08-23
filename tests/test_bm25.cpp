#include "dse/bm25.hpp"

#include <cmath>

#include <gtest/gtest.h>

#include "dse/inverted_index.hpp"
#include "dse/thread_pool.hpp"
#include "dse/tokenizer.hpp"

namespace dse {
namespace {

Document MakeDoc(const std::string& id, const std::string& body) {
  Document doc;
  doc.doc_id = id;
  doc.title = "";
  doc.url = "https://example.test/" + id;
  doc.body = body;
  return doc;
}

TEST(BM25Test, DocumentMatchingRareTermScoresHigherThanUnrelatedDocument) {
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("relevant", "the rare zephyr appears here"), tokenizer);
  index.AddDocument(MakeDoc("irrelevant", "completely unrelated content about cooking"), tokenizer);

  BM25Scorer scorer(index);
  auto scores = scorer.ScoreAll({"zephyr"});

  ASSERT_EQ(scores.size(), 1u);  // only "relevant" contains the term at all
  ASSERT_TRUE(scores.count("relevant"));
  EXPECT_GT(scores.at("relevant"), 0.0);
  EXPECT_FALSE(scores.count("irrelevant"));
}

TEST(BM25Test, UnknownTermContributesNothingButKnownTermsStillScore) {
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("d1", "search engines rank documents"), tokenizer);

  BM25Scorer scorer(index);
  auto scores_with_junk = scorer.ScoreAll({"search", "zzzznotaword"});
  auto scores_without_junk = scorer.ScoreAll({"search"});

  ASSERT_TRUE(scores_with_junk.count("d1"));
  ASSERT_TRUE(scores_without_junk.count("d1"));
  EXPECT_DOUBLE_EQ(scores_with_junk.at("d1"), scores_without_junk.at("d1"));
}

TEST(BM25Test, EmptyQueryProducesNoScores) {
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("d1", "some content here"), tokenizer);

  BM25Scorer scorer(index);
  EXPECT_TRUE(scorer.ScoreAll({}).empty());
}

TEST(BM25Test, HigherTermFrequencyScoresHigherAllElseEqual) {
  // Two documents of identical length, differing only in how many times
  // the query term occurs — BM25's term-frequency component should rank
  // the higher-frequency document above the lower-frequency one.
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("low_tf", "signal noise noise noise noise noise"), tokenizer);
  index.AddDocument(MakeDoc("high_tf", "signal signal signal signal signal noise"), tokenizer);

  BM25Scorer scorer(index);
  auto scores = scorer.ScoreAll({"signal"});

  ASSERT_TRUE(scores.count("low_tf"));
  ASSERT_TRUE(scores.count("high_tf"));
  EXPECT_GT(scores.at("high_tf"), scores.at("low_tf"));
}

TEST(BM25Test, ParallelScoringMatchesSequentialScoringForLargeCandidateSets) {
  // Build enough documents to cross ScoreAll's internal parallel-dispatch
  // threshold (64 candidates), and confirm the thread-pool path produces
  // numerically identical scores to the sequential path.
  InvertedIndex index;
  Tokenizer tokenizer;
  constexpr int kNumDocs = 200;
  for (int i = 0; i < kNumDocs; ++i) {
    std::string body = "shared query term appears " + std::to_string(i % 5) + " times";
    index.AddDocument(MakeDoc("doc" + std::to_string(i), body), tokenizer);
  }

  BM25Scorer scorer(index);
  ThreadPool pool(4);

  auto sequential = scorer.ScoreAll({"shared", "query"}, nullptr);
  auto parallel = scorer.ScoreAll({"shared", "query"}, &pool);

  ASSERT_EQ(sequential.size(), parallel.size());
  ASSERT_EQ(sequential.size(), static_cast<size_t>(kNumDocs));
  for (const auto& [doc_id, score] : sequential) {
    ASSERT_TRUE(parallel.count(doc_id));
    EXPECT_NEAR(score, parallel.at(doc_id), 1e-9);
  }
}

}  // namespace
}  // namespace dse
