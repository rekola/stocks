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

# immer: persistent (structure-sharing) vectors and maps, header-only, Boost licence. The store is an immutable value
# built from them. Pinned: a new release is a deliberate bump.
set(immer_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(immer_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(immer_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(immer_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(immer
  GIT_REPOSITORY https://github.com/arximboldi/immer
  GIT_TAG v0.8.1)
FetchContent_MakeAvailable(immer)

# httpclient: one HTTP interface over WinHTTP and libcurl. It has no build system of its own, so the sources it needs
# are compiled here. Pinned to a commit.
FetchContent_Declare(httpclient
  GIT_REPOSITORY https://github.com/rekola/httpclient
  GIT_TAG fbf8ae2b2f6fd4fcc6b0f0adf68b6c77671e1f62)
FetchContent_MakeAvailable(httpclient)
if(WIN32)
  add_library(stocks_http STATIC ${httpclient_SOURCE_DIR}/src/WinHTTPClient.cpp ${httpclient_SOURCE_DIR}/src/URI.cpp)
  target_link_libraries(stocks_http PUBLIC winhttp)
else()
  find_package(CURL REQUIRED)
  add_library(stocks_http STATIC ${httpclient_SOURCE_DIR}/src/CurlClient.cpp)
  target_link_libraries(stocks_http PUBLIC CURL::libcurl)
endif()
target_include_directories(stocks_http PUBLIC ${httpclient_SOURCE_DIR}/include)
target_compile_features(stocks_http PUBLIC cxx_std_17)

# nlohmann/json: reads the providers' answers. Header-only; pinned.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

# The data model: plain values and the Store they live in. Knows nothing of the screen.
add_library(stocks_model STATIC src/model/MemoryStore.cpp)
target_include_directories(stocks_model PUBLIC src)
target_compile_features(stocks_model PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(stocks_model PUBLIC immer Threads::Threads)

# The browser: the screen, its commands and the rows it shows, as a library so the tests can drive it.
add_library(stocks_lib STATIC src/Browser.cpp src/Quotes.cpp src/Synthetic.cpp src/source/Http.cpp src/source/Universe.cpp
  src/source/YahooSource.cpp src/source/SourceRunner.cpp)
target_include_directories(stocks_lib PUBLIC src)
target_link_libraries(stocks_lib PUBLIC cellui stocks_model stocks_http nlohmann_json::nlohmann_json)
target_compile_definitions(stocks_lib PUBLIC STOCKS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")

add_executable(stocks src/main.cpp)
target_link_libraries(stocks PRIVATE stocks_lib)

enable_testing()
# The program starts, reads its command line through cellui and says what is wrong with it.
add_test(NAME stocks_rejects_an_unknown_ui COMMAND stocks --ui nonesuch)
set_tests_properties(stocks_rejects_an_unknown_ui PROPERTIES PASS_REGULAR_EXPRESSION "unknown --ui")

# The browser on a test display: keys in, what it shows out. cellui's test display
# (TestBackend) is built where there is a POSIX event loop, so not on Windows.
if(NOT WIN32)
  add_executable(stocks_tests tests/test_main.cpp tests/BrowserTests.cpp tests/ModelTests.cpp tests/SourceTests.cpp)
  target_link_libraries(stocks_tests PRIVATE stocks_lib)
  add_test(NAME stocks_tests COMMAND stocks_tests)
  set_tests_properties(stocks_tests PROPERTIES TIMEOUT 60)
endif()
