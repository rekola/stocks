#pragma once

#include "market/Bars.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// What the market-data server keeps, whatever it is kept in. The catalog and the bars are one interface with one implementation over
// SQL (SqlStore.h), run on SQLite or on MariaDB, and one in memory for tests that need neither.
//
// An instrument is an edge, as in plans/server.md: four keys (the model's NodeKeys), the base and the quote (the two nodes a pair
// joins: currencies and units are their codes), the market (XXXX: none) and the dataset that says it. Bars are keyed by the edge,
// the level and the bar's key (market/Bars.h).
namespace market {

inline constexpr int kSchemaVersion = 2;

struct Edge {
  uint64_t base = 0, quote = 0, market = 0, dataset = 0;
  friend bool operator==(const Edge &, const Edge &) = default;
};

struct EdgeHash {
  size_t operator()(const Edge & e) const noexcept {
    size_t h = std::hash<uint64_t>()(e.base);
    for (uint64_t part : {e.quote, e.market, e.dataset}) h = h * 0x9E3779B97F4A7C15ull + std::hash<uint64_t>()(part) + (h >> 7);
    return h;
  }
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
  Level level = Level::Day;
  uint64_t version = 0;       // the batch that wrote it
  Timestamp loadedAt = 0;     // UTC seconds
  int64_t barCount = 0;
  int64_t firstKey = 0, lastKey = 0;
  Timestamp lastBarTime = 0;  // the start of the last source bar the level was built from: where its window ends
};

struct InstrumentInfo {
  InstrumentRecord instrument;
  std::vector<LevelState> levels;  // the levels that have been loaded, shortest first
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
  // Forgets an instrument and everything loaded for it. Nothing if it is not there.
  virtual void removeInstrument(const Edge &) = 0;
  // Replaces every bar of the level of the edge with `bars` (ascending) and records the load, all or nothing: nothing outside the
  // new bars remains.
  virtual void replaceBars(const Edge &, Level, const std::vector<Bar> & bars, Timestamp lastBarTime, uint64_t version) = 0;

  // ---- reading (the API) ----
  virtual std::vector<InstrumentInfo> instruments() = 0;
  // Calls `each` with the bars of the level with keys from `firstKey` to `lastKey`, both included, ascending, as they come from the
  // database: the bars are not collected first.
  virtual void forEachBar(const Edge &, Level, int64_t firstKey, int64_t lastKey, const std::function<void(const Bar &)> & each) = 0;
  // Whether the store can answer.
  virtual bool healthy() = 0;

  // The same bars in a vector, for where that is what is wanted.
  std::vector<Bar> bars(const Edge & edge, Level level, int64_t firstKey, int64_t lastKey) {
    std::vector<Bar> out;
    forEachBar(edge, level, firstKey, lastKey, [&](const Bar & b) { out.push_back(b); });
    return out;
  }
};

// The store in memory. The contract tests run it and the SQL stores against the same cases.
std::unique_ptr<MarketStore> makeMemoryStore();

}  // namespace market
