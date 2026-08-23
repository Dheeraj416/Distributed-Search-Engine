#include "dse/inverted_index.hpp"

#include <mutex>

namespace dse {

void InvertedIndex::AddDocument(const Document& doc, const Tokenizer& tokenizer) {
  // Title tokens are folded into the same body-token stream once each —
  // a simple stand-in for "give title terms extra weight" without a
  // separate field-weighted BM25F implementation (see
  // docs/DESIGN_DECISIONS.md for why that's out of scope here).
  std::vector<std::string> tokens = tokenizer.Tokenize(doc.title);
  std::vector<std::string> body_tokens = tokenizer.Tokenize(doc.body);
  tokens.insert(tokens.end(), body_tokens.begin(), body_tokens.end());

  std::unordered_map<std::string, uint32_t> term_freq;
  for (const auto& token : tokens) {
    term_freq[token]++;
  }

  Document stored = doc;
  stored.token_count = static_cast<uint32_t>(tokens.size());

  std::unique_lock lock(mutex_);

  // Re-indexing an existing doc_id: remove its old postings first so we
  // never accumulate stale term frequencies from a previous version.
  RemoveDocumentLocked(doc.doc_id);

  for (const auto& [term, freq] : term_freq) {
    postings_[term][doc.doc_id] = freq;
  }
  documents_[doc.doc_id] = stored;
  doc_lengths_[doc.doc_id] = stored.token_count;
  total_tokens_ += stored.token_count;
}

void InvertedIndex::RemoveDocument(const std::string& doc_id) {
  std::unique_lock lock(mutex_);
  RemoveDocumentLocked(doc_id);
}

void InvertedIndex::RemoveDocumentLocked(const std::string& doc_id) {
  auto doc_it = documents_.find(doc_id);
  if (doc_it == documents_.end()) return;

  // Walking every term to find this doc_id is O(vocabulary), which is
  // fine for a re-index (an uncommon operation relative to search) at
  // this project's scale; see docs/DESIGN_DECISIONS.md for the
  // forward-index tradeoff that would make this O(doc's own term count)
  // instead.
  for (auto term_it = postings_.begin(); term_it != postings_.end();) {
    term_it->second.erase(doc_id);
    if (term_it->second.empty()) {
      term_it = postings_.erase(term_it);
    } else {
      ++term_it;
    }
  }

  auto length_it = doc_lengths_.find(doc_id);
  if (length_it != doc_lengths_.end()) {
    total_tokens_ -= length_it->second;
    doc_lengths_.erase(length_it);
  }

  documents_.erase(doc_it);
}

std::optional<Document> InvertedIndex::GetDocument(const std::string& doc_id) const {
  std::shared_lock lock(mutex_);
  auto it = documents_.find(doc_id);
  if (it == documents_.end()) return std::nullopt;
  return it->second;
}

std::unordered_map<std::string, uint32_t> InvertedIndex::GetPostings(
    const std::string& term) const {
  std::shared_lock lock(mutex_);
  auto it = postings_.find(term);
  if (it == postings_.end()) return {};
  return it->second;
}

size_t InvertedIndex::DocumentFrequency(const std::string& term) const {
  std::shared_lock lock(mutex_);
  auto it = postings_.find(term);
  return it == postings_.end() ? 0 : it->second.size();
}

size_t InvertedIndex::DocumentCount() const {
  std::shared_lock lock(mutex_);
  return documents_.size();
}

uint32_t InvertedIndex::DocumentLength(const std::string& doc_id) const {
  std::shared_lock lock(mutex_);
  auto it = doc_lengths_.find(doc_id);
  return it == doc_lengths_.end() ? 0 : it->second;
}

double InvertedIndex::AverageDocumentLength() const {
  std::shared_lock lock(mutex_);
  if (documents_.empty()) return 0.0;
  return static_cast<double>(total_tokens_) / static_cast<double>(documents_.size());
}

IndexStats InvertedIndex::GetStats() const {
  std::shared_lock lock(mutex_);

  IndexStats stats;
  stats.total_documents = documents_.size();
  stats.total_terms = postings_.size();

  uint64_t total_postings = 0;
  uint64_t approx_bytes = 0;
  for (const auto& [term, docs] : postings_) {
    total_postings += docs.size();
    // Rough accounting: term string + per-posting (doc_id length + a
    // fixed per-entry overhead for the frequency and hash-map bucket).
    // This is an estimate for the `stats` CLI command, not a precise
    // memory profile.
    approx_bytes += term.size();
    for (const auto& [doc_id, freq] : docs) {
      (void)freq;
      approx_bytes += doc_id.size() + 16;
    }
  }
  for (const auto& [doc_id, doc] : documents_) {
    approx_bytes += doc_id.size() + doc.title.size() + doc.url.size() + doc.body.size();
  }

  stats.total_postings = total_postings;
  stats.approx_index_bytes = approx_bytes;
  stats.average_doc_length =
      documents_.empty() ? 0.0 : static_cast<double>(total_tokens_) / documents_.size();

  return stats;
}

}  // namespace dse
