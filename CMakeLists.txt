cmake_minimum_required(VERSION 3.20)

project(tradebot
  VERSION 0.1.0
  DESCRIPTION "Event-driven cryptocurrency trading research system"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

option(TRADEBOT_BUILD_TESTS "Build unit tests" ON)
option(TRADEBOT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(TRADEBOT_ENABLE_SANITIZERS "Enable ASan/UBSan (Debug builds)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# Interface target carrying project-wide compile settings.
add_library(tradebot_options INTERFACE)
tradebot_set_warnings(tradebot_options ${TRADEBOT_WARNINGS_AS_ERRORS})
if(TRADEBOT_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  tradebot_enable_sanitizers(tradebot_options)
endif()

# ---------------------------------------------------------------------------
# Third-party
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

# Header-only vendored libraries live under third_party/ and are exposed as
# SYSTEM includes so project warning flags do not apply to them.
add_library(tradebot_third_party INTERFACE)
target_include_directories(tradebot_third_party SYSTEM INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_library(doctest INTERFACE)
target_link_libraries(doctest INTERFACE tradebot_third_party)

# ---------------------------------------------------------------------------
# Project sources
# ---------------------------------------------------------------------------
add_subdirectory(src)
add_subdirectory(tools)

if(TRADEBOT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
