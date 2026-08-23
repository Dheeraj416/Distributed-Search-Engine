# Multi-stage build: a full apt-get + compiler toolchain in the build
# stage, but the final image only carries the shared libraries needed at
# runtime and the two compiled binaries — keeps the shipped image small
# and avoids exposing build tools in production.

# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    protobuf-compiler \
    protobuf-compiler-grpc \
    libprotobuf-dev \
    libgrpc++-dev \
    libgrpc-dev \
    libhiredis-dev \
    libspdlog-dev \
    nlohmann-json3-dev \
    libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY proto ./proto
COPY include ./include
COPY src ./src
COPY tests ./tests

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDSE_ENABLE_WARNINGS=OFF \
    && cmake --build build -j"$(nproc)" --target dse_server dse_client

# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# Runtime-only shared libraries (no -dev headers, no compilers).
RUN apt-get update && apt-get install -y --no-install-recommends \
    libprotobuf32t64 \
    libgrpc++1.51t64 \
    libgrpc29t64 \
    libhiredis1.1.0 \
    libspdlog1.12 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/dse_server ./dse_server
COPY --from=build /src/build/dse_client ./dse_client
COPY data ./data

RUN useradd --system --create-home --shell /usr/sbin/nologin dse
USER dse

ENV DSE_PORT=50051 \
    REDIS_HOST=redis \
    REDIS_PORT=6379 \
    DSE_SCORING_THREADS=4 \
    DSE_INDEXING_THREADS=4

EXPOSE 50051

ENTRYPOINT ["./dse_server"]
