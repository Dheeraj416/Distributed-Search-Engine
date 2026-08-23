// BM25 ranking function over an InvertedIndex.
//
// Standard Okapi BM25:
//
//   score(D, Q) = sum over query terms t of:
//       IDF(t) * ( tf(t, D) * (k1 + 1) ) / ( tf(t, D) + k1 * (1 - b + b * |D| / avgdl) )
//
//   IDF(t) = ln( (N - df(t) + 0.5) / (df(t) + 0.5) + 1 )
//
// where N is the total document count, df(t) is how many documents
// contain t, tf(t, D) is t's frequency in document D, |D| is D's token
// length, and avgdl is the average document length across the corpus.
// k1 (term-frequency saturation) and b (length normalization strength)
// are the standard tunable parameters, defaulted to the commonly-used
// k1=1.5, b=0.75.
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "dse/inverted_index.hpp"
#include "dse/thread_pool.hpp"

namespace dse {

struct BM25Params {
  double k1 = 1.5;
  double b = 0.75;
};

class BM25Scorer {
 public:
  explicit BM25Scorer(const InvertedIndex& index, BM25Params params = {});

  // Scores every document that contains at least one query term (the
  // "candidate set" = union of postings across query terms) and returns
  // doc_id -> score for exactly that candidate set — documents matching
  // none of the query terms are never scored or returned, matching
  // standard sparse-retrieval behavior (no implicit zero-score entries
  // for the whole corpus).
  //
  // When `pool` is non-null and the candidate set is large enough to be
  // worth the dispatch overhead, candidates are partitioned across the
  // pool's worker threads and scored concurrently; each worker computes
  // one region of the output map independently, and no query-time write
  // ever touches the InvertedIndex, so this requires no locking beyond
  // InvertedIndex's own internal read locks.
  std::unordered_map<std::string, double> ScoreAll(const std::vector<std::string>& query_terms,
                                                     ThreadPool* pool = nullptr) const;

 private:
  double Idf(const std::string& term) const;

  const InvertedIndex& index_;
  BM25Params params_;
  double avg_doc_length_;
  size_t document_count_;
};

}  // namespace dse
