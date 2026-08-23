// client_main.cpp — a small CLI client for the search server.
//
// Usage:
//   dse_client index <host:port> <corpus.json>   Bulk-index a JSON corpus
//                                                  file via the IndexBatch
//                                                  client-streaming RPC.
//   dse_client search <host:port> "<query>" [top_k=10]
//                                                  Run a search and print
//                                                  ranked results.
//   dse_client stats <host:port>                  Print index statistics.
//
// The corpus file is a JSON array of objects with doc_id/title/url/body
// fields — see data/sample_corpus.json for a worked example that ships
// with this project.
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>

#include "search.grpc.pb.h"

using json = nlohmann::json;
using dse::IndexBatchResponse;
using dse::IndexDocumentRequest;
using dse::IndexDocumentResponse;
using dse::SearchRequest;
using dse::SearchResponse;
using dse::SearchService;
using dse::StatsRequest;
using dse::StatsResponse;

namespace {

void PrintUsage(const char* argv0) {
  std::cerr << "Usage:\n"
            << "  " << argv0 << " index <host:port> <corpus.json>\n"
            << "  " << argv0 << " search <host:port> \"<query>\" [top_k]\n"
            << "  " << argv0 << " stats <host:port>\n";
}

std::string ReadFile(const std::string& path) {
  std::ifstream file(path);
  if (!file) {
    throw std::runtime_error("could not open file: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

int RunIndex(const std::shared_ptr<grpc::Channel>& channel, const std::string& corpus_path) {
  json corpus = json::parse(ReadFile(corpus_path));
  if (!corpus.is_array()) {
    std::cerr << "Corpus file must be a JSON array of documents.\n";
    return 1;
  }

  auto stub = SearchService::NewStub(channel);
  grpc::ClientContext context;
  IndexBatchResponse response;

  std::unique_ptr<grpc::ClientWriter<IndexDocumentRequest>> writer(
      stub->IndexBatch(&context, &response));

  size_t sent = 0;
  for (const auto& item : corpus) {
    IndexDocumentRequest req;
    req.set_doc_id(item.value("doc_id", ""));
    req.set_title(item.value("title", ""));
    req.set_url(item.value("url", ""));
    req.set_body(item.value("body", ""));
    if (!writer->Write(req)) {
      std::cerr << "Stream broken while writing document " << sent << "\n";
      break;
    }
    ++sent;
  }
  writer->WritesDone();
  grpc::Status status = writer->Finish();

  if (!status.ok()) {
    std::cerr << "IndexBatch RPC failed: " << status.error_message() << "\n";
    return 1;
  }

  std::cout << "Sent " << sent << " document(s).\n"
            << "Indexed: " << response.indexed_count() << "\n"
            << "Failed:  " << response.failed_count() << "\n"
            << "Elapsed: " << response.elapsed_ms() << " ms\n";
  return response.failed_count() > 0 ? 1 : 0;
}

int RunSearch(const std::shared_ptr<grpc::Channel>& channel, const std::string& query,
              uint32_t top_k) {
  auto stub = SearchService::NewStub(channel);
  SearchRequest request;
  request.set_query(query);
  request.set_top_k(top_k);

  SearchResponse response;
  grpc::ClientContext context;
  grpc::Status status = stub->Search(&context, request, &response);

  if (!status.ok()) {
    std::cerr << "Search RPC failed: " << status.error_message() << "\n";
    return 1;
  }

  std::cout << "Query: \"" << query << "\"  "
            << "(" << response.total_matches() << " match(es), "
            << response.query_time_ms() << " ms, "
            << (response.cache_hit() ? "cache HIT" : "cache MISS") << ")\n\n";

  int rank = 1;
  for (const auto& result : response.results()) {
    std::cout << rank++ << ". [" << result.score() << "] " << result.title() << "  ("
               << result.doc_id() << ")\n"
              << "   " << result.url() << "\n"
              << "   " << result.snippet() << "\n\n";
  }

  if (response.results_size() == 0) {
    std::cout << "(no results)\n";
  }
  return 0;
}

int RunStats(const std::shared_ptr<grpc::Channel>& channel) {
  auto stub = SearchService::NewStub(channel);
  StatsRequest request;
  StatsResponse response;
  grpc::ClientContext context;
  grpc::Status status = stub->GetStats(&context, request, &response);

  if (!status.ok()) {
    std::cerr << "GetStats RPC failed: " << status.error_message() << "\n";
    return 1;
  }

  std::cout << "Total documents:      " << response.total_documents() << "\n"
            << "Total distinct terms: " << response.total_terms() << "\n"
            << "Total postings:       " << response.total_postings() << "\n"
            << "Average doc length:   " << response.average_doc_length() << " tokens\n"
            << "Approx index size:    " << response.approx_index_bytes() << " bytes\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    PrintUsage(argv[0]);
    return 1;
  }

  const std::string command = argv[1];
  const std::string target = argv[2];
  auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());

  try {
    if (command == "index" && argc >= 4) {
      return RunIndex(channel, argv[3]);
    } else if (command == "search" && argc >= 4) {
      uint32_t top_k = argc >= 5 ? static_cast<uint32_t>(std::stoul(argv[4])) : 10;
      return RunSearch(channel, argv[3], top_k);
    } else if (command == "stats") {
      return RunStats(channel);
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  PrintUsage(argv[0]);
  return 1;
}
