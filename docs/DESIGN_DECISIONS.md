# Design Decisions

This document explains the non-obvious choices in this codebase: what was
verified before committing to the resume's stated tech stack, the
internal design tradeoffs made while building it, and what a production
version would add that this project deliberately leaves out.

## Stack verification: no swap needed

The two earlier projects in this series (LinkPulse, the Placement
Management Portal) each hit at least one piece of their resume-stated
stack that could not actually work in this sandbox (Maven Central being
blocked by the network egress allowlist, in the portal's case) and had to
document a swap. Before starting this project, the same question was
asked again: can every piece of "C++ | Redis | gRPC | Multithreading"
actually be installed and used here?

```
$ dpkg -l | grep -E 'grpc|protobuf|hiredis|spdlog|nlohmann|gtest'
ii  libgrpc++-dev ... 1.51.1-4.1build5
ii  libgrpc-dev   ... 1.51.1-4.1build5
ii  libhiredis-dev ... 1.2.0-6ubuntu3
ii  libprotobuf-dev ... 3.21.12-8.2ubuntu0.3
ii  libspdlog-dev ... 1:1.12.0+ds-2build1
ii  nlohmann-json3-dev ... 3.11.3-1
ii  protobuf-compiler ... 3.21.12-8.2ubuntu0.3
ii  protobuf-compiler-grpc ... 1.51.1-4.1build5
ii  libgtest-dev ... 1.14.0-1
$ which protoc grpc_cpp_plugin cmake g++
/usr/bin/protoc
/usr/bin/grpc_cpp_plugin
/usr/bin/cmake
/usr/bin/g++
```

Every package was already present via `apt` (no network fetch even
needed), and the full stack — gRPC service definition, generated C++ code,
hiredis-backed Redis client, nlohmann::json serialization, spdlog logging,
GoogleTest — was built and exercised end to end (`docs/SETUP.md` has the
full transcript). **No tech-stack substitution was necessary for this
project.**

## Why "distributed" describes the API surface, not a running cluster

The resume line is "Distributed Search Engine (C++ | Redis | Grpc |
Multithreading)". This project implements a single-node search engine
exposed over a network RPC API (gRPC) — multiple independent client
processes, potentially on different machines, can index and query it
concurrently, which is the "distributed systems" surface most relevant to
a resume bullet about gRPC and multithreading. It does **not** implement
multi-node index sharding, replication, or a consensus protocol between
multiple `dse_server` instances.

This is a scope decision, not a limitation discovered late: building a
real multi-node sharded search cluster (consistent hashing of terms or
documents across nodes, a scatter-gather query coordinator, replica
consistency) is a multi-week distributed-systems project in its own right,
and the "0 to end, fully runnable" instruction this whole project series
was built under is better served by a complete, well-tested single-node
engine than a half-implemented cluster. The codebase is structured so
sharding could be added without a rewrite — see "What a production version
would add" below.

## Why a hand-rolled thread pool instead of a library

`ThreadPool` (`include/dse/thread_pool.hpp`, `src/thread_pool.cpp`) is
~40 lines of `std::thread`/`std::mutex`/`std::condition_variable`/
`std::queue<std::function<void()>>`, not a dependency like Intel TBB or
`boost::asio::thread_pool`. Two reasons:

1. The resume line explicitly calls out "Multithreading" as a skill being
   demonstrated — reaching for a library that hides the actual
   synchronization would undercut the point of the project.
2. It's genuinely simple to get right at this scale (fixed worker count,
   a single task queue, no work-stealing or priority scheduling needed),
   so the dependency isn't buying much: `Submit()` returns a
   `std::future<R>` via `std::packaged_task`, exactly like `std::async`
   would, but with a bounded, reusable worker pool instead of spawning a
   new OS thread per call.

## Why two separate thread pools (scoring vs. indexing)

`server_main.cpp` constructs two independent `ThreadPool` instances rather
than sharing one pool for both BM25 scoring and bulk-indexing fan-out. If
they shared a pool, a single large `IndexBatch` upload (say, 10,000
documents) could occupy every worker thread for the whole batch,
effectively pausing all concurrent search traffic until indexing
finished — the opposite of what a search engine should do under load
(reads should stay responsive even while writes are happening). Keeping
them separate costs two small thread pools (`DSE_SCORING_THREADS` +
`DSE_INDEXING_THREADS`, 4 each by default = 8 total worker threads) in
exchange for that isolation, which is a reasonable tradeoff at this
project's scale.

## Why `InvertedIndex` uses one `shared_mutex` instead of sharding

