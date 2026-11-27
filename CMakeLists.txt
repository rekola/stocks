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

# httpclient: one HTTP interface over WinHTTP and libcurl (Linux, macOS: libcurl; Windows: WinHTTP). It folds host
# names with utf8proc or libunistring, and takes whichever is installed (libunistring is, for cellui). Pinned.
FetchContent_Declare(httpclient
  GIT_REPOSITORY https://github.com/rekola/httpclient
  GIT_TAG 1c0b44efb1ae71e7ae923ae45050212790d3d301)
FetchContent_MakeAvailable(httpclient)

# nlohmann/json: reads the providers' answers. Header-only; pinned.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

# pugixml: reads the Bank of Finland's XML report. Small, one source file, MIT licence (THIRD_PARTY_LICENSES.md); pinned.
set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PUGIXML_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(pugixml
  GIT_REPOSITORY https://github.com/zeux/pugixml
  GIT_TAG v1.14)
FetchContent_MakeAvailable(pugixml)

# SQLiteCpp: RAII C++ wrapper over SQLite (MIT), built with its bundled SQLite amalgamation (public domain) so no system
# library is needed on any platform. Keeps the node attributes between runs (src/model/SqliteFactStore.cpp, the one
# place that names it). Pinned to 3.4.0 (21 September 2026); a new release is a deliberate bump.
set(SQLITECPP_INTERNAL_SQLITE ON CACHE BOOL "" FORCE)
set(SQLITECPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
set(SQLITECPP_RUN_CPPCHECK OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INSTALL OFF CACHE BOOL "" FORCE)
set(SQLITECPP_INCLUDE_SCRIPT OFF CACHE BOOL "" FORCE)
set(SQLITE_OMIT_LOAD_EXTENSION ON CACHE BOOL "" FORCE)
FetchContent_Declare(SQLiteCpp
  GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp
  GIT_TAG df0a28ca9733f687ab7ce77f070c0df84368a45e)  # 3.4.0
FetchContent_MakeAvailable(SQLiteCpp)

# The data model: plain values and the Store they live in. Knows nothing of the screen.
add_library(stocks_model STATIC src/model/MemoryStore.cpp src/model/Isin.cpp src/model/Period.cpp src/model/NodeKey.cpp src/model/LegacyNodeKey.cpp
  src/model/FactWriter.cpp src/model/SqliteFactStore.cpp src/model/SummaryRecords.cpp src/model/SummaryWriter.cpp src/model/SqliteSummaryStore.cpp src/portfolio/Portfolio.cpp src/portfolio/Transaction.cpp)
target_include_directories(stocks_model PUBLIC src)
target_compile_features(stocks_model PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(stocks_model PUBLIC immer Threads::Threads)
# Private: no header names SQLiteCpp.
target_link_libraries(stocks_model PRIVATE SQLiteCpp nlohmann_json::nlohmann_json)

# The browser: the screen, its commands and the rows it shows, as a library so the tests can drive it.
# The blocking GET and POST over httpclient, apart from the browser so that the market-data programs need no window toolkit.
add_library(stocks_http STATIC src/source/Http.cpp)
target_include_directories(stocks_http PUBLIC src)
target_link_libraries(stocks_http PUBLIC httpclient::httpclient)

add_library(stocks_lib STATIC src/Browser.cpp src/BrowserPortfolio.cpp src/Positions.cpp src/News.cpp src/AttributeView.cpp src/TransactionsView.cpp src/Quotes.cpp src/Synthetic.cpp src/source/InstrumentLists.cpp
  src/icons/IconLoader.cpp src/history/HistoryLoader.cpp src/source/NordnetHistory.cpp src/source/YahooSource.cpp src/source/YahooHistory.cpp src/source/NordnetSource.cpp src/source/SourceRunner.cpp src/source/DatasetCodes.cpp src/source/AutoSource.cpp
  src/source/DataDirectory.cpp src/source/FundRows.cpp src/source/FundSource.cpp src/source/NordnetDetails.cpp src/source/Countries.cpp src/source/KnownMarkets.cpp src/source/NordnetNews.cpp src/source/SuomenPankkiSource.cpp src/source/EcbSource.cpp src/source/PhysicalSource.cpp src/source/GoldApiSource.cpp src/source/GoldPriceSource.cpp src/source/GoldPriceHistory.cpp src/source/SifterSource.cpp)
target_include_directories(stocks_lib PUBLIC src)
target_link_libraries(stocks_lib PUBLIC cellui stocks_model stocks_http httpclient::httpclient nlohmann_json::nlohmann_json pugixml::pugixml)
target_compile_definitions(stocks_lib PUBLIC STOCKS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
  STOCKS_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_executable(stocks src/main.cpp)

# The data the program ships with (data/lists, data/funds): a copy beside the program in the build directory, and in share/stocks when installed
# (the program is found by its own path, so either place works wherever it is run from).
add_custom_target(stocks_data ALL COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data $<TARGET_FILE_DIR:stocks>/data
                  COMMENT "Copying data beside the program")
add_dependencies(stocks stocks_data)
include(GNUInstallDirs)
install(TARGETS stocks RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY data/ DESTINATION ${CMAKE_INSTALL_DATADIR}/stocks)
target_link_libraries(stocks PRIVATE stocks_lib)

# What a period costs on tens of thousands of rows (run it in a Release build; see the file).
add_executable(stocks_frame_bench tools/FrameBench.cpp)
target_link_libraries(stocks_frame_bench PRIVATE stocks_lib)
add_executable(stocks_period_bench tools/PeriodBench.cpp)
target_link_libraries(stocks_period_bench PRIVATE stocks_lib)
# What start-up costs on a million facts and thirty thousand edges (run it in a Release build; see the file).
add_executable(stocks_restore_bench tools/RestoreBench.cpp)
target_link_libraries(stocks_restore_bench PRIVATE stocks_model)

enable_testing()
# The program starts, reads its command line through cellui and says what is wrong with it.
add_test(NAME stocks_rejects_an_unknown_ui COMMAND stocks --ui nonesuch)
set_tests_properties(stocks_rejects_an_unknown_ui PROPERTIES PASS_REGULAR_EXPRESSION "unknown --ui")

# The benchmarks are built with everything else (above), and run here at a tiny size, so that a change to the store's interface
# breaks a test and not a benchmark nobody runs. Their numbers mean nothing at this size; a crash or a missing line fails.
add_test(NAME stocks_period_bench_smoke COMMAND stocks_period_bench 30)
set_tests_properties(stocks_period_bench_smoke PROPERTIES PASS_REGULAR_EXPRESSION "30 rows, 60 daily bars each.*build.*reference.*year +period" TIMEOUT 60)
add_test(NAME stocks_period_bench_full_smoke COMMAND stocks_period_bench 30 full)
set_tests_properties(stocks_period_bench_full_smoke PROPERTIES PASS_REGULAR_EXPRESSION "30 rows, 60 daily \\+ 100 5m.*year +period" TIMEOUT 60)
add_test(NAME stocks_frame_bench_smoke COMMAND stocks_frame_bench 30)
set_tests_properties(stocks_frame_bench_smoke PROPERTIES PASS_REGULAR_EXPRESSION "PageDown.*present again.*store change" TIMEOUT 60)

# The one-off script that moves a portfolio file's transactions from one ISIN to another (tools/rekey_isin.py).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME stocks_rekey_isin COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/rekey_isin.py --selftest)
  set_tests_properties(stocks_rekey_isin PROPERTIES PASS_REGULAR_EXPRESSION "rekey_isin: ok")
endif()

# The browser on a test display: keys in, what it shows out. cellui's test display
# (TestBackend) is built where there is a POSIX event loop, so not on Windows.
if(NOT WIN32)
  add_executable(stocks_tests tests/test_main.cpp tests/BrowserTests.cpp tests/ModelTests.cpp tests/SourceTests.cpp tests/InstrumentListsTests.cpp tests/NordnetTests.cpp tests/HistoryTests.cpp tests/NordnetHistoryTests.cpp tests/DatesTests.cpp tests/IconTests.cpp tests/FundTests.cpp tests/DataDirectoryTests.cpp tests/FactTests.cpp tests/NodeKeyTests.cpp tests/NordnetDetailsTests.cpp tests/NordnetNewsTests.cpp tests/SuomenPankkiTests.cpp tests/EcbTests.cpp tests/PortfolioTests.cpp tests/PortfolioFileTests.cpp tests/PhysicalTests.cpp tests/KnownIsinsTests.cpp tests/GoldApiTests.cpp tests/GoldPriceTests.cpp tests/SifterTests.cpp tests/SummaryTests.cpp tests/SummaryStoreTests.cpp)
  target_link_libraries(stocks_tests PRIVATE stocks_lib SQLiteCpp)  # the tests of the file make an old one with it
  add_test(NAME stocks_tests COMMAND stocks_tests)
  set_tests_properties(stocks_tests PROPERTIES TIMEOUT 60)
endif()

# The market-data server and its ingest (src/market, plans/server.md): `market-ingest` loads datasets into a database and `market-server`
# serves them. The catalog and the bars are kept in SQLite, and in MariaDB too where libmariadb is found (pkg-config). Not built on Windows.
if(NOT WIN32)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(MARIADB QUIET IMPORTED_TARGET GLOBAL libmariadb)
  endif()
  add_subdirectory(src/market)
endif()
