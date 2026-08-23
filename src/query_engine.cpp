#include "dse/query_engine.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace dse {

using json = nlohmann::json;

QueryEngine::QueryEngine(InvertedIndex& index, RedisCache* cache, ThreadPool* scoring_pool,
                          BM25Params params)
    : index_(index), cache_(cache), pool_(scoring_pool), params_(params) {}

std::string QueryEngine::SerializeOutcome(const SearchOutcome& outcome) const {
  json j;
  j["total_matches"] = outcome.total_matches;
  j["results"] = json::array();
  for (const auto& r : outcome.results) {
    j["results"].push_back({
        {"doc_id", r.doc_id},
        {"title", r.title},
        {"url", r.url},
        {"score", r.score},
        {"snippet", r.snippet},
    });
  }
  return j.dump();
}

bool QueryEngine::TryDeserializeOutcome(const std::string& text, SearchOutcome* out) const {
  try {
    json j = json::parse(text);
    out->total_matches = j.at("total_matches").get<size_t>();
    out->results.clear();
    for (const auto& item : j.at("results")) {
      SearchResultItem r;
      r.doc_id = item.at("doc_id").get<std::string>();
      r.title = item.at("title").get<std::string>();
      r.url = item.at("url").get<std::string>();
      r.score = item.at("score").get<double>();
      r.snippet = item.at("snippet").get<std::string>();
      out->results.push_back(std::move(r));
    }
    return true;
  } catch (const std::exception& e) {
    spdlog::warn("QueryEngine: failed to deserialize cached result ({}), treating as a miss", e.what());
    return false;
  }
}

std::string QueryEngine::BuildSnippet(const Document& doc,
                                       const std::vector<std::string>& query_terms) const {
  // Find the earliest position in the body (case-insensitively) where any
  // query term occurs as a substring, and return a fixed-width window of
  // context around it — a simple, fast approximation of "highlight why
  // this result matched" without a full text-alignment/highlighting
  // engine.
  std::string lower_body = doc.body;
  std::transform(lower_body.begin(), lower_body.end(), lower_body.begin(),
                  [](unsigned char c) { return std::tolower(c); });

  size_t best_pos = std::string::npos;
  for (const auto& term : query_terms) {
    size_t pos = lower_body.find(term);
    if (pos != std::string::npos && (best_pos == std::string::npos || pos < best_pos)) {
      best_pos = pos;
    }
  }

  if (best_pos == std::string::npos) {
    // No literal substring match (can happen if the match came from the
    // title, or from a token that doesn't appear as a contiguous
    // substring due to punctuation stripping) — fall back to the start
    // of the body.
    best_pos = 0;
  }

  size_t start = best_pos > kSnippetContextChars ? best_pos - kSnippetContextChars : 0;
  size_t end = std::min(doc.body.size(), best_pos + kSnippetContextChars);
  std::string snippet = doc.body.substr(start, end - start);

  std::string prefix = start > 0 ? "…" : "";
  std::string suffix = end < doc.body.size() ? "…" : "";
  return prefix + snippet + suffix;
}

SearchOutcome QueryEngine::Search(const std::string& raw_query, uint32_t top_k) const {
  auto start_time = std::chrono::steady_clock::now();
  if (top_k == 0) top_k = kDefaultTopK;

  std::vector<std::string> query_terms = tokenizer_.Tokenize(raw_query);

  SearchOutcome outcome;
  if (query_terms.empty()) {
    outcome.query_time_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start_time)
                                 .count();
    return outcome;
  }

  // Cache key is built from the *sorted* term list so "redis cache" and
  // "cache redis" (same terms, different order) hit the same cache
  // entry — search is a bag-of-words model, so their results are
  // identical anyway.
  std::vector<std::string> sorted_terms = query_terms;
  std::sort(sorted_terms.begin(), sorted_terms.end());
  std::string normalized_query;
  for (const auto& t : sorted_terms) {
    if (!normalized_query.empty()) normalized_query += " ";
    normalized_query += t;
  }
  std::string cache_key = RedisCache::BuildKey(normalized_query, top_k);

  if (cache_ != nullptr) {
    auto cached = cache_->Get(cache_key);
    if (cached.has_value() && TryDeserializeOutcome(*cached, &outcome)) {
      outcome.cache_hit = true;
      outcome.query_time_ms = std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - start_time)
                                   .count();
      return outcome;
    }
  }

  BM25Scorer scorer(index_, params_);
  std::unordered_map<std::string, double> scores = scorer.ScoreAll(query_terms, pool_);

  std::vector<std::pair<std::string, double>> ranked(scores.begin(), scores.end());
  size_t k = std::min(static_cast<size_t>(top_k), ranked.size());
  std::partial_sort(
      ranked.begin(), ranked.begin() + k, ranked.end(),
      [](const auto& a, const auto& b) { return a.second > b.second; });

  outcome.total_matches = ranked.size();
  outcome.results.reserve(k);
  for (size_t i = 0; i < k; ++i) {
    const auto& [doc_id, score] = ranked[i];
    auto doc_opt = index_.GetDocument(doc_id);
    if (!doc_opt.has_value()) continue;  // defensive: doc removed mid-query

    SearchResultItem item;
    item.doc_id = doc_id;
    item.title = doc_opt->title;
    item.url = doc_opt->url;
    item.score = score;
    item.snippet = BuildSnippet(*doc_opt, query_terms);
    outcome.results.push_back(std::move(item));
  }

  outcome.query_time_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time)
          .count();

  if (cache_ != nullptr) {
    cache_->Set(cache_key, SerializeOutcome(outcome));
  }

  return outcome;
}

}  // namespace dse
