# Architecture

## Component overview

```
                         ┌─────────────────────────────┐
                         │           dse_client          │
                         │  (CLI: index / search / stats)│
                         └───────────────┬───────────────┘
                                         │ gRPC (search.proto)
                                         ▼
┌───────────────────────────────────────────────────────────────────────┐
│                              dse_server                                │
│                                                                        │
│   ┌────────────────────────┐        ┌─────────────────────────────┐   │
│   │   SearchServiceImpl     │◄──────►│         QueryEngine          │   │
│   │  (gRPC-facing adapter)  │        │  tokenize → cache check →    │   │
│   └───────────┬─────────────┘        │  BM25 scoring → snippet →    │   │
│               │                      │  cache write                 │   │
│               │                      └───────────┬──────────┬───────┘   │
│               ▼                                  │          │           │
│   ┌────────────────────────┐        ┌────────────▼───┐  ┌───▼────────┐ │
│   │   ThreadPool (indexing) │        │  InvertedIndex   │  │ BM25Scorer │ │
│   │  fans out IndexBatch    │───────►│ (shared_mutex-   │◄─┤            │ │
│   │  documents concurrently │        │  guarded index)  │  └────────────┘ │
│   └────────────────────────┘        └──────────────────┘                │
│                                                    ▲                     │
│   ┌────────────────────────┐                      │                     │
│   │  ThreadPool (scoring)   │──────────────────────┘                     │
│   │  parallel BM25 scoring  │                                            │
│   │  for large candidate    │        ┌──────────────────┐                │
│   │  sets                   │        │    RedisCache      │                │
│   └────────────────────────┘        │ (pooled hiredis    │                │
│                                      │  connections)       │                │
│                                      └─────────┬──────────┘                │
└────────────────────────────────────────────────┼───────────────────────────┘
                                                  ▼
                                          ┌──────────────┐
                                          │    Redis      │
                                          └──────────────┘
```

## Request lifecycles

### Indexing a single document (`IndexDocument`)

1. `SearchServiceImpl::IndexDocument` validates `doc_id` is non-empty.
2. Translates the protobuf `IndexDocumentRequest` into a plain `Document`.
3. Calls `InvertedIndex::AddDocument`, which:
   - Tokenizes `title` and `body` (title tokens folded into the same
     term-frequency count as body tokens — a simple stand-in for "give
     title terms extra weight").
   - Takes the index's `shared_mutex` in exclusive (write) mode.
   - If `doc_id` already exists, removes its old postings first (a clean
     replace, not an accumulation).
   - Inserts new postings, updates the per-document length and running
     token total.
4. Returns `{success: true}`.

### Bulk indexing (`IndexBatch`, client-streaming)

1. The client opens a client-streaming RPC and writes many
   `IndexDocumentRequest` messages, then closes the stream.
2. `SearchServiceImpl::IndexBatch` first drains the entire stream into a
   `std::vector` — gRPC's `ServerReader` is itself single-threaded per
   call, so there's no way to read concurrently, but the *processing* of
   each already-received document doesn't have to be sequential.
3. Every document's tokenize-and-insert step is submitted as a task to the
   dedicated indexing `ThreadPool`, returning a `std::future<bool>` per
   document.
4. The handler thread waits on every future, tallying `indexed_count` /
   `failed_count`, and reports `elapsed_ms` for the whole batch.

This is deliberately a separate thread pool from the one used for query
scoring (see `docs/DESIGN_DECISIONS.md`) so a large bulk upload can't starve
concurrent search traffic of worker threads, and vice versa.

### Searching (`Search`)

1. `QueryEngine::Search` tokenizes the raw query string with the same
   `Tokenizer` used at index time (so query and document terms are
   normalized identically).
2. Builds a cache key from the *sorted* set of query terms plus `top_k`,
   so "redis cache" and "cache redis" (same terms, different order) share
   one cache entry — search here is a bag-of-words model, so their results
   are identical anyway.
3. Checks `RedisCache::Get`. On a hit, deserializes the cached JSON payload
   and returns immediately with `cache_hit = true`.
4. On a miss:
   a. `BM25Scorer::ScoreAll` computes a relevance score for every candidate
      document (the union of postings across query terms) — see
      "BM25 scoring in detail" below for the sequential/parallel split.
   b. The top `top_k` candidates are selected with `std::partial_sort`
      (O(n log k), not a full O(n log n) sort).
   c. Each result gets a generated snippet (`QueryEngine::BuildSnippet`):
      the earliest case-insensitive substring match of any query term in
      the document body, with a fixed-width window of context around it.
   d. The result set is serialized to JSON and written back to
      `RedisCache::Set` with a TTL (default 60s), so the *next* identical
      query is a cache hit.
5. Returns the ranked results, `total_matches` (candidate count before
   truncating to `top_k`), `query_time_ms`, and `cache_hit`.

### BM25 scoring in detail

`BM25Scorer::ScoreAll` is where the "multithreading" part of the resume
line actually shows up on the query path:

1. For every query term, it fetches that term's IDF (inverse document
   frequency, computed from `InvertedIndex::DocumentFrequency`) and a
   *copy* of its postings list (`doc_id → term frequency`), bundling both
   into an immutable `TermContext`. This copy happens once, up front,
   while still holding (briefly, per term) the index's read lock.
