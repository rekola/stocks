#pragma once

#include <cstdint>
#include <limits>
#include <tuple>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The data model's plain values. Nothing here knows the screen or where data comes from.

// Ids are assigned by the Store; a default-constructed id (0) is "none".
template <class Tag>
struct Id {
  uint32_t value = 0;
  bool valid() const { return value != 0; }
  friend bool operator==(Id a, Id b) { return a.value == b.value; }
  friend bool operator<(Id a, Id b) { return a.value < b.value; }
};
using InstrumentId = Id<struct InstrumentTag>;
using MarketId = Id<struct MarketTag>;
using DatasetId = Id<struct DatasetTag>;

// UTC, milliseconds since the Unix epoch. Sessions and time zones belong to the market, not here.
using Timestamp = int64_t;

// A half-open range [from, to).
struct TimeRange {
  Timestamp from = std::numeric_limits<Timestamp>::min(), to = std::numeric_limits<Timestamp>::max();
  bool contains(Timestamp t) const { return t >= from && t < to; }
};

// Anything that can be priced or that prices something: a share, a currency, a commodity, an index.
enum class InstrumentKind { Company, Currency, Commodity, Index, Other };

struct Instrument {
  InstrumentId id;
  std::string symbol, name;
  InstrumentKind kind = InstrumentKind::Other;
};

// A place where an instrument is listed and priced. Currency, calendar and time zone join later.
struct Market {
  MarketId id;
  std::string name;
};

// Where a fact came from: a provider, a file, a prediction model. Two datasets that disagree both stay.
struct Dataset {
  DatasetId id;
  std::string name;
};

// "One unit of `base` costs this much `quote`", as one dataset reports it on one market. The key of a price series
// and of an order book series.
struct EdgeKey {
  InstrumentId base, quote;
  MarketId market;
  DatasetId dataset;
  friend bool operator==(const EdgeKey &, const EdgeKey &) = default;
  friend bool operator<(const EdgeKey & a, const EdgeKey & b) {
    return std::tie(a.market, a.base, a.quote, a.dataset) < std::tie(b.market, b.base, b.quote, b.dataset);
  }
};

// The key of what is said about one instrument itself rather than about a price: announcements and corporate actions.
struct EventKey {
  InstrumentId instrument;
  DatasetId dataset;
  friend bool operator==(const EventKey &, const EventKey &) = default;
  friend bool operator<(const EventKey & a, const EventKey & b) {
    return std::tie(a.instrument, a.dataset) < std::tie(b.instrument, b.dataset);
  }
};

// Prices are doubles: enough for display and models; nothing here settles.
struct Candle {
  Timestamp time = 0;  // start of the bar
  double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

// The bars of one edge, time-ordered, one bar per time, all `intervalMs` long. A different interval is a different
// dataset.
struct CandleSeries {
  int64_t intervalMs = 0;
  std::vector<Candle> candles;
};

struct BookLevel {
  double price = 0, size = 0;
};

// What stood in the book at a moment: bids best (highest) first, asks best (lowest) first.
struct BookSnapshot {
  Timestamp time = 0;
  std::vector<BookLevel> bids, asks;
};

// Something a company said: a press release, a filing, a profit warning.
struct Announcement {
  Timestamp time = 0;  // when it was published
  std::string category, headline, body, url;
};

enum class ActionKind {
  Split,     // `value` is new shares per old (2 for a 2:1 split, 0.1 for a 1:10 reverse split)
  Dividend,  // `value` is the amount per share, in `currency`
  Other,     // spin-off, merger, rights issue, ...: described by `note`
};

// A change to the shares themselves that moves the price without the company being worth more or less.
struct CorporateAction {
  Timestamp time = 0;  // the ex-date: the first moment the price no longer carries it
  ActionKind kind = ActionKind::Other;
  double value = 0;
  InstrumentId currency;  // dividends only
  std::string note;
};
