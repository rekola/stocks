#pragma once

#include "market/Bars.h"

#include <functional>
#include <string>
#include <vector>

// What a dataset is to the ingest: a place that has instruments and, for each, the bars of the levels (market/Bars.h). The ingest
// (Ingest.h) and the store name no provider; an adapter (src/source/) knows its endpoints, its files and how to read them, and hands
// over finished bars. More datasets (recent prices) are more adapters; the only list of them is the ingest program's main().
namespace market {

using Log = std::function<void(const std::string &)>;

// An instrument as a dataset names it. A pair or a quoted instrument is an edge between two nodes, which the keys of the catalog
// name by their codes (a currency's or a unit's, `currencyKey`), and a market (XXXX where there is none).
struct InstrumentSpec {
  std::string code;        // how the dataset and the command line name it, lower case: "eurusd"
  std::string name;        // "EUR/USD"
  std::string assetClass;  // "currency", "commodity", "index" or "other"
  std::string base, quote; // the codes of the two nodes: "EUR", "USD"
  std::string market = "XXXX";
  int firstPeriod = 0;     // the month the data begins, YYYYMM; 0 where not known
};

struct LoadOptions {
  std::vector<Level> levels;  // the levels to build
  bool refresh = false;            // ask again for what may have grown since (a file of the current year)
};

struct LevelBars {
  Level level = Level::Year;
  std::vector<Bar> bars;  // the window of the level, ascending
};

struct LoadResult {
  std::vector<LevelBars> levels;     // the levels that were built whole; a level that a file was missing for is not here
  Timestamp lastBarTime = 0;         // the start of the instrument's last available bar of the finest data (UTC seconds); 0 for none
  std::vector<std::string> failed;   // what could not be had, each as "what: why"
  std::vector<std::string> skipped;  // the levels left as they are because of that, as "level: why"
  bool inactive = false;             // no data in the current year (or, early in a year, the last): the instrument is not carried
  int activeFromYear = 0;            // what that means: the year its data has to reach into
  int downloads = 0, cached = 0;     // files asked for, and files that were there already
};

class Dataset {
 public:
  virtual ~Dataset() = default;
  // The dataset's own code ("histdata"): what its bars are tagged with, and the name of its key in the catalog.
  virtual std::string code() const = 0;
  virtual std::string name() const = 0;
  // The instruments the dataset has (all that it lists: whether they are active is `load`'s to say). An adapter that cannot find out falls back to a list of its own and says so in the log. What it
  // has read before may be kept (in the raw cache), so that a run with everything in hand asks for nothing; `refresh` reads it again.
  virtual std::vector<InstrumentSpec> discover(const Log & log, bool refresh) = 0;
  // Gets what the levels need and builds them. Never throws for a file that cannot be had: that is `failed`.
  virtual LoadResult load(const InstrumentSpec & instrument, const LoadOptions & options, const Log & log) = 0;
};

}  // namespace market
