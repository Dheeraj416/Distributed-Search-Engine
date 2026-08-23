# Distributed Search Engine

A multithreaded, gRPC-based full-text search engine written in modern C++
(C++17). It maintains an in-memory inverted index, ranks results with the
BM25 algorithm, caches repeated queries in Redis, and parallelizes both
query scoring and bulk document indexing across a hand-rolled thread pool.

This project is a from-scratch rebuild matching the "Distributed Search
Engine (C++ | Redis | gRPC | Multithreading)" line on the author's resume.
It is a single-node engine — "distributed" here refers to its network-facing
gRPC API (multiple client processes across machines can index and query it
concurrently) and to the design being explicitly built with horizontal
sharding in mind (see `docs/DESIGN_DECISIONS.md`), not to an implemented
multi-node cluster. That scope decision, and everything else non-obvious
about this codebase, is explained in `docs/DESIGN_DECISIONS.md`.

## What it does

- Indexes documents (`doc_id`, `title`, `url`, `body`) into an in-memory
  inverted index (term → posting list of `doc_id → term frequency`).
- Answers free-text queries ranked by BM25, an industry-standard
  probabilistic ranking function used by real search engines and search
  libraries (Elasticsearch/Lucene, Ohio, etc.) as the default relevance
  score.
- Caches serialized search results in Redis, keyed by a normalized,
  order-independent form of the query, so repeated or popular queries skip
  scoring entirely — with a documented, tested fallback to running fully
  uncached if Redis is unreachable.
- Parallelizes BM25 scoring across a thread pool once a query's candidate
  set is large enough to be worth the dispatch overhead, and parallelizes
  bulk indexing (`IndexBatch`) the same way.
- Exposes all of this over a gRPC API (`proto/search.proto`) with a CLI
  client (`dse_client`) for indexing a JSON corpus and running searches
  from the terminal.

## Project structure

```
distributed-search-engine/
├── proto/
│   └── search.proto            # gRPC service + message definitions
├── include/dse/                 # Public headers for every core class
│   ├── document.hpp
│   ├── tokenizer.hpp
│   ├── inverted_index.hpp
│   ├── bm25.hpp
│   ├── thread_pool.hpp
│   ├── redis_cache.hpp
│   ├── query_engine.hpp
│   └── search_service_impl.hpp
├── src/
│   ├── tokenizer.cpp
│   ├── inverted_index.cpp
│   ├── bm25.cpp
│   ├── thread_pool.cpp
│   ├── redis_cache.cpp
│   ├── query_engine.cpp
│   ├── search_service_impl.cpp
│   ├── server_main.cpp          # dse_server entrypoint
│   └── client_main.cpp          # dse_client CLI entrypoint
├── tests/                       # GoogleTest unit + integration tests
├── data/
│   └── sample_corpus.json       # a worked example corpus to index
├── docs/
│   ├── ARCHITECTURE.md
│   ├── API.md
│   ├── SETUP.md
│   └── DESIGN_DECISIONS.md
├── CMakeLists.txt
├── Dockerfile
└── docker-compose.yml
```

## Quick start

The fastest path is Docker Compose, which builds the server image and
starts Redis alongside it:

```bash
docker compose up --build
```

Then, from a second terminal (build the client locally, or exec into the
container — see `docs/SETUP.md`):

```bash
./build/dse_client index localhost:50051 data/sample_corpus.json
./build/dse_client search localhost:50051 "distributed search engine" 5
./build/dse_client stats localhost:50051
```

For building and running without Docker, and for every command used to
verify this project (including the full test run), see `docs/SETUP.md`.

## Tech stack

| Concern | Choice | Resume match |
|---|---|---|
| Language | C++17/20 | ✅ exact |
| RPC | gRPC + Protocol Buffers | ✅ exact |
| Cache | Redis via hiredis (C client) | ✅ exact |
| JSON | nlohmann::json | ✅ exact |
| Logging | spdlog | ✅ exact |
| Testing | GoogleTest | ✅ exact |
| Build | CMake | ✅ exact |
| Concurrency | `std::thread` / `std::shared_mutex` custom thread pool | ✅ exact |
| Packaging | Docker / Docker Compose | ✅ exact |

Unlike the previous two projects in this series, every piece of this
project's resume-stated stack is actually available and fully functional
in the build sandbox — no substitutions were required here. See
`docs/DESIGN_DECISIONS.md` for how that was verified before committing to
the stack, and for the few internal implementation choices (not stack
swaps) made along the way.

## Running the tests

```bash
cd build
ctest --output-on-failure
```

37 tests across 6 suites (tokenizer, inverted index, BM25 ranking, thread
pool, query engine, and a full gRPC integration suite that runs a real
server on an ephemeral port) — all passing. See `docs/SETUP.md` for the
full build-and-test transcript this project was verified against.
