#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <functional>
#include <immer/flex_vector.hpp>
#include <optional>
#include <string>
#include <vector>

// The data model's plain values. Nothing here knows the screen or where data comes from.

// An immutable, time-ordered sequence. Slices and copies share storage, and a reference into one stays valid for as
// long as that sequence is held.
template <class T>
using Seq = immer::flex_vector<T>;

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
using EdgeId = Id<struct EdgeTag>;  // an edge as a node, made when it first has data

// An instant: whole seconds since the Unix epoch, UTC. Where whole seconds are not enough (order book snapshots) it is
// floating-point seconds, never milliseconds. Sessions and time zones belong to the market, not here. A date is not an
// instant: it is a day number (Dates.h).
using Timestamp = int64_t;

// A half-open range [from, to), in whatever the thing it selects is keyed by: seconds, slot numbers or day numbers.
struct TimeRange {
  Timestamp from = std::numeric_limits<Timestamp>::min(), to = std::numeric_limits<Timestamp>::max();
  bool contains(Timestamp t) const { return t >= from && t < to; }
};

// One thing said about a node: a predicate and its value. Every node (an instrument, a market, a dataset, an edge) keeps
// what is known about it as attributes, all in the same way, one value per predicate; a value can change. What depends
// on a market ("the Helsinki market calls this instrument NOKIA", "its order book there is ...") is an attribute of the
// edge, which has the market, the currency and the dataset that says it: a fat edge. `market` is set for the attributes
// of an edge, to the edge's market, so that one can be found by what it says at a market.
struct Attribute {
  std::string predicate, value;
  MarketId market;
  friend bool operator==(const Attribute &, const Attribute &) = default;
};

namespace predicate {
inline constexpr const char * name = "name";
inline constexpr const char * symbol = "symbol";
inline constexpr const char * isin = "isin";
inline constexpr const char * kind = "kind";
inline constexpr const char * type = "type";  // the provider's own type
inline constexpr const char * nordnetId = "nordnet-id";  // a market's id at Nordnet
inline constexpr const char * nordnetInstrument = "nordnet-instrument";  // an instrument's id at Nordnet
inline constexpr const char * identifier = "identifier";  // the market's own id for the instrument
inline constexpr const char * orderBook = "order-book";   // the id its history is asked for by
inline constexpr const char * mic = "mic";  // a market's ISO 10383 code, the way an ISIN identifies an instrument
}  // namespace predicate

enum class NodeType : uint8_t { Instrument, Market, Dataset, Edge };

// Any node. The typed ids convert to it.
struct NodeRef {
  NodeType type = NodeType::Instrument;
  uint32_t id = 0;
  NodeRef() = default;
  NodeRef(InstrumentId i) : type(NodeType::Instrument), id(i.value) {}
  NodeRef(MarketId i) : type(NodeType::Market), id(i.value) {}
  NodeRef(DatasetId i) : type(NodeType::Dataset), id(i.value) {}
  NodeRef(EdgeId i) : type(NodeType::Edge), id(i.value) {}
  friend bool operator==(const NodeRef &, const NodeRef &) = default;
};

// A price, volume or other number a provider does not give is undefined (NaN), not zero.
inline constexpr double kUndefined = std::numeric_limits<double>::quiet_NaN();
inline bool isDefined(double v) { return v == v; }

// What an instrument is, in one vocabulary for all providers: each adapter maps its own types onto these (Nordnet's UETF
// and PTC, Yahoo's EQUITY and FUTURE, ...). The provider's own type is kept beside it, in `Instrument::type`.
enum class InstrumentKind { Company, Currency, Commodity, Index, Etf, Certificate, Fund, Unit, Other };  // a unit: what an index is quoted in

inline const char * kindName(InstrumentKind k) {
  switch (k) {
    case InstrumentKind::Company: return "company";
    case InstrumentKind::Currency: return "currency";
    case InstrumentKind::Commodity: return "commodity";
    case InstrumentKind::Index: return "index";
    case InstrumentKind::Etf: return "etf";
    case InstrumentKind::Certificate: return "certificate";
    case InstrumentKind::Fund: return "fund";
    case InstrumentKind::Unit: return "unit";
    case InstrumentKind::Other: break;
  }
  return "other";
}
inline InstrumentKind kindOfName(const std::string & s) {
  for (InstrumentKind k : {InstrumentKind::Company, InstrumentKind::Currency, InstrumentKind::Commodity, InstrumentKind::Index,
                           InstrumentKind::Etf, InstrumentKind::Certificate, InstrumentKind::Fund, InstrumentKind::Unit})
    if (s == kindName(k)) return k;
  return InstrumentKind::Other;
}

// The resolved view of an instrument's attributes, for the app.
struct Instrument {
  InstrumentId id;
  std::string symbol, name;  // a symbol said of the instrument itself (a currency's code); the ones markets give are on edges
  InstrumentKind kind = InstrumentKind::Other;
  std::string isin;  // an instrument with an ISIN is that ISIN: one listed on two markets is one instrument
  std::string type;  // the provider's own type ("UETF", "EQUITY"), beside the combined kind
};

