#include "TestFramework.h"

#include "source/Universe.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace {

namespace fs = std::filesystem;

// A directory of files that goes away with it.
struct TempDirectory {
  fs::path path;
  TempDirectory() {
    static std::atomic<int> counter{0};
    path = fs::temp_directory_path() / ("stocks_universe_test_" + std::to_string(counter++) + "_" +
                                         std::to_string(reinterpret_cast<uintptr_t>(this)));
    fs::create_directories(path);
  }
  ~TempDirectory() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  void write(const std::string & name, const std::string & text) const { std::ofstream(path / name, std::ios::binary) << text; }
};

}  // namespace

TEST(a_list_file_is_one_market_of_comma_separated_rows) {
  TempDirectory dir;
  dir.write("b.csv",
            "# market: Second\n# a comment\n\nkind,base,quote,name,yahoo\ncommodity,XAU,USD,Gold,GC=F\n");
  dir.write("a.csv",
            "# market: First\r\n"
            "company,NOKIA,EUR,\"Nokia, Oyj\",NOKIA.HE\r\n"
            "currency , EUR , USD , \"The \"\"euro\"\"\" , EURUSD=X\n"
            "index,OMXH,EUR,OMX Helsinki,^OMXH25\n");
  std::string error;
  Universe u = loadUniverse(dir.path, error);
  CHECK(error.empty() && u.size() == 4);
  CHECK(u[0].market == "First" && u[0].kind == InstrumentKind::Company && u[0].base == "NOKIA" && u[0].quote == "EUR");
  CHECK(u[0].name == "Nokia, Oyj" && u[0].yahoo == "NOKIA.HE");  // a quoted field keeps its comma
  CHECK(u[1].kind == InstrumentKind::Currency && u[1].base == "EUR" && u[1].name == "The \"euro\"" && u[1].yahoo == "EURUSD=X");
  CHECK(u[2].kind == InstrumentKind::Index && u[2].yahoo == "^OMXH25");
  CHECK(u[3].market == "Second" && u[3].kind == InstrumentKind::Commodity);  // files in name order
}

TEST(a_wrong_list_is_refused_with_where_it_is_wrong) {
  auto refused = [](const std::string & text, const std::string & message_part) {
    TempDirectory dir;
    dir.write("x.csv", text);
    std::string error;
    Universe u = loadUniverse(dir.path, error);
    CHECK(u.empty());
    CHECK(error.find(message_part) != std::string::npos);
    if (error.find(message_part) == std::string::npos) std::fprintf(stderr, "  error was: %s\n", error.c_str());
  };
  refused("company,NOKIA,EUR,Nokia,NOKIA.HE\n", "before the '# market:'");
  refused("kind,base,quote,name,yahoo\n", "no '# market:' line");
  refused("# market: M\ncompany,NOKIA,EUR,Nokia\n", "x.csv:2: expected 5 fields");
  refused("# market: M\nshare,NOKIA,EUR,Nokia,NOKIA.HE\n", "unknown kind 'share'");
  refused("# market: M\ncompany,,EUR,Nokia,NOKIA.HE\n", "cannot be empty");
  refused("# market: M\n", "");  // a market without rows is no instruments at all

  std::string error;
  CHECK(loadUniverse(fs::temp_directory_path() / "stocks_no_such_directory", error).empty() && !error.empty());
}

TEST(onlyMarkets_keeps_the_entries_of_the_named_markets_in_order) {
  Universe u = {{"A", "1", "X", "one", "1", InstrumentKind::Other}, {"B", "2", "X", "two", "2", InstrumentKind::Other},
                {"A", "3", "X", "three", "3", InstrumentKind::Other}, {"C", "4", "X", "four", "4", InstrumentKind::Other}};
  CHECK(onlyMarkets(u, {}).size() == 4);
  Universe a = onlyMarkets(u, {"A"});
  CHECK(a.size() == 2 && a[0].base == "1" && a[1].base == "3");
  CHECK(onlyMarkets(u, {"C", "B"}).size() == 2);
  CHECK(onlyMarkets(u, {"nope"}).empty());
}

TEST(the_universe_directory_is_the_one_given_else_the_default_ones) {
  CHECK(findUniverseDirectory("elsewhere") == fs::path("elsewhere"));
  fs::path found = findUniverseDirectory("");
  CHECK(!found.empty() && fs::is_directory(found));
}

// The lists that ship: the checks that keep a hand-edited or regenerated file usable.
TEST(the_lists_on_disk_are_consistent) {
  std::string error;
  Universe u = loadUniverse(findUniverseDirectory(""), error);
  CHECK(error.empty());

  std::map<std::string, size_t> perMarket;
  std::set<std::string> tickers, pairs;
  std::map<std::string, std::pair<InstrumentKind, std::string>> bySymbol;  // a symbol is one instrument
  for (const UniverseEntry & e : u) {
    perMarket[e.market]++;
    CHECK(!e.name.empty() && !e.base.empty() && !e.quote.empty() && !e.yahoo.empty());
    CHECK(tickers.insert(e.yahoo).second);                                // no ticker twice
    CHECK(pairs.insert(e.market + "|" + e.base + "|" + e.quote).second);  // no edge twice
    auto [it, inserted] = bySymbol.insert({e.base, {e.kind, e.name}});
    CHECK(inserted || (it->second.first == e.kind && it->second.second == e.name));  // the same symbol, the same thing
    if (e.kind == InstrumentKind::Currency) CHECK(e.yahoo == e.base + e.quote + "=X");
    if (e.market == "Nasdaq Helsinki") {
      CHECK(e.kind == InstrumentKind::Company && e.yahoo == e.base + ".HE");
    }
    if (e.market == "Commodities") CHECK(e.kind == InstrumentKind::Commodity && e.yahoo.size() > 2 && e.yahoo.substr(e.yahoo.size() - 2) == "=F");
  }
  CHECK(perMarket.size() == 3);
  CHECK(perMarket["Nasdaq Helsinki"] >= 90);
  CHECK(perMarket["Commodities"] >= 28);
  CHECK(perMarket["Currencies"] >= 80);
  // Every currency that is a quote has a name, shown beside the pair.
  for (const UniverseEntry & e : u) CHECK(std::string(currencyName(e.quote)) != "" || e.kind != InstrumentKind::Currency);
}
