cmake_minimum_required(VERSION 3.20)
project(stocks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# cellui is not pinned: the two are developed together, so this follows its
# main branch (re-run cmake to update). To work against a local checkout:
#   cmake -B build -DFETCHCONTENT_SOURCE_DIR_CELLUI=/path/to/cellui
# The window needs GTK 4.10 or later where there is one (Linux); without it
# stocks is terminal only, and `--ui window` says so.
if(NOT WIN32 AND NOT APPLE AND NOT DEFINED CELLUI_ENABLE_GTK)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(STOCKS_GTK4 QUIET gtk4>=4.10)
  endif()
  if(STOCKS_GTK4_FOUND)
    set(CELLUI_ENABLE_GTK ON)
  endif()
endif()

include(FetchContent)
FetchContent_Declare(cellui
  GIT_REPOSITORY https://github.com/rekola/cellui
  GIT_TAG main)
FetchContent_MakeAvailable(cellui)

# The application. For now it shows cellui's widgets over made-up rows; there
# is no data model yet.
add_executable(stocks src/main.cpp)
target_link_libraries(stocks PRIVATE cellui)

enable_testing()
# The program starts, reads its command line through cellui and says what is wrong with it.
add_test(NAME stocks_rejects_an_unknown_ui COMMAND stocks --ui nonesuch)
set_tests_properties(stocks_rejects_an_unknown_ui PROPERTIES PASS_REGULAR_EXPRESSION "unknown --ui")
