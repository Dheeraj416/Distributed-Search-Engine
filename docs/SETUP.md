# Setup Guide

## Option A: Docker Compose (recommended)

Requires Docker and Docker Compose.

```bash
docker compose up --build
```

This builds the server image (a multi-stage build — see `Dockerfile`) and
starts two containers: `dse-redis` (Redis 7) and `dse-server` (this
project, listening on `50051`, waiting for Redis's healthcheck before it
starts). The compose file was validated with `docker compose config`
against this exact `docker-compose.yml`; the sandbox this project was
built in has no Docker daemon available to run an actual `docker build`,
so the image itself has not been built end-to-end here — the CMake build
below is what was used for the full compile-and-test verification, and the
Dockerfile mirrors those same CMake commands and library set.

Once it's up, build the CLI client locally (see Option B below, `dse_client`
only needs `cmake --build build --target dse_client`) and point it at the
container:

```bash
./build/dse_client index localhost:50051 data/sample_corpus.json
./build/dse_client search localhost:50051 "distributed search engine" 5
./build/dse_client stats localhost:50051
```

Stop everything with `docker compose down` (add `-v` to also drop the
Redis data volume).

## Option B: Build locally with CMake

### Prerequisites

On Ubuntu/Debian, everything needed is available via `apt`:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake pkg-config \
  protobuf-compiler protobuf-compiler-grpc \
  libprotobuf-dev libgrpc++-dev libgrpc-dev \
  libhiredis-dev libspdlog-dev nlohmann-json3-dev libgtest-dev
```

Verify the toolchain (this is the exact check this project's build was
verified against):

```bash
$ g++ --version | head -1
g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0
$ cmake --version | head -1
cmake version 3.28.3
$ protoc --version
libprotoc 3.21.12
$ which grpc_cpp_plugin
/usr/bin/grpc_cpp_plugin
```

### Configure and build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc)"
```

This produces three binaries in `build/`: `dse_server`, `dse_client`, and
`dse_tests`. It also generates `search.pb.{h,cc}` and
`search.grpc.pb.{h,cc}` from `proto/search.proto` into
`build/generated/` — these are build artifacts, never checked into the
repository, so a clean build always regenerates them from the one source
of truth (the `.proto` file).

**Known packaging quirk (documented in `docs/DESIGN_DECISIONS.md`):** this
project's `CMakeLists.txt` locates hiredis via `pkg-config` rather than
`find_package(Hiredis)`, because the Ubuntu 24.04 `libhiredis-dev` package
ships a `HiredisConfigVersion.cmake` that throws a CMake configure-time
error when no version is requested. If you see
`CMake Error ... HiredisConfigVersion.cmake ... string sub-command REPLACE
requires at least four arguments`, you're hitting exactly this — the
`pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)` call in
`CMakeLists.txt` is the fix already applied here.

### Run the tests

```bash
cd build
ctest --output-on-failure
```

Expected output (the actual transcript from this project's verification):

```
100% tests passed, 0 tests failed out of 37

Total Test time (real) =   0.33 sec
```

Or run the GoogleTest binary directly for more detail per-test:

```bash
./build/dse_tests
```

### Run the server

```bash
# Defaults to REDIS_HOST=localhost, REDIS_PORT=6379, port 50051.
# If no Redis is running, the server logs a warning and runs uncached —
# this is not a startup failure (see docs/DESIGN_DECISIONS.md).
./build/dse_server
```

Environment variables (all optional):

| Variable | Default | Meaning |
|---|---|---|
| `DSE_BIND_ADDRESS` | `0.0.0.0:50051` (or `0.0.0.0:$DSE_PORT`) | Full bind address, overrides `DSE_PORT` if set. |
| `DSE_PORT` | `50051` | Port to bind, used to build the default `DSE_BIND_ADDRESS`. |
| `REDIS_HOST` | `localhost` | Redis host for the result cache. |
| `REDIS_PORT` | `6379` | Redis port. |
| `DSE_SCORING_THREADS` | `4` | Worker threads for parallel BM25 scoring. |
| `DSE_INDEXING_THREADS` | `4` | Worker threads for `IndexBatch` fan-out. |

### Run the CLI client

```bash
# Bulk-index the bundled sample corpus (12 short technical documents):
./build/dse_client index localhost:50051 data/sample_corpus.json

# Search:
./build/dse_client search localhost:50051 "distributed search engine" 5
./build/dse_client search localhost:50051 "thread pool concurrency" 3

# Index stats:
./build/dse_client stats localhost:50051
```

This was run end-to-end against a locally started `dse_server` as part of
verifying this project; a representative excerpt:

```
$ ./build/dse_client index localhost:50099 data/sample_corpus.json
Sent 12 document(s).
Indexed: 12
Failed:  0
Elapsed: 0.82 ms

$ ./build/dse_client search localhost:50099 "distributed search engine" 5
Query: "distributed search engine"  (12 match(es), 0.05 ms, cache MISS)

1. [0.9152] Introduction to Distributed Systems  (doc-001)
   https://example-notes.dev/distributed-systems-intro
   A distributed system is a collection of independent computers …
...

$ ./build/dse_client stats localhost:50099
Total documents:      12
Total distinct terms: 328
Total postings:       456
Average doc length:   45.17 tokens
Approx index size:    18355 bytes
```

## Indexing your own corpus

Any JSON file matching this shape works with `dse_client index`:

```json
[
  {
    "doc_id": "unique-id",
    "title": "Document Title",
    "url": "https://example.com/some-page",
    "body": "The full text to index and search over."
  }
]
```

## Troubleshooting

- **`Failed to start gRPC server on 0.0.0.0:50051 — is the port already in
  use?`** — another `dse_server` instance (or something else) is already
  bound to that port. Set `DSE_PORT` to a different value.
- **`RedisCache: could not connect to redis at ...`** at startup — this is
  a warning, not a fatal error. Search still works, just without caching.
  Start Redis (`redis-server`, or `docker compose up redis`) if you want
  caching.
- **`gtest_discover_tests` fails during build/configure** — this happens
  if `ctest`/CMake can't execute `dse_tests` at configure time (e.g. a
  cross-compilation setup). Not applicable to the standard native build
  covered above.
