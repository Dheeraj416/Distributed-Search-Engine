#include "dse/tokenizer.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace dse {

namespace {
// A small, standard English stopword list — large enough to remove the
// most common low-information words from postings lists (which keeps
// postings lists shorter and BM25 scores more discriminative) without
// trying to be an exhaustive linguistic resource.
const std::vector<std::string> kDefaultStopwords = {
    "a",    "an",   "and",  "are",  "as",   "at",   "be",   "by",   "for",
    "from", "has",  "he",   "in",   "is",   "it",   "its",  "of",   "on",
    "that", "the",  "to",   "was",  "were", "will", "with", "this", "these",
    "those", "or", "but", "not", "can", "could", "would", "should", "which",
    "their", "there", "than", "then", "into", "onto", "your", "you",
};
}  // namespace

Tokenizer::Tokenizer() : stopwords_(kDefaultStopwords.begin(), kDefaultStopwords.end()) {}

std::string Tokenizer::NormalizeToken(const std::string& raw) {
  std::string out;
  out.reserve(raw.size());
  for (unsigned char c : raw) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
    }
  }
  return out;
}

std::vector<std::string> Tokenizer::Tokenize(const std::string& text) const {
  std::vector<std::string> tokens;
  std::istringstream stream(text);
  std::string word;

  while (stream >> word) {
    std::string normalized = NormalizeToken(word);
    if (normalized.size() <= 1) continue;             // drop single characters/noise
    if (stopwords_.count(normalized) > 0) continue;    // drop stopwords
    tokens.push_back(std::move(normalized));
  }

  return tokens;
}

}  // namespace dse