Every term and document lives behind a single `std::shared_mutex`. Reads
(the overwhelming majority of operations — every `Search` call reads the
index multiple times) take a shared lock and run fully concurrently with
each other; writes (`AddDocument`/`RemoveDocument`) take an exclusive lock
and briefly block all readers.

This is the simplest correct design, and it is a genuine bottleneck for a
production system: at high write throughput, every single-document
`AddDocument` call blocks every in-flight query, however briefly. A
production version would shard the index — e.g., hash `doc_id` (or the
term) across N independent `InvertedIndex` shards, each with its own
`shared_mutex`, so writes to one shard never block reads against another.
`BM25Scorer::ScoreAll` already has the right shape for this (it treats the
index as an opaque source of postings/document-length data), so sharding
would mostly be a change to how `InvertedIndex` itself is composed, not to
the scoring or query-engine layers above it.

`RemoveDocumentLocked`'s current implementation is also worth calling out:
removing a document walks every term in the vocabulary
(O(vocabulary size)) to find and erase its postings, rather than looking
up which terms that specific document contains (O(document's own term
count)). This is a deliberate simplicity-over-throughput tradeoff — the
straightforward fix is a second, forward index (`doc_id → set of terms`)
maintained alongside the postings, which was left out because
`RemoveDocument`/re-indexing is a much less frequent operation than
`Search` at this project's scale, and the forward index adds a second
data structure that must always be kept in sync with the first. A
production version handling frequent re-indexing (e.g. a live crawler
continuously updating documents) should add it.

## Why Redis caching degrades gracefully instead of failing hard

`RedisCache` is designed so that an unreachable Redis is never a hard
failure anywhere in the system: the constructor logs a warning (not an
exception) if no connection in its pool succeeds, `IsAvailable()` reports
this, and `Get`/`Set` become a permanent miss/no-op for that instance
rather than throwing. `QueryEngine` itself accepts a `nullptr` cache
pointer as a first-class configuration (fully uncached), and
`server_main.cpp` always constructs a real `RedisCache` but never treats a
failed connection as a reason to abort startup.

This was a deliberate choice, not an oversight: a result cache is
explicitly a performance optimization, not a correctness dependency — the
system computes the exact same BM25-ranked results whether or not the
cache is available, just faster on a hit. Treating an optional
dependency's outage as a hard failure (refusing to start the server, or
returning errors to search clients) would make the system less reliable,
not more. This is verified directly:
`QueryEngineTest.DegradesGracefullyWhenRedisIsUnreachable` and every test
in `tests/test_search_service_integration.cpp` run against a `RedisCache`
pointed at `localhost:6379` with nothing listening there, and all pass —
search still returns correct results, just always as a cache miss.

The alternative considered and rejected was hiredis's async API
(`hiredis/async.h`, typically paired with libevent or libuv), which would
let a single connection issue many concurrent commands without blocking a
thread per call. That's the right choice for a system issuing thousands of
cache operations per second; for this project's scale, a small
synchronous connection pool (default 4 connections, round-robin, one
mutex each) is simpler to reason about and sufficiently fast, at the cost
of one blocking syscall per cache operation per connection.

## Why BM25 title-weighting is "fold title tokens into body tokens" rather than field-weighted BM25F

`InvertedIndex::AddDocument` tokenizes the title and body separately, then
concatenates the token lists before building term frequencies — so a term
appearing in both the title and body of a document counts twice toward
that document's term frequency for that term, giving title matches a
mild, implicit relevance boost without any special-casing in the scorer.

A more accurate approach is BM25F, which tracks per-field term frequencies
and length normalization separately (with tunable per-field weights, e.g.
title terms worth 2x body terms) and combines them before applying the
IDF component. This project's simpler approach was chosen because it
requires no changes to `InvertedIndex`'s single-postings-list data model
or to `BM25Scorer`'s scoring loop — BM25F would need the postings
structure itself to track per-field frequencies (`doc_id → {field →
frequency}`), a larger structural change than this project's scope
warranted. The tradeoff and the BM25F alternative are noted here so it's
clear this was a conscious simplification, not a misunderstanding of BM25F.

## Fixed: Ubuntu 24.04's `libhiredis-dev` CMake config bug

While configuring the build, `find_package(Hiredis REQUIRED)` failed with:

```
CMake Error at .../HiredisConfigVersion.cmake:4 (string):
  string sub-command REPLACE requires at least four arguments.
```