// The resolved view of a market's attributes. The id is a surrogate: a market is found by what is said about it. The
// name is empty until one is known.
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

// How big the bars of a series are, as a code. The intraday resolutions are fixed lengths and are keyed by their slot
// number: the UTC Unix seconds of the start of the bar divided by the length (the hour number is the seconds divided by
// 3600). The date resolutions are not fixed lengths (daylight saving, calendars) and are keyed by a date, as a day number
// (Dates.h): a day by its trading date, a week, a month and a quarter by their first day. An edge has a series for each
// resolution it has data in. The order is shortest first.
enum class Resolution : uint8_t { Min1, Min2, Min5, Min15, Min30, Hour1, Hour4, Day, Week, Month, Quarter };

constexpr bool isDateResolution(Resolution r) { return r >= Resolution::Day; }

// The length in seconds of an intraday resolution; 0 for a date resolution, which has none.
constexpr int64_t seconds(Resolution r) {
  switch (r) {
    case Resolution::Min1: return 60;
    case Resolution::Min2: return 120;
    case Resolution::Min5: return 300;
    case Resolution::Min15: return 900;
    case Resolution::Min30: return 1800;
    case Resolution::Hour1: return 3600;
    case Resolution::Hour4: return 14400;
    default: return 0;
  }
}

// A short name: 1m 2m 5m 15m 30m 1h 4h 1d 1w 1M 1Q.
constexpr const char * name(Resolution r) {
  switch (r) {
    case Resolution::Min1: return "1m";
    case Resolution::Min2: return "2m";
    case Resolution::Min5: return "5m";
    case Resolution::Min15: return "15m";
    case Resolution::Min30: return "30m";
    case Resolution::Hour1: return "1h";
    case Resolution::Hour4: return "4h";
    case Resolution::Day: return "1d";
    case Resolution::Week: return "1w";
    case Resolution::Month: return "1M";
    case Resolution::Quarter: return "1Q";
  }
  return "";
}

// One series: an edge at a resolution.
struct SeriesKey {
  EdgeKey edge;
  Resolution resolution = Resolution::Day;
  friend bool operator==(const SeriesKey &, const SeriesKey &) = default;
};

// Prices are doubles: enough for display and models; nothing here settles.
struct Candle {
  // The key of the bar: its slot number for an intraday resolution, the day number of its first day for a date resolution.
  int64_t time = 0;
  double open = 0, high = 0, low = 0, close = 0, volume = 0;
};

// The bars of one edge at one resolution, time-ordered, one bar per time.
struct CandleSeries {
  Resolution resolution = Resolution::Day;
  Seq<Candle> candles;
};

struct BookLevel {
  double price = 0, size = 0;
};

// What stood in the book at a moment: bids best (highest) first, asks best (lowest) first.
struct BookSnapshot {
  double time = 0;  // UTC seconds, with a fraction
  std::vector<BookLevel> bids, asks;
};

// Something a company said: a press release, a filing, a profit warning.
struct Announcement {
  Timestamp time = 0;  // when it was published: UTC seconds
  std::string category, headline, body, url;
};

enum class ActionKind {
  Split,     // `value` is new shares per old (2 for a 2:1 split, 0.1 for a 1:10 reverse split)
  Dividend,  // `value` is the amount per share, in `currency`
  Other,     // spin-off, merger, rights issue, ...: described by `note`
};

// A change to the shares themselves that moves the price without the company being worth more or less.
struct CorporateAction {
  Timestamp time = 0;  // the ex-date, as a day number: the first day the price no longer carries it
  ActionKind kind = ActionKind::Other;
  double value = 0;
  InstrumentId currency;  // dividends only
  std::string note;
};

// Hashes for the keys, for the store's persistent maps.
inline size_t hashCombine(size_t seed, size_t v) { return seed ^ (v + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)); }

template <class Tag>
struct std::hash<Id<Tag>> {
  size_t operator()(Id<Tag> i) const { return std::hash<uint32_t>()(i.value); }
};
template <>
struct std::hash<NodeRef> {
  size_t operator()(NodeRef n) const { return hashCombine(static_cast<size_t>(n.type), n.id); }
};
template <>
struct std::hash<EdgeKey> {
  size_t operator()(const EdgeKey & k) const {
    size_t h = std::hash<InstrumentId>()(k.base);
    h = hashCombine(h, std::hash<InstrumentId>()(k.quote));
    h = hashCombine(h, std::hash<MarketId>()(k.market));
    return hashCombine(h, std::hash<DatasetId>()(k.dataset));
  }
};
template <>
struct std::hash<SeriesKey> {
  size_t operator()(const SeriesKey & k) const {
    return hashCombine(std::hash<EdgeKey>()(k.edge), std::hash<int>()(static_cast<int>(k.resolution)));
  }
};
template <>
struct std::hash<EventKey> {
  size_t operator()(const EventKey & k) const {
    return hashCombine(std::hash<InstrumentId>()(k.instrument), std::hash<DatasetId>()(k.dataset));
  }
};
