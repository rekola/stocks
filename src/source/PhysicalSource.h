#pragma once

#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// The physical metals, estimated: the spot prices the store already has (whichever source gave them, found by their listing symbols,
// which data/physical/items.csv names, one price for each currency: goldprice.dev's gold in euros and in dollars, else Nordnet's in dollars)
// turned into the price of what is held in hand: a gold bar by the gram, a gold coin by the troy ounce, a platinum bar by the gram, and whatever the file adds (silver, scrap).
// Nothing publishes such a price, so each item's is the spot price per ounce, times the fine ounces in one unit, times a bid factor from
// the file (what a dealer pays, which is what a holding is valued at: the premium is lost when it is sold). The items are instruments
// of their own on XOFF (traded off exchange, so they can be bought and sold like a fund) in the dataset "PHYSICAL", one edge in each currency the spot has, one
// daily bar each, on the day of the spot's last bar.
//
// The factors are estimates and are the one place the guess lives. It writes nothing until a spot price exists, and says so once after a
// few tries (spaced five seconds apart: the lists may be loading), and then it loads again every half minute so that the price follows the spot's.
struct PhysicalItem {
  std::string metal;  // which spot prices it is made from: "gold", "platinum" (the rows `spot,METAL,SYMBOL` of the file)
  std::string symbol, name;
  double ounces = 0;  // fine troy ounces in one unit
  double bid = 1, ask = 1;  // against the melt value; the ask is not used until the store keeps quotes
  std::string symbol_icon;  // a Unicode symbol shown where there is no picture; empty for none
};

struct PhysicalTable {
  struct Spot {
    std::string metal, symbol;  // a listing symbol to look for the metal's price by
  };
  std::vector<Spot> spots;  // best first, for each metal
  std::vector<PhysicalItem> items;
};

// The file's text as a table; false, with `error` set (the line), where a row is wrong or there is no spot or no item.
bool parsePhysicalTable(const std::string & text, PhysicalTable &, std::string & error);
// The table of the file `data/physical/items.csv` beside the program (DataDirectory.h).
bool loadPhysicalTable(PhysicalTable &, std::string & error);

class PhysicalSource : public Source {
 public:
  explicit PhysicalSource(PhysicalTable table, std::chrono::milliseconds refresh = std::chrono::seconds(30)) : table_(std::move(table)), refresh_(refresh) {}

  std::string name() const override { return "physical"; }
  std::vector<std::string> datasetCodes() const override { return {kPhysicalDataset}; }
  std::string description() const override { return "Physical gold (a bar by the gram, a coin by the ounce), estimated from the gold spot price of another source"; }
  void load(Store &, Http &, SourceListener &) override;
  // Until a spot price is found, it tries again every few seconds (the source that has it is loading); after, at the refresh.
  std::chrono::milliseconds refreshInterval() const override { return misses_ > 0 ? std::min(refresh_, kRetry) : refresh_; }

 private:
  PhysicalTable table_;
  static constexpr std::chrono::milliseconds kRetry{5000};
  std::chrono::milliseconds refresh_;
  int misses_ = 0;  // loads that found no spot price
};
