#pragma once

#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

// Physical gold, estimated: the gold spot price the store already has (whichever source gave it, found by its listing symbol, which
// data/gold/items.csv names) turned into the price of what is held in hand: a gold bar by the gram, a gold coin by the troy ounce.
// Nothing publishes such a price, so each item's is the spot price per ounce, times the fine ounces in one unit, times a bid factor from
// the file (what a dealer pays, which is what a holding is valued at: the premium is lost when it is sold). The items are instruments
// of their own on XOFF (traded off exchange, so they can be bought and sold like a fund) in the dataset "GOLDEST", quoted in the spot's
// currency, one daily bar each, on the day of the spot's last bar.
//
// The factors are estimates and are the one place the guess lives. It writes nothing until a spot price exists, and says so once after a
// few tries (spaced five seconds apart: the lists may be loading), and then it loads again every half minute so that the price follows the spot's.
struct GoldItem {
  std::string symbol, name;
  double ounces = 0;  // fine troy ounces in one unit
  double bid = 1, ask = 1;  // against the melt value; the ask is not used until the store keeps quotes
};

struct GoldTable {
  std::vector<std::string> spots;  // listing symbols to look for, best first
  std::vector<GoldItem> items;
};

// The file's text as a table; false, with `error` set (the line), where a row is wrong or there is no spot or no item.
bool parseGoldTable(const std::string & text, GoldTable &, std::string & error);
// The table of the file `data/gold/items.csv` beside the program (DataDirectory.h).
bool loadGoldTable(GoldTable &, std::string & error);

class GoldSource : public Source {
 public:
  explicit GoldSource(GoldTable table, std::chrono::milliseconds refresh = std::chrono::seconds(30)) : table_(std::move(table)), refresh_(refresh) {}

  std::string name() const override { return "gold"; }
  std::vector<std::string> datasetCodes() const override { return {kGoldDataset}; }
  std::string description() const override { return "Physical gold (a bar by the gram, a coin by the ounce), estimated from the gold spot price of another source"; }
  void load(Store &, Http &, SourceListener &) override;
  // Until a spot price is found, it tries again every few seconds (the source that has it is loading); after, at the refresh.
  std::chrono::milliseconds refreshInterval() const override { return misses_ > 0 ? std::min(refresh_, kRetry) : refresh_; }

 private:
  GoldTable table_;
  static constexpr std::chrono::milliseconds kRetry{5000};
  std::chrono::milliseconds refresh_;
  int misses_ = 0;  // loads that found no spot price
};
