#include "dse/bm25.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <unordered_set>

namespace dse {

namespace {
// Below this many candidate documents, the overhead of dispatching to the
// thread pool (task allocation, future synchronization) isn't worth it —
// sequential scoring is faster in practice for small candidate sets.
constexpr size_t kParallelScoringThreshold = 64;
}  // namespace

BM25Scorer::BM25Scorer(const InvertedIndex& index, BM25Params params)
    : index_(index),
      params_(params),
      avg_doc_length_(index.AverageDocumentLength()),
      document_count_(index.DocumentCount()) {}

double BM25Scorer::Idf(const std::string& term) const {
  size_t df = index_.DocumentFrequency(term);
  if (df == 0 || document_count_ == 0) return 0.0;
  double n = static_cast<double>(document_count_);
  double dfd = static_cast<double>(df);
  // The "+1" inside the log keeps IDF non-negative even for a term that
  // appears in more than half the corpus (the classic BM25+ fix for
  // Robertson-Sparck Jones IDF going negative on very common terms).
  return std::log((n - dfd + 0.5) / (dfd + 0.5) + 1.0);
}

namespace {
// One query term's precomputed contribution: its IDF weight and a copy
// of its postings list (doc_id -> tf). Copying the postings out from
// under InvertedIndex's lock once per query, up front, is what lets the
// actual scoring loop below run fully lock-free across worker threads.
struct TermContext {
  double idf;
  std::unordered_map<std::string, uint32_t> postings;
};

double ScoreDocumentAgainstTerms(const std::string& doc_id, uint32_t doc_length,
                                  const std::vector<TermContext>& terms,
                                  const BM25Params& params, double avg_doc_length) {
  double score = 0.0;
  const double length_norm =
      params.k1 * (1.0 - params.b + params.b * (avg_doc_length > 0
                                                      ? doc_length / avg_doc_length
                                                      : 0.0));
  for (const auto& term : terms) {
    auto it = term.postings.find(doc_id);
    if (it == term.postings.end()) continue;
    double tf = static_cast<double>(it->second);
    score += term.idf * (tf * (params.k1 + 1.0)) / (tf + length_norm);
  }
  return score;
}
}  // namespace

std::unordered_map<std::string, double> BM25Scorer::ScoreAll(
    const std::vector<std::string>& query_terms, ThreadPool* pool) const {
  // Precompute each term's IDF and postings once, outside the per-document
  // scoring loop — this is the classic "hoist loop-invariant work" that
  // also happens to be exactly what makes the scoring loop below
  // thread-safe without any locking: every worker only ever reads these
  // already-materialized, immutable TermContext copies.
  std::vector<TermContext> terms;
  terms.reserve(query_terms.size());
  std::unordered_set<std::string> candidate_ids;

  for (const auto& term : query_terms) {
    TermContext ctx;
    ctx.idf = Idf(term);
    ctx.postings = index_.GetPostings(term);
    for (const auto& [doc_id, freq] : ctx.postings) {
      (void)freq;
      candidate_ids.insert(doc_id);
    }
    terms.push_back(std::move(ctx));
  }

  std::vector<std::string> candidates(candidate_ids.begin(), candidate_ids.end());
  std::unordered_map<std::string, double> scores;
  scores.reserve(candidates.size());

  if (pool == nullptr || candidates.size() < kParallelScoringThreshold ||
      pool->ThreadCount() <= 1) {
    for (const auto& doc_id : candidates) {
      uint32_t length = index_.DocumentLength(doc_id);
      scores[doc_id] = ScoreDocumentAgainstTerms(doc_id, length, terms, params_, avg_doc_length_);
    }
    return scores;
  }

  // Partition candidates into ~ThreadCount() contiguous chunks and score
  // each chunk on the pool. Every chunk writes to its own local map, so
  // there's no shared-state contention between workers; the partial maps
  // are merged back on this (calling) thread after all futures resolve.
  size_t num_chunks = std::min(pool->ThreadCount(), candidates.size());
  std::vector<std::future<std::unordered_map<std::string, double>>> futures;
  futures.reserve(num_chunks);

  size_t chunk_size = (candidates.size() + num_chunks - 1) / num_chunks;
  for (size_t start = 0; start < candidates.size(); start += chunk_size) {
    size_t end = std::min(start + chunk_size, candidates.size());
    futures.push_back(pool->Submit(
        [this, &candidates, &terms, start, end]() -> std::unordered_map<std::string, double> {
          std::unordered_map<std::string, double> partial;
          partial.reserve(end - start);
          for (size_t i = start; i < end; ++i) {
            const auto& doc_id = candidates[i];
            uint32_t length = index_.DocumentLength(doc_id);
            partial[doc_id] =
                ScoreDocumentAgainstTerms(doc_id, length, terms, params_, avg_doc_length_);
          }
          return partial;
        }));
  }

  for (auto& future : futures) {
    auto partial = future.get();
    scores.insert(partial.begin(), partial.end());
  }

  return scores;
}

}  // namespace dse