2. The candidate set is the union of doc_ids across all `TermContext`
   postings.
3. If the candidate set is smaller than `kParallelScoringThreshold` (64
   documents) or no thread pool was supplied, scoring runs sequentially on
   the calling thread — the dispatch overhead of a thread pool isn't worth
   it for small candidate sets.
4. Otherwise, candidates are partitioned into `pool->ThreadCount()`
   contiguous chunks, each submitted as a `ThreadPool::Submit` task that
   scores its chunk into its own local `unordered_map` (no shared mutable
   state between workers — each only reads the already-materialized
   `TermContext` list and the index's own internally-locked
   `DocumentLength` accessor). The calling thread waits on every future and
   merges the partial maps.

This is why `BM25Scorer::ScoreAll`'s worker lambdas need no additional
locking of their own: every read they perform is either against immutable,
pre-copied data (`TermContext`), or against `InvertedIndex`'s own
internally shared_mutex-guarded accessors.

## Concurrency model

- **`InvertedIndex`** is guarded by one `std::shared_mutex`. Reads
  (postings lookups, document fetches, stats) take a shared lock, so many
  queries can read concurrently. Writes (`AddDocument`/`RemoveDocument`)
  take an exclusive lock, briefly blocking readers — the standard, simple
  tradeoff for an index at this project's scale (see
  `docs/DESIGN_DECISIONS.md` for the sharded alternative).
- **`ThreadPool`** is a small, hand-rolled fixed-size pool
  (`std::thread`/`std::mutex`/`std::condition_variable`/
  `std::queue<std::function<void()>>`), used in two independent instances:
  one for query-time BM25 scoring, one for bulk-indexing fan-out.
- **`RedisCache`** holds a small pool of `hiredis` connections (default 4),
  each with its own mutex, selected round-robin via an atomic counter — a
  single `redisContext` is not safe to share across threads making
  concurrent calls, and one mutex around the whole cache would serialize
  every cache access, defeating the point of a multithreaded query engine.

## Data flow summary

```
Document (JSON) ──IndexBatch/IndexDocument──► InvertedIndex (postings, docs)
                                                        │
Query string ──Search──► Tokenizer ──► cache key ──► RedisCache.Get
                                                        │ miss
                                                        ▼
                                          BM25Scorer.ScoreAll (± ThreadPool)
                                                        │
                                          partial_sort top_k + snippets
                                                        │
                                          RedisCache.Set ──► SearchResponse
```
