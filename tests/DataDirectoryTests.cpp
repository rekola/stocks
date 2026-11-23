#include "TestFramework.h"

#include "source/DataDirectory.h"
#include "source/FundSource.h"
#include "source/InstrumentLists.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// A layout as an installation makes it: prefix/bin/stocks, prefix/share/stocks/funds/x.json.
struct Tree {
  fs::path root = fs::temp_directory_path() / ("stocks-data-test-" + std::to_string(std::hash<std::string>()(__FILE__)));
  Tree() {
    fs::remove_all(root);
    fs::create_directories(root / "prefix" / "bin");
    fs::create_directories(root / "prefix" / "share" / "stocks" / "funds");
    fs::create_directories(root / "build" / "data" / "lists");
    std::ofstream(root / "prefix" / "share" / "stocks" / "funds" / "x.json") << "{}";
  }
  ~Tree() { fs::remove_all(root); }
};

}  // namespace

TEST(the_data_is_found_beside_the_build_beside_the_installation_and_in_the_source_tree) {
  Tree t;
  // Not the current directory's, which is first and is wherever the test runs.
  auto without = [](std::vector<fs::path> roots) {
    CHECK(roots.front() == fs::path("data"));
    roots.erase(roots.begin());
    return roots;
  };
  // Installed: the data is in share/stocks, a level up from the program.
  auto roots = without(dataRoots(t.root / "prefix" / "bin"));
  CHECK(findIn(roots, "funds") == t.root / "prefix" / "share" / "stocks" / "funds");
  // In a build directory: the copy beside the program.
  roots = without(dataRoots(t.root / "build"));
  CHECK(findIn(roots, "lists") == t.root / "build" / "data" / "lists");
  // The data of the source tree is the last place, so a program built here finds it from anywhere.
  CHECK(roots.back() == fs::path(STOCKS_DATA_DIR));
  CHECK(!findIn({fs::path(STOCKS_DATA_DIR)}, "funds").empty() && !findIn({fs::path(STOCKS_DATA_DIR)}, "lists").empty());
  // The nearer one wins, and nothing found is empty, not a guess.
  CHECK(findIn({t.root / "build" / "data", fs::path(STOCKS_DATA_DIR)}, "lists") == t.root / "build" / "data" / "lists");
  CHECK(findIn({t.root / "nowhere"}, "funds").empty());
}

TEST(the_data_the_program_ships_with_is_found_from_where_the_tests_run) {
  CHECK(!findDataDirectory("lists").empty() && !findDataDirectory("funds").empty());
  CHECK(!executableDirectory().empty());
  FundSchema schema;
  std::string error;
  CHECK(FundSchema::load("op", schema, error));
}
