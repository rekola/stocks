#pragma once

#include "market/Bars.h"
#include "model/Summary.h"
#include "model/Types.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

// What a dataset is to the ingest: a place that has instruments and, for each, bars of the resolutions (market/Bars.h). The ingest
// (Ingest.h) writes them to the application's own Store, the way a source does, and names no provider; an adapter (market/offline/) knows its
// endpoints, its files and how to read them, and hands over finished bars. More datasets (recent prices) are more adapters; the only list of
// them is the ingest program's main().
namespace market {

using Log = std::function<void(const std::string &)>;

// An instrument as a dataset names it, and so how the store makes it: an edge (see Types.h) of a base node, a quote node and a market.
struct InstrumentSpec {
  std::string code;    // how the dataset and the command line name it, lower case: "eurusd"
  std::string symbol;  // what the dataset calls it on its market (the listing's): "EURUSD"
  std::string name;    // what a row shows (the listing's): "EUR/USD"
  std::string type;    // the provider's own type for it ("CURRENCY", "METAL", "OIL", "INDEX")
  // The base node: a currency is found by its symbol; anything else is an instrument by its ISIN (source/KnownIsins.h), made if new.
  std::string baseSymbol, baseName, baseIsin;
  InstrumentKind baseKind = InstrumentKind::Currency;
  std::string quote;                 // a currency's symbol, or kPointsUnit for an index
  std::string market = kNoMarket;    // a MIC
  int firstPeriod = 0;               // the month the data begins, YYYYMM; 0 where not known
};

struct LoadOptions {
  std::vector<Resolution> resolutions;  // the resolutions to build
  bool refresh = false;                 // ask again for what may have grown since (a file of the current year)
};

struct Series {
  Resolution resolution = Resolution::Day;
  std::vector<Candle> bars;  // the window of the resolution, ascending
};

struct LoadResult {
  std::vector<Series> series;        // the resolutions that were built whole; one that a file was missing for is not here
  // What the dataset says the instrument stands at now, for the summary (`Quote`: a last price, and a bid and an ask where the dataset has both
  // sides). Not a history: a dataset that has one writes its own series. None for a dataset that gives bars only.
  std::optional<Quote> quote;
  Timestamp lastBarTime = 0;         // the start of the instrument's last available bar of the finest data (UTC seconds); 0 for none
  std::vector<std::string> failed;   // what could not be had, each as "what: why"
  std::vector<std::string> skipped;  // the resolutions left as they are because of that, as "resolution: why"
  bool inactive = false;             // no data in the current year (or, early in a year, the last): the instrument is not carried
  int activeFromYear = 0;            // what that means: the year its data has to reach into
  int downloads = 0, cached = 0;     // files asked for, and files that were there already
};

class Dataset {
 public:
  virtual ~Dataset() = default;
  // The dataset's own code ("HISTDATA", capitals as the store has them) and the name a person reads.
  virtual std::string code() const = 0;
  virtual std::string name() const = 0;
  // The instruments the dataset has (all that it lists: whether they are active is `load`'s to say). An adapter that cannot find out falls
  // back to a list of its own and says so in the log. What it has read before may be kept (in the raw cache), so that a run with everything
  // in hand asks for nothing; `refresh` reads it again.
  virtual std::vector<InstrumentSpec> discover(const Log & log, bool refresh) = 0;
  // Gets what the resolutions need and builds them. Never throws for a file that cannot be had: that is `failed`.
  virtual LoadResult load(const InstrumentSpec & instrument, const LoadOptions & options, const Log & log) = 0;
};

}  // namespace market
