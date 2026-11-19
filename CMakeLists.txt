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

# The browser: the screen, its commands and the made-up rows, as a library so the tests can drive it.
add_library(stocks_lib STATIC src/Browser.cpp src/Quotes.cpp)
target_include_directories(stocks_lib PUBLIC src)
target_link_libraries(stocks_lib PUBLIC cellui)

add_executable(stocks src/main.cpp)
target_link_libraries(stocks PRIVATE stocks_lib)

enable_testing()
# The program starts, reads its command line through cellui and says what is wrong with it.
add_test(NAME stocks_rejects_an_unknown_ui COMMAND stocks --ui nonesuch)
set_tests_properties(stocks_rejects_an_unknown_ui PROPERTIES PASS_REGULAR_EXPRESSION "unknown --ui")

# The browser on a test display: keys in, what it shows out. cellui's test display
# (TestBackend) is built where there is a POSIX event loop, so not on Windows.
if(NOT WIN32)
  add_executable(stocks_tests tests/test_main.cpp tests/BrowserTests.cpp)
  target_link_libraries(stocks_tests PRIVATE stocks_lib)
  add_test(NAME stocks_tests COMMAND stocks_tests)
  set_tests_properties(stocks_tests PROPERTIES TIMEOUT 60)
endif()
