# API Reference

The service contract lives in `proto/search.proto` and is the single
source of truth; this document explains each RPC's behavior and shows
worked examples using the bundled CLI client (`dse_client`) and
[`grpcurl`](https://github.com/fullstorydev/grpcurl) (optional, for raw
RPC calls without the CLI).

## Service: `dse.SearchService`

### `IndexDocument(IndexDocumentRequest) → IndexDocumentResponse`

Indexes a single document. Idempotent: indexing the same `doc_id` again
*replaces* its previous postings rather than accumulating duplicate term
frequencies.

**Request**

| Field | Type | Notes |
|---|---|---|
| `doc_id` | string | Required, non-empty. Caller-assigned — the indexer never generates IDs. |
| `title` | string | Folded into the same tokenized term stream as `body`. |
| `url` | string | Returned verbatim in search results; not indexed. |
| `body` | string | The main indexed text. |

**Response**

| Field | Type | Notes |
|---|---|---|
| `success` | bool | `false` if `doc_id` was empty — reported as a normal response, not a gRPC error, so the client always gets a message back. |
| `message` | string | Human-readable status. |

**Example**

```bash
./build/dse_client index localhost:50051 data/sample_corpus.json
```

(`dse_client index` uses `IndexBatch` for a whole file — see below. A
single-document `IndexDocument` call is exercised directly in
`tests/test_search_service_integration.cpp` and via `grpcurl`:)

```bash
grpcurl -plaintext -import-path proto -proto search.proto \
  -d '{"doc_id":"doc-100","title":"Test","url":"https://x","body":"hello world"}' \
  localhost:50051 dse.SearchService/IndexDocument
```

### `IndexBatch(stream IndexDocumentRequest) → IndexBatchResponse`

Client-streaming bulk indexing. The client sends many
`IndexDocumentRequest` messages over one connection and receives a single
summary once the stream closes — used by the CLI's `index` command so an
entire corpus file is pushed in one RPC instead of one round-trip per
document. The server fans the actual tokenize-and-insert work for each
document out across its indexing `ThreadPool` (see
`docs/ARCHITECTURE.md`).

**Response**

| Field | Type | Notes |
|---|---|---|
| `indexed_count` | uint32 | Documents successfully indexed. |
| `failed_count` | uint32 | Documents rejected (currently: empty `doc_id`). |
| `elapsed_ms` | double | Wall-clock time for the whole batch's processing (not including stream transfer time). |

**Example**

```bash
./build/dse_client index localhost:50051 data/sample_corpus.json
```

```
Sent 12 document(s).
Indexed: 12
Failed:  0
Elapsed: 0.82 ms
```

The corpus file format is a JSON array of objects with `doc_id`, `title`,
`url`, `body` fields — see `data/sample_corpus.json`.

### `Search(SearchRequest) → SearchResponse`

Ranks and returns the top-`k` documents for a free-text query using BM25
scoring, with a Redis-backed result cache in front of the scorer.

**Request**

| Field | Type | Notes |
|---|---|---|
| `query` | string | Free text; tokenized the same way as indexed documents. |
| `top_k` | uint32 | Number of results to return. `0` defaults to 10 server-side. |

**Response**

| Field | Type | Notes |
|---|---|---|
| `results` | repeated `SearchResult` | Ranked highest-score first. |
| `total_matches` | uint32 | Candidate document count *before* truncating to `top_k` — lets a client show "showing 10 of 47 matches". |
| `query_time_ms` | double | Server-side time for this call, cache hit or miss. |
| `cache_hit` | bool | Whether this result came from the Redis cache. |

`SearchResult` fields: `doc_id`, `title`, `url`, `score` (the raw BM25
score, unbounded — compare relatively, not against a fixed scale), and
`snippet` (a short excerpt of the body around the first matched term).

**Example**

```bash
./build/dse_client search localhost:50051 "distributed search engine" 5
```

```
Query: "distributed search engine"  (12 match(es), 0.05 ms, cache MISS)

1. [0.9152] Introduction to Distributed Systems  (doc-001)
   https://example-notes.dev/distributed-systems-intro
   A distributed system is a collection of independent computers …
...
```

Running the exact same query again returns `cache HIT` and a much smaller
`query_time_ms`, since Redis serves the previously-computed, serialized
result directly (when Redis is reachable — see `docs/DESIGN_DECISIONS.md`
for the uncached fallback).

### `GetStats(StatsRequest) → StatsResponse`

Point-in-time introspection of the index. Takes no fields.

**Response**

| Field | Type | Notes |
|---|---|---|
| `total_documents` | uint64 | |
| `total_terms` | uint64 | Distinct vocabulary size. |
| `total_postings` | uint64 | Sum of postings-list lengths across all terms. |
| `average_doc_length` | double | Average token count per document. |
| `approx_index_bytes` | uint64 | A rough memory-footprint estimate for the CLI's `stats` command — not a precise profile (see the comment in `inverted_index.cpp`). |

**Example**

```bash
./build/dse_client stats localhost:50051
```

```
Total documents:      12
Total distinct terms: 328
Total postings:       456
Average doc length:   45.17 tokens
Approx index size:    18355 bytes
```

## Error handling conventions

- **Validation failures** (e.g. an empty `doc_id` on `IndexDocument`, or
  one bad document inside an `IndexBatch` stream) are reported *inside* the
  response payload (`success: false`, or counted in `failed_count`), not as
  a gRPC-level error — a batch of 100 documents with one bad entry still
  succeeds for the other 99.
- **Transport-level failures** (server unreachable, connection refused,
  malformed request) surface as a non-OK `grpc::Status`, which every
  `dse_client` command checks and reports before exiting non-zero.
- A query that matches nothing (empty index, or no query terms present in
  the vocabulary) is not an error — it's a normal `SearchResponse` with an
  empty `results` list and `total_matches: 0`.
