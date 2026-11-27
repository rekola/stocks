#pragma once

#include "model/Types.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What is kept of each edge between runs, and the interface of the place it is kept in. Nothing here knows SQLite: a store backed by a
// server keeps the same records somewhere else.
//
// THE SUMMARY IS NOT THE DATA. The data is the series of bars a source writes (`Candle`, `appendCandles`, `Snapshot::candles()`): it
// stays in memory only, as written. The summary is a separate record that the store derives from it (and from what a source says of
// a price) for the tables, the sparklines and the portfolios: the edge's QUOTE and, for each resolution, a QUEUE of the bars that the
// periods using that resolution show. Only the summary is kept.

// An edge's price as one provider says it: the last price, the bid and the ask, and the one time they are for (seconds, UTC). A provider
// that gives only a last price, or none, leaves the rest undefined. A time of 0 is unknown.
struct Quote {
  double last = kUndefined, bid = kUndefined, ask = kUndefined;
  Timestamp time = 0;
  bool says() const { return isDefined(last) || isDefined(bid) || isDefined(ask); }
  friend bool operator==(const Quote & a, const Quote & b) {
    const auto same = [](double x, double y) { return x == y || (!isDefined(x) && !isDefined(y)); };
    return same(a.last, b.last) && same(a.bid, b.bid) && same(a.ask, b.ask) && a.time == b.time;
  }
};

// An edge's change over each period, as a number of its own: what the table sorts by before any history is loaded, and what stays
// in step with the bars once there are some. In the order of `Period` (Period.h: day, week, month, quarter, year, 3 years, 10 years),
// in the unit of `PeriodView::change()`: per cent, or basis points for a rate. Undefined: not known. A source that says its own
// numbers (a provider's list of returns) writes them; a period the queues cover is then worked out from the bars instead.
inline constexpr size_t kChangeCount = 7;
struct Changes {
  std::array<double, kChangeCount> pct;
  Changes() { pct.fill(kUndefined); }
  bool says() const {
    for (double c : pct)
      if (isDefined(c)) return true;
    return false;
  }
  friend bool operator==(const Changes & a, const Changes & b) {
    for (size_t i = 0; i < kChangeCount; i++)
      if (!(a.pct[i] == b.pct[i] || (!isDefined(a.pct[i]) && !isDefined(b.pct[i])))) return false;
    return true;
  }
};

// What a queue holds: a bar with an open, a high, a low, a close and a volume, or only a value (a fund's NAV, an index's value of the day).
enum class QueueKind : uint8_t { Candle = 0, Scalar = 1 };
inline QueueKind queueKindOf(InstrumentKind kind) { return kind == InstrumentKind::Fund || kind == InstrumentKind::Index ? QueueKind::Scalar : QueueKind::Candle; }

// What the file says of a queue without its bars: all that start-up reads of it.
struct QueueCoverage {
  QueueKind kind = QueueKind::Candle;
  TimestampMs fetched = 0;   // when the queue was last changed by what a source said (milliseconds UTC)
  uint32_t count = 0;        // the number of bars
  int64_t first_key = 0;     // the keys of its first and last bar (slot numbers, or day numbers for a date resolution)
  int64_t last_key = 0;
  uint64_t last_hash = 0;    // of the last bar's stored record: a bar said again is the same one if its key and hash are, without reading the queue
};

// What a snapshot holds of one queue. A queue is in memory (`resident`: `bars` is the whole of it) or only on the disk, where `disk` says
// what the file has of it and `bars` are the bars said since it was last read (they are put into it when it is read, and win over it).
struct QueueState {
  Seq<Candle> bars;
  QueueKind kind = QueueKind::Candle;
  TimestampMs fetched = 0;
  bool resident = true;
  bool unknown = false;  // while the file is being read: whether it has this queue is not known yet (`disk` says nothing)
  QueueCoverage disk;
};

// One edge as it is kept: what is said of it, and for each resolution the coverage of its queue (`loadEdges()` only).
struct StoredEdge {
  Listing listing;  // its key is the edge's; the rest is empty where no listing was said
  Quote quote;
  Changes changes;
  int64_t previous_close_day = 0;       // the close before the session and the day number it was said for
  double previous_close = kUndefined;   // undefined: none
  std::vector<std::pair<Resolution, QueueCoverage>> queues;
  bool hasListing() const {
    return !listing.symbol.empty() || !listing.identifier.empty() || !listing.orderBook.empty() || !listing.type.empty() || !listing.name.empty();
  }
};

// One queue as it is put to the file, or read from it: the bars in their decoded form, oldest first.
//   `merge` false: the queue is these bars, replacing the one there.
//   `merge` true:  these bars are put into the one there (a bar at a time already there replaces it, the rest are added), and the
//                  result is cut to `keep` seconds measured back from its last bar. What a store says of a queue it does not have in
//                  memory, which it cannot replace without reading it.
struct StoredQueue {
  EdgeKey edge;
  Resolution resolution = Resolution::Day;
  QueueKind kind = QueueKind::Candle;
  TimestampMs fetched = 0;
  bool merge = false;
  int64_t keep = 0;
  std::vector<Candle> bars;
};

// What one write asks of the file: edges put (their listing, quote and close before the session), queues put. A batch is one unit.
struct SummaryBatch {
  std::vector<StoredEdge> edges;
  std::vector<StoredQueue> queues;
  bool empty() const { return edges.empty() && queues.empty(); }
};

// The summaries of one dataset. The calls block on the medium (SummaryWriter keeps the store from waiting on them) and none throws: an
// implementation that cannot read or write says so through its own log and carries on, because the application runs without a place to
// keep things. `loadEdges` and `readQueue` may be called from another thread than `apply`.
class SummaryStore {
 public:
  virtual ~SummaryStore() = default;
  // Every edge of the dataset with its listing, quote, close before the session and the coverage of each of its queues, in one query. No
  // queue is read.
  virtual std::vector<StoredEdge> loadEdges() = 0;
  // The bars of one queue, oldest first; none where there is no such queue (or it cannot be read).
  virtual std::optional<StoredQueue> readQueue(const EdgeKey &, Resolution) = 0;
  // Applies a batch as one transaction.
  virtual void apply(const SummaryBatch &) = 0;
};

// Where the summaries of all the datasets are kept: one store for each dataset's code.
class SummaryStores {
 public:
  virtual ~SummaryStores() = default;
  // The codes of the datasets that already have a store (what an earlier run kept).
  virtual std::vector<std::string> datasets() = 0;
  // The store of a dataset, made if there is none; null where it cannot be had.
  virtual std::shared_ptr<SummaryStore> open(const std::string & dataset) = 0;
};
