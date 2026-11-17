cmake_minimum_required(VERSION 3.20)
project(stocks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# cellui is not pinned: the two are developed together, so this follows its
# main branch (re-run cmake to update). To work against a local checkout:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_CELLUI=/path/to/cellui
include(FetchContent)
FetchContent_Declare(cellui
  GIT_REPOSITORY https://github.com/rekola/cellui
  GIT_TAG main)
FetchContent_MakeAvailable(cellui)

# The application. For now it shows cellui's widgets over made-up rows; there
# is no data model yet.
add_executable(stocks src/main.cpp)
target_link_libraries(stocks PRIVATE cellui)
