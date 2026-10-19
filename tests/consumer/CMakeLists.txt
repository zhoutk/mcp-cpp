# Installed-package consumer smoke test (CI: `install-consumer` job).
#
# Builds against an INSTALLED mcp package only - no add_subdirectory, no
# BUILD_INTERFACE - so it catches missing INTERFACE link dependencies in
# mcpTargets.cmake (P0-3: ws2_32 / Threads were absent, so HTTP consumers
# failed to link) and a stale package version (P0-4).
cmake_minimum_required(VERSION 3.18)
project(mcp-consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Versioned lookup: SameMajorVersion accepts 0.2.x, and rejects the 0.1.0
# project version the package used to declare.
find_package(mcp 0.2 CONFIG REQUIRED)

if(NOT mcp_VERSION)
  message(FATAL_ERROR "mcp_VERSION is not set by mcpConfigVersion.cmake")
endif()

add_executable(mcp_consumer main.cpp)
target_link_libraries(mcp_consumer PRIVATE mcp::mcp)

enable_testing()
add_test(NAME mcp_consumer_run COMMAND mcp_consumer)
