cmake_minimum_required(VERSION 3.16)
project(FeedAgg VERSION 1.0.0 LANGUAGES CXX)

# ─────────────────────────────────────────────────────────────────────────────
# Global settings
# ─────────────────────────────────────────────────────────────────────────────

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Compiler / SIMD flags
# ─────────────────────────────────────────────────────────────────────────────

add_compile_options(
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Release>:-O3 -march=native -mavx2 -msse4.2>
  $<$<CONFIG:Debug>:-g -fsanitize=address>
)
add_link_options(
  $<$<CONFIG:Debug>:-fsanitize=address>
)

# ─────────────────────────────────────────────────────────────────────────────
# Dependencies
# ─────────────────────────────────────────────────────────────────────────────

find_package(Threads REQUIRED)

# spdlog (optional, via pkg-config)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(SPDLOG QUIET spdlog)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Header-only interface library
# ─────────────────────────────────────────────────────────────────────────────

add_library(feed_agg_headers INTERFACE)
target_include_directories(feed_agg_headers INTERFACE
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(feed_agg_headers INTERFACE cxx_std_17)

if(SPDLOG_FOUND)
  target_compile_definitions(feed_agg_headers INTERFACE FEED_HAVE_SPDLOG)
  target_include_directories(feed_agg_headers INTERFACE ${SPDLOG_INCLUDE_DIRS})
  target_link_libraries(feed_agg_headers INTERFACE ${SPDLOG_LIBRARIES})
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Core library (aggregator, simulator, metrics)
# ─────────────────────────────────────────────────────────────────────────────

add_library(feed_agg_core STATIC
  src/feed_simulator.cpp
  src/aggregator.cpp
  src/metrics.cpp
)
target_link_libraries(feed_agg_core PUBLIC
  feed_agg_headers
  Threads::Threads
  rt   # POSIX shared memory
)
target_compile_options(feed_agg_core PRIVATE
  $<$<CONFIG:Release>:-mavx2 -msse4.2>
)

# ─────────────────────────────────────────────────────────────────────────────
# Executables
# ─────────────────────────────────────────────────────────────────────────────

# Main aggregator process
add_executable(feed-agg src/main.cpp)
target_link_libraries(feed-agg PRIVATE feed_agg_core)

# Standalone SHM consumer
add_executable(feed-consumer src/consumer.cpp)
target_link_libraries(feed-consumer PRIVATE feed_agg_headers Threads::Threads rt)

# ─────────────────────────────────────────────────────────────────────────────
# Install
# ─────────────────────────────────────────────────────────────────────────────

include(GNUInstallDirs)
install(TARGETS feed-agg feed-consumer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/feed_agg DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ─────────────────────────────────────────────────────────────────────────────
# Summary
# ─────────────────────────────────────────────────────────────────────────────

message(STATUS "FeedAgg ${PROJECT_VERSION}")
message(STATUS "  Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "  AVX2/SSE42 : enabled in Release mode")
message(STATUS "  spdlog     : ${SPDLOG_FOUND}")
