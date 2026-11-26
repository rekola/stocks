# The market-data server and its ingest (plans/server.md). Included from the top-level file where the MariaDB client library and zlib
# are found. cpp-httplib: one header, MIT licence (THIRD_PARTY_LICENSES.md), the server's HTTP; pinned.
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
  GIT_REPOSITORY https://github.com/yhirose/cpp-httplib
  GIT_TAG v0.18.3)
FetchContent_MakeAvailable(httplib)

# Everything provider-free: bars, the store (MariaDB, and one in memory), the raw file cache, the ingest and the API.
add_library(market_lib STATIC Bars.cpp Zip.cpp RawCache.cpp MemoryStore.cpp MariaDbStore.cpp Config.cpp Ingest.cpp Api.cpp)
target_include_directories(market_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(market_lib PUBLIC cxx_std_20)
target_link_libraries(market_lib PUBLIC stocks_model stocks_http ZLIB::ZLIB PkgConfig::MARIADB nlohmann_json::nlohmann_json httplib::httplib)

# The adapter for HistData.com (src/source/): the only code in these targets that knows a provider.
add_library(market_histdata STATIC ${PROJECT_SOURCE_DIR}/src/source/HistData.cpp)
target_link_libraries(market_histdata PUBLIC market_lib)

add_executable(market-ingest market_ingest_main.cpp)
target_link_libraries(market-ingest PRIVATE market_lib market_histdata)
add_executable(market-server market_server_main.cpp)
target_link_libraries(market-server PRIVATE market_lib)
set_target_properties(market-ingest market-server PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# The tests: the parser, the shift, the keys, the aggregation, the ingest with a fake site and the API. The ones that need a MariaDB
# server (the store and the API over it) run where MARKET_TEST_DB_USER is set (see tests/MarketTestDb.h) and say so where it is not.
# Until the test files are in (work in progress), the target is only made where they are.
if(EXISTS ${PROJECT_SOURCE_DIR}/tests/MarketBarsTests.cpp)
  set(MARKET_TESTS ${PROJECT_SOURCE_DIR}/tests)
  add_executable(market_tests ${MARKET_TESTS}/test_main.cpp ${MARKET_TESTS}/MarketBarsTests.cpp ${MARKET_TESTS}/MarketZipTests.cpp
    ${MARKET_TESTS}/HistDataTests.cpp ${MARKET_TESTS}/MarketIngestTests.cpp ${MARKET_TESTS}/MarketApiTests.cpp ${MARKET_TESTS}/MarketStoreTests.cpp)
  target_link_libraries(market_tests PRIVATE market_lib market_histdata)
  add_test(NAME market_tests COMMAND market_tests)
  set_tests_properties(market_tests PROPERTIES TIMEOUT 120)
endif()
