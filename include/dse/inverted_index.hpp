// InvertedIndex: the core data structure — term -> {doc_id -> term
// frequency}, plus a document store and per-document lengths, all guarded
// by one std::shared_mutex.
//
// Concurrency model: reads (postings lookups, document fetches, stats)
// take a shared (read) lock, so many queries can read the index at once —
// this matters because Search() is the highest-QPS operation and multiple
// query-engine worker threads (see thread_pool.hpp) read the index
// concurrently while scoring different candidate documents. Writes
// (AddDocument/RemoveDocument) take an exclusive lock — indexing briefly
// blocks concurrent reads, which is the standard, simple tradeoff for an
// index this size; docs/DESIGN_DECISIONS.md discusses the sharded
// alternative for higher write throughput.
#pragma once

#include <cstdint>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "dse/document.hpp"
#include "dse/tokenizer.hpp"

namespace dse {

struct IndexStats {
  uint64_t total_documents = 0;
  uint64_t total_terms = 0;        // distinct vocabulary size
  uint64_t total_postings = 0;     // sum of postings-list lengths
  double average_doc_length = 0.0;
  uint64_t approx_index_bytes = 0;
};

class InvertedIndex {
 public:
  InvertedIndex() = default;

  // Tokenizes `doc.body` (title tokens are also folded in, weighted once
  // each — see inverted_index.cpp), builds the term-frequency postings for
  // this document, and inserts it. If `doc.doc_id` already exists, its
  // previous postings are removed first, so re-indexing a doc_id is a
  // clean replace, not an accumulation.
  void AddDocument(const Document& doc, const Tokenizer& tokenizer);

  // No-op if `doc_id` isn't present.
  void RemoveDocument(const std::string& doc_id);

  std::optional<Document> GetDocument(const std::string& doc_id) const;

  // doc_id -> term frequency for every document containing `term`. Empty
  // map if the term isn't in the vocabulary.
  std::unordered_map<std::string, uint32_t> GetPostings(const std::string& term) const;

  // Number of distinct documents containing `term` — the "df" in BM25's
  // IDF component.
  size_t DocumentFrequency(const std::string& term) const;

  size_t DocumentCount() const;
  double AverageDocumentLength() const;
  IndexStats GetStats() const;

  // Lightweight accessor used on the BM25 scoring hot path — avoids
  // copying an entire Document (title/url/body strings) just to read its
  // token count for every candidate document in a query.
  uint32_t DocumentLength(const std::string& doc_id) const;

 private:
  mutable std::shared_mutex mutex_;

  // term -> (doc_id -> term frequency in that doc)
  std::unordered_map<std::string, std::unordered_map<std::string, uint32_t>> postings_;
  std::unordered_map<std::string, Document> documents_;
  std::unordered_map<std::string, uint32_t> doc_lengths_;
  uint64_t total_tokens_ = 0;

  void RemoveDocumentLocked(const std::string& doc_id);
};

}  // namespace dse
