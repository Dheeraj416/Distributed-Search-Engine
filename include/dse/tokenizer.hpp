// Tokenizer: turns raw text into a normalized sequence of terms.
//
// Kept deliberately simple and dependency-free (no external NLP library):
// lowercase, strip anything that isn't an alphanumeric character, split on
// whitespace, and drop a small English stopword list plus single-character
// tokens. This is enough to make BM25 scoring meaningful over the sample
// corpus without pulling in a stemmer — see docs/DESIGN_DECISIONS.md for
// the tradeoff (and what a production system would add: stemming,
// lemmatization, n-grams).
#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace dse {

class Tokenizer {
 public:
  Tokenizer();

  // Lowercase, strip punctuation, split on whitespace, drop stopwords and
  // single-character tokens. Deterministic and pure — no shared state is
  // mutated, so this is trivially thread-safe to call concurrently.
  std::vector<std::string> Tokenize(const std::string& text) const;

  // Exposed separately because both indexing (per-document) and query
  // parsing (per-query) need "just normalize one token" without also
  // wanting the stopword filter applied to already-split input twice.
  static std::string NormalizeToken(const std::string& raw);

 private:
  std::unordered_set<std::string> stopwords_;
};

}  // namespace dse
