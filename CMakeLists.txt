cmake_minimum_required(VERSION 3.20)
project(stocks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# cellui at a pinned commit. To work against a local checkout instead:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_CELLUI=/path/to/cellui
include(FetchContent)
FetchContent_Declare(cellui
  GIT_REPOSITORY https://github.com/rekola/cellui
  GIT_TAG a29230b380d1c77de5b20ef3d0167c1530f43f58)
FetchContent_MakeAvailable(cellui)

# The application. For now it shows cellui's widgets over made-up rows; there
# is no data model yet.
add_executable(stocks src/main.cpp)
target_link_libraries(stocks PRIVATE cellui)
