#include "dse/inverted_index.hpp"

#include <thread>
#include <vector>

#include <gtest/gtest.h>

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

TEST(InvertedIndexTest, AddAndGetDocumentRoundTrips) {
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("d1", "Hello World", "This is a simple search test document"), tokenizer);

  auto fetched = index.GetDocument("d1");
  ASSERT_TRUE(fetched.has_value());
  EXPECT_EQ(fetched->title, "Hello World");
  EXPECT_GT(fetched->token_count, 0u);
  EXPECT_EQ(index.DocumentCount(), 1u);
}

TEST(InvertedIndexTest, GetDocumentMissingReturnsNullopt) {
  InvertedIndex index;
  EXPECT_FALSE(index.GetDocument("does-not-exist").has_value());
}

TEST(InvertedIndexTest, PostingsAndDocumentFrequencyAcrossMultipleDocs) {
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("d1", "Search Engine", "distributed search engine ranking"), tokenizer);
  index.AddDocument(MakeDoc("d2", "Cooking Recipes", "vegetable soup recipe"), tokenizer);
  index.AddDocument(MakeDoc("d3", "More Search", "search ranking algorithms search"), tokenizer);

  EXPECT_EQ(index.DocumentFrequency("search"), 2u);   // d1 and d3
  EXPECT_EQ(index.DocumentFrequency("ranking"), 2u);  // d1 and d3
  EXPECT_EQ(index.DocumentFrequency("recipe"), 1u);   // d2 only
  EXPECT_EQ(index.DocumentFrequency("nonexistent"), 0u);

  // Title tokens are folded into the same term-frequency count as body
  // tokens (see InvertedIndex::AddDocument), so "search" in d1's title
  // ("Search Engine") plus its body ("...search engine...") totals 2, and
  // d3's title ("More Search") plus two body occurrences totals 3.
  auto postings = index.GetPostings("search");
  ASSERT_EQ(postings.size(), 2u);
  EXPECT_EQ(postings.at("d1"), 2u);
  EXPECT_EQ(postings.at("d3"), 3u);
}

TEST(InvertedIndexTest, ReindexingSameDocIdReplacesOldPostings) {
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("d1", "Animals", "cat cat dog"), tokenizer);
  EXPECT_EQ(index.DocumentFrequency("cat"), 1u);
  EXPECT_EQ(index.DocumentFrequency("dog"), 1u);

  // Re-index the same doc_id with entirely different content.
  index.AddDocument(MakeDoc("d1", "Vehicles", "car truck airplane"), tokenizer);

  EXPECT_EQ(index.DocumentFrequency("cat"), 0u);
  EXPECT_EQ(index.DocumentFrequency("dog"), 0u);
  EXPECT_EQ(index.DocumentFrequency("car"), 1u);
  EXPECT_EQ(index.DocumentCount(), 1u);  // still just one document, replaced not accumulated

  auto fetched = index.GetDocument("d1");
  ASSERT_TRUE(fetched.has_value());
  EXPECT_EQ(fetched->title, "Vehicles");
}

TEST(InvertedIndexTest, RemoveDocumentClearsItsPostings) {
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("d1", "Alpha", "unique term here"), tokenizer);
  index.AddDocument(MakeDoc("d2", "Beta", "another document entirely"), tokenizer);

  ASSERT_EQ(index.DocumentFrequency("unique"), 1u);
  index.RemoveDocument("d1");

  EXPECT_EQ(index.DocumentFrequency("unique"), 0u);
  EXPECT_FALSE(index.GetDocument("d1").has_value());
  EXPECT_EQ(index.DocumentCount(), 1u);
  EXPECT_TRUE(index.GetDocument("d2").has_value());
}

TEST(InvertedIndexTest, RemoveNonexistentDocumentIsANoop) {
  InvertedIndex index;
  Tokenizer tokenizer;
  index.AddDocument(MakeDoc("d1", "Alpha", "some content"), tokenizer);
  EXPECT_NO_THROW(index.RemoveDocument("never-existed"));
  EXPECT_EQ(index.DocumentCount(), 1u);
}

TEST(InvertedIndexTest, AverageDocumentLengthAndStats) {
  InvertedIndex index;
  Tokenizer tokenizer;
  EXPECT_DOUBLE_EQ(index.AverageDocumentLength(), 0.0);  // empty index

  index.AddDocument(MakeDoc("d1", "", "one two three four"), tokenizer);
  index.AddDocument(MakeDoc("d2", "", "five six"), tokenizer);

  // d1 has 4 tokens, d2 has 2 tokens -> average 3.0
  EXPECT_DOUBLE_EQ(index.AverageDocumentLength(), 3.0);

  IndexStats stats = index.GetStats();
  EXPECT_EQ(stats.total_documents, 2u);
  EXPECT_EQ(stats.total_terms, 6u);  // one,two,three,four,five,six all distinct
  EXPECT_GT(stats.total_postings, 0u);
  EXPECT_GT(stats.approx_index_bytes, 0u);
  EXPECT_DOUBLE_EQ(stats.average_doc_length, 3.0);
}

TEST(InvertedIndexTest, ConcurrentAddsFromMultipleThreadsAreAllVisible) {
  // A functional concurrency smoke test: many threads add distinct
  // documents at the same time; std::shared_mutex serializes the writes
  // internally, so every document must be present and correctly counted
  // once every thread has joined, with no crash or data race under
  // normal (non-sanitized) execution.
  InvertedIndex index;
  Tokenizer tokenizer;
  constexpr int kNumThreads = 8;
  constexpr int kDocsPerThread = 25;

  std::vector<std::thread> threads;
  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&index, &tokenizer, t] {
      for (int i = 0; i < kDocsPerThread; ++i) {
        std::string id = "t" + std::to_string(t) + "-d" + std::to_string(i);
        index.AddDocument(MakeDoc(id, "Concurrent", "shared term thread " + std::to_string(t)),
                           tokenizer);
      }
    });
  }
  for (auto& thread : threads) thread.join();

  EXPECT_EQ(index.DocumentCount(), static_cast<size_t>(kNumThreads * kDocsPerThread));
  EXPECT_EQ(index.DocumentFrequency("shared"), static_cast<size_t>(kNumThreads * kDocsPerThread));
}

}  // namespace
}  // namespace dse
