#pragma once

#include <cstdint>
#include <string>

namespace dse {

// A document as stored by the index. `doc_id` is caller-assigned (the
// indexer does not generate IDs) so callers can index against their own
// stable identifiers (URLs, database keys, etc.).
struct Document {
  std::string doc_id;
  std::string title;
  std::string url;
  std::string body;
  uint32_t token_count = 0;  // filled in by InvertedIndex::AddDocument
};

}  // namespace dse
