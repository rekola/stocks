#pragma once

#include "market/Bars.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// What the market-data server keeps, whatever it is kept in. The catalog and the bars are one interface: the MariaDB store
// (MariaDbStore.h) is the real one, and the memory store is the same contract for tests that need no server.
//
// An instrument is an edge, as in plans/server.md: four keys (the model's NodeKeys), the base and the quote (the two nodes
// a pair joins: currencies and units are their codes), the market (XXXX: none) and the dataset that says it. Bars are
// keyed by the edge, the level (the model's Resolution code) and the bar's key (market/Bars.h).
namespace market {

inline constexpr int kSchemaVersion = 1;

struct Edge {
  uint64_t base = 0, quote = 0, market = 0, dataset = 0;
  friend bool operator==(const Edge &, const Edge &) = default;
  friend auto operator<=>(const Edge &, const Edge &) = default;
};

struct InstrumentRecord {
  Edge edge;
  std::string code;        // "eurusd", unique within its dataset
  std::string name;        // "EUR/USD"
  std::string assetClass;  // "currency", "commodity", "index", "other"
  int firstPeriod = 0;     // YYYYMM the dataset's data begins, 0 unknown
};

// What a load left of one level of one instrument.
struct LevelState {
  Resolution level = Resolution::Day;
  uint64_t version = 0;       // the batch that wrote it
  Timestamp loadedAt = 0;     // UTC seconds
  int64_t barCount = 0;
  int64_t firstKey = 0, lastKey = 0;
  Timestamp lastBarTime = 0;  // the start of the last source bar the level was built from: where its window ends
};

struct InstrumentInfo {
  InstrumentRecord instrument;
  std::vector<LevelState> levels;  // the levels that have been loaded, finest first
};

class MarketStore {
 public:
  virtual ~MarketStore() = default;

  // ---- writing (the ingest) ----
  // Makes the dataset known (its key is made from its code) and returns the key.
  virtual uint64_t putDataset(const std::string & code, const std::string & name) = 0;
  // One ingest run: a number that goes up with each, and tags what the run wrote.
  virtual uint64_t beginBatch(uint64_t dataset, const std::string & note) = 0;
  // Makes the instrument if it is not there; says what is known of it where it is.
  virtual void putInstrument(const InstrumentRecord &) = 0;
  // Replaces every bar of the level of the edge with `bars` (ascending) and records the load, all or nothing: nothing outside the
  // new bars remains.
  virtual void replaceBars(const Edge &, Resolution level, const std::vector<Bar> & bars, Timestamp lastBarTime, uint64_t version) = 0;

  // ---- reading (the API) ----
  virtual std::vector<InstrumentInfo> instruments() = 0;
  // The bars of the level with keys from `firstKey` to `lastKey`, both included, ascending.
  virtual std::vector<Bar> bars(const Edge &, Resolution level, int64_t firstKey, int64_t lastKey) = 0;
  // Whether the store can answer.
  virtual bool healthy() = 0;
};

// The store in memory: a map. The contract tests run it and the MariaDB store against the same cases.
std::unique_ptr<MarketStore> makeMemoryStore();

}  // namespace market
