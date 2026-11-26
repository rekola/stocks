# The market-data server and its ingest (plans/server.md). Included from the top-level file. Libraries, all pinned and open source:
#   cpp-httplib  the server's HTTP (MIT, one header)
#   miniz        reads the zip files a dataset downloads (MIT)
#   sqlpp11      typed SQL with prepared statements over SQLite and MariaDB, one interface (BSD 2-clause), and HowardHinnant's date, which
#                it fetches itself (MIT)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
  GIT_TAG v0.18.3)
FetchContent_MakeAvailable(httplib)

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)
# miniz asks for CMake 3.0, which CMake 4 refuses.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz
  GIT_TAG 3.0.2)
FetchContent_MakeAvailable(miniz)

# sqlpp11 is headers only (its connectors too), so its sources are fetched and not built as a project: its install rules would want
# SQLite's target exported with it. A target with the headers, and Howard Hinnant's date (MIT) that it includes, then each connector is
# that and its client library: SQLite is the one SQLiteCpp builds and names SQLite::SQLite3 (so the program has one copy of it), MariaDB
# is the system's libmariadb where there is one (then the "mariadb" database type works too).
FetchContent_Declare(sqlpp11
  GIT_REPOSITORY https://github.com/rbock/sqlpp11
  GIT_TAG 0.64
  SOURCE_SUBDIR no_cmake_project_here)
FetchContent_MakeAvailable(sqlpp11)
FetchContent_Declare(date
  GIT_REPOSITORY https://github.com/HowardHinnant/date
  GIT_TAG v3.0.0
  SOURCE_SUBDIR no_cmake_project_here)
FetchContent_MakeAvailable(date)
if(NOT TARGET SQLite::SQLite3)
  message(FATAL_ERROR "SQLiteCpp's SQLite::SQLite3 target is missing")
endif()
add_library(sqlpp11_headers INTERFACE)
target_include_directories(sqlpp11_headers SYSTEM INTERFACE ${sqlpp11_SOURCE_DIR}/include ${date_SOURCE_DIR}/include)
target_compile_features(sqlpp11_headers INTERFACE cxx_std_20)
add_library(sqlpp11_sqlite3 INTERFACE)
target_link_libraries(sqlpp11_sqlite3 INTERFACE sqlpp11_headers SQLite::SQLite3)
add_library(sqlpp11_mariadb INTERFACE)
if(MARIADB_FOUND)
  target_link_libraries(sqlpp11_mariadb INTERFACE sqlpp11_headers PkgConfig::MARIADB)
endif()

# Everything provider-free: bars, the stores (SQL over SQLite and MariaDB, and one in memory), the raw file cache, the ingest and the API.
add_library(market_lib STATIC Bars.cpp Zip.cpp RawCache.cpp MemoryStore.cpp SqliteStore.cpp SqlStores.cpp Config.cpp Ingest.cpp Api.cpp)
target_include_directories(market_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(market_lib PUBLIC cxx_std_20)
target_link_libraries(market_lib PUBLIC stocks_model stocks_http miniz nlohmann_json::nlohmann_json httplib::httplib
  sqlpp11_sqlite3)
if(MARIADB_FOUND)
  target_sources(market_lib PRIVATE MariaDbStore.cpp)
  target_link_libraries(market_lib PUBLIC sqlpp11_mariadb)
  target_compile_definitions(market_lib PUBLIC MARKET_WITH_MARIADB)
endif()

# The offline datasets (market/offline/): loaded when market-ingest is asked, apart from the sources of src/source/ that run beside the
# browser. The only code in these targets that knows a provider.
add_library(market_offline STATIC offline/HistData.cpp)
target_link_libraries(market_offline PUBLIC market_lib)

add_executable(market-ingest market_ingest_main.cpp)
target_link_libraries(market-ingest PRIVATE market_lib market_offline)
add_executable(market-server market_server_main.cpp)
target_link_libraries(market-server PRIVATE market_lib)
set_target_properties(market-ingest market-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# The tests: the parser, the shift, the keys, the aggregation, the ingest with a fake site and the API. They run on SQLite always, and on
# MariaDB too where MARKET_TEST_DB_USER is set (see tests/MarketTestSupport.h).
set(MARKET_TESTS ${PROJECT_SOURCE_DIR}/tests)
add_executable(market_tests ${MARKET_TESTS}/test_main.cpp ${MARKET_TESTS}/MarketBarsTests.cpp ${MARKET_TESTS}/MarketZipTests.cpp
  ${MARKET_TESTS}/HistDataTests.cpp ${MARKET_TESTS}/MarketIngestTests.cpp ${MARKET_TESTS}/MarketApiTests.cpp ${MARKET_TESTS}/MarketStoreTests.cpp)
target_link_libraries(market_tests PRIVATE market_lib market_offline SQLiteCpp)  # the tests of a schema change a file with it
add_test(NAME market_tests COMMAND market_tests)
set_tests_properties(market_tests PROPERTIES TIMEOUT 120)
