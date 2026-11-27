#pragma once

#include "model/Types.h"

#include <functional>
#include <vector>

// Where the bars are kept: all of them, as they are said, not the summary (Summary.h) derived from them. The summary is what the tables and
// the sparklines read and is cut to the window of each period; the bars are the data, for what reads a whole history (the market regime
// embeddings), and only a server that is asked to keeps them. Nothing here knows a database.
//
// The bars said of an edge at a resolution are put in place of those with the same time (a correction, or today's bar updated); the rest
// are inserted. Nothing is ever removed by a put: how much a store keeps is its operator's business, not the store's.

// The bars of one series, time-ordered, one per time.
struct StoredBars {
  EdgeKey edge;
  Resolution resolution = Resolution::Day;
  std::vector<Candle> bars;
};

// What one write to the store asks the medium to do.
struct BarBatch {
  std::vector<StoredBars> series;
  bool empty() const { return series.empty(); }
};

// The calls block on the medium (BarWriter keeps the store from waiting on them); `apply` throws what the medium throws, and the writer
// counts it.
class BarStore {
 public:
  virtual ~BarStore() = default;
  // Puts the bars, all of the batch or none.
  virtual void apply(const BarBatch &) = 0;
  // Calls `each` with the bars of the series with keys from `first` to `last`, both included, ascending, as they come from the medium: they
  // are not collected first.
  virtual void read(const EdgeKey &, Resolution, int64_t first, int64_t last, const std::function<void(const Candle &)> & each) = 0;
};
