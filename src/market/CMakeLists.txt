# The market-data server and its ingest (plans/server.md). Included from the top-level file. Both run on the application's own Store
# (src/model): what they keep is kept by the model's stores, in SQLite files or, where libmariadb was found, in MariaDB (src/model/MariaDbStores.h).
# Libraries, all pinned and open source:
#   cpp-httplib  the server's HTTP (MIT, one header)
#   miniz        reads the zip files a dataset downloads (MIT)
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

# Everything provider-free: the bars, the raw file cache, the ingest and the API.
add_library(market_lib STATIC Bars.cpp Zip.cpp RawCache.cpp Backends.cpp Config.cpp Ingest.cpp Api.cpp)
target_include_directories(market_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(market_lib PUBLIC cxx_std_20)
target_link_libraries(market_lib PUBLIC stocks_model stocks_nodes stocks_http miniz nlohmann_json::nlohmann_json httplib::httplib)
if(TARGET stocks_mariadb)
  target_link_libraries(market_lib PUBLIC stocks_mariadb)
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

# The tests: the keys, the aggregation, the parser, the shift, the ingest with a fake site and the API, on SQLite files, and on MariaDB too
# where MARKET_TEST_DB_USER is set (see tests/MarketTestSupport.h).
set(MARKET_TESTS ${PROJECT_SOURCE_DIR}/tests)
add_executable(market_tests ${MARKET_TESTS}/test_main.cpp ${MARKET_TESTS}/MarketBarsTests.cpp ${MARKET_TESTS}/MarketZipTests.cpp
  ${MARKET_TESTS}/HistDataTests.cpp ${MARKET_TESTS}/MarketIngestTests.cpp ${MARKET_TESTS}/MarketApiTests.cpp)
target_link_libraries(market_tests PRIVATE market_lib market_offline)
add_test(NAME market_tests COMMAND market_tests)
set_tests_properties(market_tests PROPERTIES TIMEOUT 120)
