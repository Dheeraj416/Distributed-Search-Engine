#include "dse/search_service_impl.hpp"

#include <chrono>
#include <future>
#include <vector>

#include <spdlog/spdlog.h>

namespace dse {

namespace {
Document ToDocument(const IndexDocumentRequest& req) {
  Document doc;
  doc.doc_id = req.doc_id();
  doc.title = req.title();
  doc.url = req.url();
  doc.body = req.body();
  return doc;
}
}  // namespace

SearchServiceImpl::SearchServiceImpl(InvertedIndex& index, QueryEngine& query_engine,
                                      ThreadPool& indexing_pool)
    : index_(index), query_engine_(query_engine), indexing_pool_(indexing_pool) {}

grpc::Status SearchServiceImpl::IndexDocument(grpc::ServerContext* /*context*/,
                                               const IndexDocumentRequest* request,
                                               IndexDocumentResponse* response) {
  if (request->doc_id().empty()) {
    response->set_success(false);
    response->set_message("doc_id must not be empty");
    return grpc::Status::OK;  // a validation failure, not a transport error
  }

  index_.AddDocument(ToDocument(*request), tokenizer_);
  response->set_success(true);
  response->set_message("indexed");
  return grpc::Status::OK;
}

grpc::Status SearchServiceImpl::IndexBatch(grpc::ServerContext* /*context*/,
                                            grpc::ServerReader<IndexDocumentRequest>* reader,
                                            IndexBatchResponse* response) {
  auto start_time = std::chrono::steady_clock::now();

  // Read the whole stream first (gRPC's ServerReader is itself
  // single-threaded per call), then fan the actual indexing work — the
  // CPU-bound part, tokenization + postings insertion — out across the
  // indexing thread pool. This is what "multithreaded indexing" means
  // concretely here: many documents from one bulk upload are tokenized
  // and inserted concurrently rather than strictly one at a time.
  std::vector<IndexDocumentRequest> requests;
  IndexDocumentRequest req;
  while (reader->Read(&req)) {
    requests.push_back(req);
  }

  std::vector<std::future<bool>> futures;
  futures.reserve(requests.size());
  for (auto& r : requests) {
    futures.push_back(indexing_pool_.Submit([this, r]() -> bool {
      if (r.doc_id().empty()) return false;
      index_.AddDocument(ToDocument(r), tokenizer_);
      return true;
    }));
  }

  uint32_t indexed = 0;
  uint32_t failed = 0;
  for (auto& f : futures) {
    if (f.get()) {
      ++indexed;
    } else {
      ++failed;
    }
  }

  double elapsed_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time)
          .count();

  response->set_indexed_count(indexed);
  response->set_failed_count(failed);
  response->set_elapsed_ms(elapsed_ms);

  spdlog::info("IndexBatch: indexed {} document(s), {} failed, in {:.2f}ms ({} pool threads)",
               indexed, failed, elapsed_ms, indexing_pool_.ThreadCount());

  return grpc::Status::OK;
}

grpc::Status SearchServiceImpl::Search(grpc::ServerContext* /*context*/,
                                        const SearchRequest* request, SearchResponse* response) {
  SearchOutcome outcome = query_engine_.Search(request->query(), request->top_k());

  for (const auto& item : outcome.results) {
    SearchResult* result = response->add_results();
    result->set_doc_id(item.doc_id);
    result->set_title(item.title);
    result->set_url(item.url);
    result->set_score(item.score);
    result->set_snippet(item.snippet);
  }
  response->set_total_matches(static_cast<uint32_t>(outcome.total_matches));
  response->set_query_time_ms(outcome.query_time_ms);
  response->set_cache_hit(outcome.cache_hit);

  return grpc::Status::OK;
}

grpc::Status SearchServiceImpl::GetStats(grpc::ServerContext* /*context*/,
                                          const StatsRequest* /*request*/,
                                          StatsResponse* response) {
  IndexStats stats = index_.GetStats();
  response->set_total_documents(stats.total_documents);
  response->set_total_terms(stats.total_terms);
  response->set_total_postings(stats.total_postings);
  response->set_average_doc_length(stats.average_doc_length);
  response->set_approx_index_bytes(stats.approx_index_bytes);
  return grpc::Status::OK;
}

}  // namespace dse
