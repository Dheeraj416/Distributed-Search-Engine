# Distributed Search Engine — top-level build.
#
# Layout:
#   proto/search.proto  -> generated into ${CMAKE_CURRENT_BINARY_DIR}/generated
#   include/dse/*.hpp    -> public headers for the "dse_core" library
#   src/*.cpp            -> dse_core sources (everything except the two
#                            entrypoints and tests)
#   src/server_main.cpp  -> dse_server executable
#   src/client_main.cpp  -> dse_client executable
#   tests/*.cpp           -> dse_tests executable (GoogleTest)
#
# See docs/SETUP.md for the exact commands to configure/build/test/run.
cmake_minimum_required(VERSION 3.16)
project(distributed_search_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(DSE_ENABLE_WARNINGS "Build with -Wall -Wextra" ON)

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(GTest REQUIRED)

# Hiredis ships a CMake config (HiredisConfigVersion.cmake) on this
# distro that is broken when no version is requested (a CMake
# `string(REPLACE ...)` call on an empty PACKAGE_FIND_VERSION aborts
# configure). pkg-config's hiredis.pc is correct and stable, so use that
# path instead and wrap it in the same Hiredis::hiredis imported target
# name the rest of this file expects.
find_package(PkgConfig REQUIRED)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)
if(NOT TARGET Hiredis::hiredis)
  add_library(Hiredis::hiredis ALIAS PkgConfig::HIREDIS)
endif()

# --- Protocol Buffers / gRPC code generation -------------------------------
# Generated into the build directory (not checked in) so `git status` never
# shows stale generated code, and a clean `cmake --build` always regenerates
# from proto/search.proto.
set(PROTO_FILE ${CMAKE_CURRENT_SOURCE_DIR}/proto/search.proto)
set(GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

set(PROTO_SRCS ${GENERATED_DIR}/search.pb.cc)
set(PROTO_HDRS ${GENERATED_DIR}/search.pb.h)
set(GRPC_SRCS ${GENERATED_DIR}/search.grpc.pb.cc)
set(GRPC_HDRS ${GENERATED_DIR}/search.grpc.pb.h)

find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

add_custom_command(
  OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
  COMMAND protobuf::protoc
  ARGS
    --cpp_out=${GENERATED_DIR}
    --grpc_out=${GENERATED_DIR}
    --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
    -I ${CMAKE_CURRENT_SOURCE_DIR}/proto
    ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
  COMMENT "Generating protobuf/gRPC C++ code from search.proto"
  VERBATIM)

add_library(dse_proto STATIC ${PROTO_SRCS} ${GRPC_SRCS})
target_include_directories(dse_proto PUBLIC ${GENERATED_DIR})
target_link_libraries(dse_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

# --- Core library (everything except the two main()s) ----------------------
add_library(dse_core STATIC
  src/tokenizer.cpp
  src/inverted_index.cpp
  src/bm25.cpp
  src/thread_pool.cpp
  src/redis_cache.cpp
  src/query_engine.cpp
  src/search_service_impl.cpp
)
target_include_directories(dse_core PUBLIC include)
target_link_libraries(dse_core
  PUBLIC
    dse_proto
    Threads::Threads
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    Hiredis::hiredis
)

if(DSE_ENABLE_WARNINGS)
  target_compile_options(dse_core PRIVATE -Wall -Wextra)
endif()

# --- Executables -------------------------------------------------------------
add_executable(dse_server src/server_main.cpp)
target_link_libraries(dse_server PRIVATE dse_core)

add_executable(dse_client src/client_main.cpp)
target_link_libraries(dse_client PRIVATE dse_core)

# --- Tests -------------------------------------------------------------------
enable_testing()

add_executable(dse_tests
  tests/test_tokenizer.cpp
  tests/test_inverted_index.cpp
  tests/test_bm25.cpp
  tests/test_thread_pool.cpp
  tests/test_query_engine.cpp
  tests/test_search_service_integration.cpp
)
target_link_libraries(dse_tests
  PRIVATE
    dse_core
    GTest::gtest
    GTest::gtest_main
)
target_include_directories(dse_tests PRIVATE include)

include(GoogleTest)
gtest_discover_tests(dse_tests)
