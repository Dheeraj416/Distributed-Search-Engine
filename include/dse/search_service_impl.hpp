// SearchServiceImpl: the gRPC-facing adapter. Deliberately thin — it
// translates protobuf messages to/from the plain-C++ types the engine
// classes use (Document, SearchOutcome, IndexStats), and every real
// decision (eligibility, scoring, caching) lives in QueryEngine/
// InvertedIndex/BM25Scorer, which are all independently unit-testable
// without spinning up gRPC at all.
#pragma once

#include <memory>

#include "dse/inverted_index.hpp"
#include "dse/query_engine.hpp"
#include "dse/redis_cache.hpp"
#include "dse/thread_pool.hpp"
#include "dse/tokenizer.hpp"
#include "search.grpc.pb.h"

namespace dse {

class SearchServiceImpl final : public SearchService::Service {
 public:
  SearchServiceImpl(InvertedIndex& index, QueryEngine& query_engine, ThreadPool& indexing_pool);

  grpc::Status IndexDocument(grpc::ServerContext* context, const IndexDocumentRequest* request,
                              IndexDocumentResponse* response) override;

  grpc::Status IndexBatch(grpc::ServerContext* context,
                           grpc::ServerReader<IndexDocumentRequest>* reader,
                           IndexBatchResponse* response) override;

  grpc::Status Search(grpc::ServerContext* context, const SearchRequest* request,
                       SearchResponse* response) override;

  grpc::Status GetStats(grpc::ServerContext* context, const StatsRequest* request,
                         StatsResponse* response) override;

 private:
  InvertedIndex& index_;
  QueryEngine& query_engine_;
  ThreadPool& indexing_pool_;
  Tokenizer tokenizer_;
};

}  // namespace dse