Reading `/usr/lib/x86_64-linux-gnu/cmake/Hiredis/HiredisConfigVersion.cmake`
shows the bug directly: it calls
`string(REPLACE "." ";" REQUESTED_VERSION_COMPONENTS ${PACKAGE_FIND_VERSION})`
unconditionally, but `PACKAGE_FIND_VERSION` is only set by CMake when
`find_package(Hiredis <version>)` is called *with* a version — calling it
unversioned (`find_package(Hiredis REQUIRED)`, the normal usage) leaves
that variable empty, and `string(REPLACE ...)` on a missing argument is a
hard CMake error, not a soft failure. This is a packaging bug in the
`.deb`, not a gRPC/hiredis/CMake incompatibility.

**Fix applied** (`CMakeLists.txt`): locate hiredis via `pkg-config`
instead, which ships a correct `hiredis.pc`, and wrap the result in an
`ALIAS` target named `Hiredis::hiredis` so the rest of the build (`target_
link_libraries(dse_core PUBLIC ... Hiredis::hiredis)`) is unaffected by
which discovery mechanism found it:

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
add_library(Hiredis::hiredis ALIAS PkgConfig::HIREDIS)
```

Verified by a clean `cmake .. && cmake --build .` succeeding afterward
(full transcript in `docs/SETUP.md`).

## Test strategy

- **`test_tokenizer.cpp`** — pure-function tests: lowercasing, punctuation
  stripping, stopword/single-character filtering, `NormalizeToken`.
- **`test_inverted_index.cpp`** — add/get/remove correctness, re-indexing
  replaces rather than accumulates, stats computation, and a real
  multi-threaded stress test (8 threads × 25 documents each, added
  concurrently) that asserts every document ends up correctly counted and
  indexed — a functional concurrency check, not a substitute for running
  under ThreadSanitizer, but it does exercise the `shared_mutex` under
  genuine contention.
- **`test_bm25.cpp`** — ranking correctness (a document containing a query
  term outscores one that doesn't; higher term frequency scores higher,
  all else equal; an unknown query term contributes nothing rather than
  erroring), and a scaled test (200 documents, crossing the
  `kParallelScoringThreshold = 64` candidate threshold) asserting the
  parallel (`ThreadPool`-dispatched) and sequential scoring paths produce
  numerically identical scores.
- **`test_thread_pool.cpp`** — correctness of `Submit`'s future-based
  result delivery, exception propagation through the future, and a test
  that submits many short sleeping tasks and confirms more than one
  distinct OS thread actually executed them (real concurrency, not just
  "the API didn't crash").
- **`test_query_engine.cpp`** — end-to-end query behavior above the raw
  scorer: ranked ordering, empty/no-match queries, `top_k` truncation,
  non-empty snippets, and the Redis-unreachable degrade path (constructing
  a real `RedisCache` against `localhost:6379` with nothing listening, and
  confirming search still works, always as a cache miss).
- **`test_search_service_integration.cpp`** — the highest-fidelity test
  suite: each test starts a **real `grpc::Server`** bound to an ephemeral
  `127.0.0.1:0` port (letting the OS pick a free port, read back via
  `ServerBuilder::AddListeningPort`'s `selected_port` out-parameter), and
  drives it with a real `grpc::Channel`/stub over that port — exercising
  actual protobuf serialization and gRPC dispatch, not just calling the
  C++ methods directly. Covers `IndexDocument` (success and the
  empty-`doc_id` rejection path), `IndexBatch` (a 10-document stream plus
  one deliberately invalid entry, confirming it's counted as failed rather
  than aborting the stream), `Search`, and `GetStats`, including on an
  empty index.

All 37 tests pass in the environment this project was built and verified
in (`docs/SETUP.md` has the full transcript).

## What a production version would add

- **Multi-node sharding** — the biggest gap between this project and a
  literally "distributed" search engine, discussed above.
- **A forward index** (`doc_id → terms`) to make `RemoveDocument` O(that
  document's term count) instead of O(vocabulary size).
- **BM25F field weighting** instead of the current fold-title-into-body
  approximation.
- **Persistence** — the index is entirely in-memory; a restart loses all
  indexed documents. A production version would either periodically
  snapshot to disk or rebuild from an upstream source of truth on startup.
- **hiredis's async API** for the Redis cache, to avoid one blocking
  syscall per cache operation at high query-per-second scale.
- **Reconnection logic** in `RedisCache` — currently, a connection that
  drops mid-request is treated as a permanent miss for that call, but the
  pool doesn't attempt to re-establish a dropped connection on a later
  call; it's only ever (re-)connected in the constructor.
- **TLS** on the gRPC channel — the server currently uses
  `InsecureServerCredentials()`/`InsecureChannelCredentials()`, appropriate
  for this project's scope (a local/Docker-Compose deployment) but not for
  a network-exposed production service.
