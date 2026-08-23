// Integration test: spins up a real gRPC server backed by
// SearchServiceImpl on an ephemeral localhost port, then drives it with a
// real gRPC client stub over the network stack — exercising the actual
// protobuf (de)serialization and RPC dispatch, not just the C++ methods
// directly.
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>

#include "dse/inverted_index.hpp"
#include "dse/query_engine.hpp"
#include "dse/redis_cache.hpp"
#include "dse/search_service_impl.hpp"
#include "dse/thread_pool.hpp"
#include "search.grpc.pb.h"

namespace dse {
namespace {

class SearchServiceIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    RedisConfig redis_config;
    redis_config.connect_timeout_ms = 100;  // no live redis in this test env — fails fast
    cache_ = std::make_unique<RedisCache>(redis_config);

    scoring_pool_ = std::make_unique<ThreadPool>(2);
    indexing_pool_ = std::make_unique<ThreadPool>(2);
    query_engine_ = std::make_unique<QueryEngine>(index_, cache_.get(), scoring_pool_.get());
    service_ = std::make_unique<SearchServiceImpl>(index_, *query_engine_, *indexing_pool_);

    grpc::ServerBuilder builder;
    int selected_port = 0;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &selected_port);
    builder.RegisterService(service_.get());
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr);
    ASSERT_NE(selected_port, 0);

    auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(selected_port),
                                        grpc::InsecureChannelCredentials());
    stub_ = SearchService::NewStub(channel);
  }

  void TearDown() override {
    if (server_) server_->Shutdown();
  }

  InvertedIndex index_;
  std::unique_ptr<RedisCache> cache_;
  std::unique_ptr<ThreadPool> scoring_pool_;
  std::unique_ptr<ThreadPool> indexing_pool_;
  std::unique_ptr<QueryEngine> query_engine_;
  std::unique_ptr<SearchServiceImpl> service_;
  std::unique_ptr<grpc::Server> server_;
  std::unique_ptr<SearchService::Stub> stub_;
};

TEST_F(SearchServiceIntegrationTest, IndexDocumentThenSearchFindsIt) {
  IndexDocumentRequest index_req;
  index_req.set_doc_id("doc-1");
  index_req.set_title("gRPC Integration Test");
  index_req.set_url("https://example.test/doc-1");
  index_req.set_body("This document is indexed over a real gRPC connection for testing.");

  IndexDocumentResponse index_resp;
  grpc::ClientContext ctx1;
  grpc::Status status = stub_->IndexDocument(&ctx1, index_req, &index_resp);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(index_resp.success());

  SearchRequest search_req;
  search_req.set_query("gRPC connection");
  search_req.set_top_k(5);
  SearchResponse search_resp;
  grpc::ClientContext ctx2;
  status = stub_->Search(&ctx2, search_req, &search_resp);
  ASSERT_TRUE(status.ok()) << status.error_message();

  ASSERT_GE(search_resp.results_size(), 1);
  EXPECT_EQ(search_resp.results(0).doc_id(), "doc-1");
  EXPECT_FALSE(search_resp.cache_hit());
}

TEST_F(SearchServiceIntegrationTest, IndexDocumentRejectsEmptyDocId) {
  IndexDocumentRequest req;
  req.set_title("No ID");
  req.set_body("This request has no doc_id and should be rejected as invalid.");

  IndexDocumentResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->IndexDocument(&ctx, req, &resp);

  ASSERT_TRUE(status.ok());  // validation failure is reported in the payload, not a gRPC error
  EXPECT_FALSE(resp.success());
}

TEST_F(SearchServiceIntegrationTest, IndexBatchStreamsMultipleDocumentsAndReportsCounts) {
  grpc::ClientContext ctx;
  IndexBatchResponse resp;
  std::unique_ptr<grpc::ClientWriter<IndexDocumentRequest>> writer(
      stub_->IndexBatch(&ctx, &resp));

  for (int i = 0; i < 10; ++i) {
    IndexDocumentRequest req;
    req.set_doc_id("batch-" + std::to_string(i));
    req.set_title("Batch Document " + std::to_string(i));
    req.set_body("shared batch term number " + std::to_string(i));
    ASSERT_TRUE(writer->Write(req));
  }
  // One deliberately invalid request (empty doc_id) to confirm it's
  // counted as failed rather than crashing the stream.
  IndexDocumentRequest bad_req;
  bad_req.set_title("no id");
  ASSERT_TRUE(writer->Write(bad_req));

  writer->WritesDone();
  grpc::Status status = writer->Finish();

  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(resp.indexed_count(), 10u);
  EXPECT_EQ(resp.failed_count(), 1u);
  EXPECT_GE(resp.elapsed_ms(), 0.0);

  StatsRequest stats_req;
  StatsResponse stats_resp;
  grpc::ClientContext stats_ctx;
  status = stub_->GetStats(&stats_ctx, stats_req, &stats_resp);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(stats_resp.total_documents(), 10u);
}

TEST_F(SearchServiceIntegrationTest, GetStatsReflectsEmptyIndexInitially) {
  StatsRequest req;
  StatsResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->GetStats(&ctx, req, &resp);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(resp.total_documents(), 0u);
  EXPECT_EQ(resp.total_terms(), 0u);
  EXPECT_EQ(resp.total_postings(), 0u);
}

TEST_F(SearchServiceIntegrationTest, SearchOnEmptyIndexReturnsNoResults) {
  SearchRequest req;
  req.set_query("anything");
  req.set_top_k(10);
  SearchResponse resp;
  grpc::ClientContext ctx;
  grpc::Status status = stub_->Search(&ctx, req, &resp);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(resp.results_size(), 0);
  EXPECT_EQ(resp.total_matches(), 0u);
}

}  // namespace
}  // namespace dse
