#include "TestFramework.h"

#include "model/Isin.h"

#include "source/InstrumentLists.h"

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
    path = fs::temp_directory_path() / ("stocks_list_test_" + std::to_string(counter++) + "_" +
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
            "# market: Second\n# a comment\n\nkind,base,quote,name,yahoo,isin\ncommodity,XAU,USD,Gold,GC=F,XC0009655157\n");
  dir.write("a.csv",
            "# market: First\r\n"
            "company,NOKIA,EUR,\"Nokia, Oyj\",NOKIA.HE,FI0009000681\r\n"
            "currency , EUR , USD , \"The \"\"euro\"\"\" , EURUSD=X,\n"
            "index,OMXH,pts,OMX Helsinki,^OMXH25,FI0008900006\n");
  std::string error;
  InstrumentList u = loadInstrumentLists(dir.path, error);
  CHECK(error.empty() && u.size() == 4);
  CHECK(u[0].market == "First" && u[0].kind == InstrumentKind::Company && u[0].base == "NOKIA" && u[0].quote == "EUR");
  CHECK(u[0].name == "Nokia, Oyj" && u[0].yahoo == "NOKIA.HE" && u[0].isin == "FI0009000681");  // a quoted field keeps its comma
  CHECK(u[1].kind == InstrumentKind::Currency && u[1].base == "EUR" && u[1].name == "The \"euro\"" && u[1].yahoo == "EURUSD=X" && u[1].isin.empty());
  CHECK(u[2].kind == InstrumentKind::Index && u[2].yahoo == "^OMXH25");
  CHECK(u[3].market == "Second" && u[3].kind == InstrumentKind::Commodity);  // files in name order
}

TEST(a_wrong_list_is_refused_with_where_it_is_wrong) {
  auto refused = [](const std::string & text, const std::string & message_part) {
    TempDirectory dir;
    dir.write("x.csv", text);
    std::string error;
    InstrumentList u = loadInstrumentLists(dir.path, error);
    CHECK(u.empty());
    CHECK(error.find(message_part) != std::string::npos);
    if (error.find(message_part) == std::string::npos) std::fprintf(stderr, "  error was: %s\n", error.c_str());
  };
  refused("company,NOKIA,EUR,Nokia,NOKIA.HE,FI0009000681\n", "before the '# market:'");
  refused("kind,base,quote,name,yahoo,isin\n", "no '# market:' line");
  refused("# market: M\ncompany,NOKIA,EUR,Nokia\n", "x.csv:2: expected 6 fields");
  refused("# market: M\nshare,NOKIA,EUR,Nokia,NOKIA.HE,FI0009000681\n", "unknown kind 'share'");
  refused("# market: M\ncompany,,EUR,Nokia,NOKIA.HE,FI0009000681\n", "cannot be empty");
  refused("# market: M\ncompany,NOKIA,EUR,Nokia,NOKIA.HE,\n", "has no ISIN");  // only a currency is without
  refused("# market: M\n", "");  // a market without rows is no instruments at all

  std::string error;
  CHECK(loadInstrumentLists(fs::temp_directory_path() / "stocks_no_such_directory", error).empty() && !error.empty());
}

TEST(onlyMarkets_keeps_the_entries_of_the_named_markets_in_order) {
  InstrumentList u = {{"A", "1", "X", "one", "1", InstrumentKind::Other}, {"B", "2", "X", "two", "2", InstrumentKind::Other},
                {"A", "3", "X", "three", "3", InstrumentKind::Other}, {"C", "4", "X", "four", "4", InstrumentKind::Other}};
  CHECK(onlyMarkets(u, {}).size() == 4);
  InstrumentList a = onlyMarkets(u, {"A"});
  CHECK(a.size() == 2 && a[0].base == "1" && a[1].base == "3");
  CHECK(onlyMarkets(u, {"C", "B"}).size() == 2);
  CHECK(onlyMarkets(u, {"nope"}).empty());
}

TEST(the_list_directory_is_the_one_given_else_the_default_ones) {
  CHECK(findListsDirectory("elsewhere") == fs::path("elsewhere"));
  fs::path found = findListsDirectory("");
  CHECK(!found.empty() && fs::is_directory(found));
}

// The lists that ship: the checks that keep a hand-edited or regenerated file usable.
TEST(a_spec_names_the_lists_to_load_and_the_markets_to_keep) {
  TempDirectory dir;
  dir.write("a.csv", "# market: First\ncompany,A,EUR,A,A.HE,I1\ncompany,B,EUR,B,B.HE,I2\n");
  dir.write("b.csv", "# market: Second\ncommodity,XAU,USD,Gold,GC=F,XC0009655157\n");
  std::string error;
  InstrumentList all = resolveList({dir.path.string(), {}}, error);
  CHECK(error.empty() && all.size() == 3);
  InstrumentList second = resolveList({dir.path.string(), {"Second"}}, error);
  CHECK(second.size() == 1 && second[0].base == "XAU");
  CHECK(resolveList({dir.path.string(), {"Nowhere"}}, error).empty() && error == "no instruments in the markets asked for");
  CHECK(resolveList({(fs::temp_directory_path() / "stocks_no_such_directory").string(), {}}, error).empty() && !error.empty());
  CHECK(!resolveList({"", {}}, error).empty());  // the default lists, the ones that ship
}

TEST(the_lists_on_disk_are_consistent) {
  std::string error;
  InstrumentList u = loadInstrumentLists(findListsDirectory(""), error);
  CHECK(error.empty());

  std::map<std::string, size_t> perMarket;
  std::set<std::string> tickers, pairs, isins;
  std::map<std::string, std::pair<InstrumentKind, std::string>> bySymbol;  // a symbol is one instrument
  for (const ListEntry & e : u) {
    perMarket[e.market]++;
    CHECK(!e.name.empty() && !e.base.empty() && !e.quote.empty() && !e.yahoo.empty());
    CHECK(tickers.insert(e.yahoo).second);                                // no ticker twice
    CHECK(pairs.insert(e.market + "|" + e.base + "|" + e.quote).second);  // no edge twice
    auto [it, inserted] = bySymbol.insert({e.base, {e.kind, e.name}});
    CHECK(inserted || (it->second.first == e.kind && it->second.second == e.name));  // the same symbol, the same thing
    if (e.kind == InstrumentKind::Currency) CHECK(e.yahoo == e.base + e.quote + "=X");
    if (e.market == "Nasdaq Helsinki") {
      std::string stem = e.base;  // Nordnet's symbol, which has a space where Yahoo's has a hyphen
      for (char & c : stem)
        if (c == ' ') c = '-';
      CHECK(e.kind == InstrumentKind::Company && e.yahoo == stem + ".HE");
    }
    // Only a currency is without an ISIN, and an ISIN is one instrument with a right check digit.
    CHECK(e.kind == InstrumentKind::Currency ? e.isin.empty() : isValidIsin(e.isin));
    if (!e.isin.empty()) CHECK(isins.insert(e.market + "|" + e.isin).second);
    if (e.market == "Indices") CHECK(e.kind == InstrumentKind::Index && e.quote == "pts");
  }
  CHECK(perMarket.size() == 3);  // no commodities: Yahoo's are futures contracts, not the commodity's price
  CHECK(perMarket["Nasdaq Helsinki"] >= 90 && perMarket["Indices"] >= 5);
  CHECK(perMarket["Currencies"] >= 80);
  // Every currency that is a quote has a name, shown beside the pair.
  for (const ListEntry & e : u) CHECK(std::string(currencyName(e.quote)) != "" || e.kind != InstrumentKind::Currency);
}
