#pragma once

#include "model/NodeKey.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include <functional>
#include <immer/flex_vector.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// The data model's plain values. Nothing here knows the screen or where data comes from.

// An immutable, time-ordered sequence. Slices and copies share storage, and a reference into one stays valid for as
// long as that sequence is held.
template <class T>
using Seq = immer::flex_vector<T>;

// A node is named by its key (NodeKey.h): the same number in every run, made
// from what the node is (an ISIN, a code, a MIC). A default-constructed id (0)
// is "none".
template <class Tag>
struct Id {
  uint64_t value = 0;
  bool valid() const { return value != 0; }
  friend bool operator==(Id a, Id b) { return a.value == b.value; }
  friend bool operator<(Id a, Id b) { return a.value < b.value; }
};
using InstrumentId = Id<struct InstrumentTag>;
using MarketId = Id<struct MarketTag>;
using DatasetId = Id<struct DatasetTag>;
using PortfolioId = Id<struct PortfolioTag>;  // a minted key: the user's own

// An instant: whole seconds since the Unix epoch, UTC. Where whole seconds are not enough (order book snapshots) it is
// floating-point seconds, never milliseconds. Sessions and time zones belong to the market, not here. A date is not an
// instant: it is a day number (Dates.h).
using Timestamp = int64_t;

// The time of a fact's write, which is kept to the millisecond: milliseconds since the Unix epoch, UTC. Only the store
// assigns one, where the write happens.
using TimestampMs = int64_t;

// A half-open range [from, to), in whatever the thing it selects is keyed by: seconds, slot numbers or day numbers.
struct TimeRange {
  Timestamp from = std::numeric_limits<Timestamp>::min(), to = std::numeric_limits<Timestamp>::max();
  bool contains(Timestamp t) const { return t >= from && t < to; }
};

// What a triple says: text, a whole number (a day is a day number), a real number, a truth value or a node (a link: the key of
// another node, which the viewer shows as the node it names). A number that is not given is not said at all: there is no
// undefined value in a triple. A link says nothing if its key is none.
using Value = std::variant<std::monostate, std::string, int64_t, double, bool, NodeKey>;

inline std::optional<std::string> asText(const Value & v) {
  return std::holds_alternative<std::string>(v) ? std::optional<std::string>(std::get<std::string>(v)) : std::nullopt;
}
inline std::optional<int64_t> asInteger(const Value & v) {
  return std::holds_alternative<int64_t>(v) ? std::optional<int64_t>(std::get<int64_t>(v)) : std::nullopt;
}
inline std::optional<double> asReal(const Value & v) {  // a whole number is a real one too
  if (std::holds_alternative<double>(v)) return std::get<double>(v);
  if (std::holds_alternative<int64_t>(v)) return static_cast<double>(std::get<int64_t>(v));
  return std::nullopt;
}
inline std::optional<bool> asBoolean(const Value & v) {
  return std::holds_alternative<bool>(v) ? std::optional<bool>(std::get<bool>(v)) : std::nullopt;
}
inline std::optional<NodeKey> asNode(const Value & v) {
  return std::holds_alternative<NodeKey>(v) ? std::optional<NodeKey>(std::get<NodeKey>(v)) : std::nullopt;
}

// One thing said of a node: a triple (subject, predicate, object), the subject being the node. Instruments, markets and
// datasets keep what is known about them in the same way: their name, ISIN and kind, an instrument's icon, the number of
// its shares, who runs it. These are not part of any market, so no dataset says them: one value per predicate, a value
// that changes replaces the one there (the last said stands, until a rule picks the authoritative source). What predicates
// there are is data. Edges are not nodes: a fat edge is a Listing, below, which has the dataset.
//
// A fact also says when it was stored: `updated` is when a source last said it, whether or not the value was the same as
// before, to within the store's recheck interval (an hour by default): a value said again sooner than that after `updated`
// leaves it as it was. It is set by the store where the write happens, 0 on one a source has just made. It does not say when
// the value last changed: sources are read too seldom to know that. Two attributes are equal when they say the same, whatever the
// time: "did it change" never looks at it.
struct Attribute {
  std::string predicate;
  Value value;
  TimestampMs updated = 0;
  Attribute(std::string p, std::string v) : predicate(std::move(p)), value(std::move(v)) {}
  Attribute(std::string p, const char * v) : predicate(std::move(p)), value(std::string(v)) {}
  Attribute(std::string p, int64_t v) : predicate(std::move(p)), value(v) {}
  Attribute(std::string p, double v) : predicate(std::move(p)), value(v) {}
  Attribute(std::string p, bool v) : predicate(std::move(p)), value(v) {}
  Attribute(std::string p, NodeKey v) : predicate(std::move(p)), value(v) {}
  Attribute(std::string p, Value v) : predicate(std::move(p)), value(std::move(v)) {}
  friend bool operator==(const Attribute & a, const Attribute & b) { return a.predicate == b.predicate && a.value == b.value; }
};


namespace predicate {
inline constexpr const char * name = "name";
inline constexpr const char * symbol = "symbol";
inline constexpr const char * isin = "isin";
inline constexpr const char * kind = "kind";
inline constexpr const char * type = "type";  // the provider's own type
inline constexpr const char * country = "country";  // a market's country (ISO 3166 alpha-2)
inline constexpr const char * timezone = "timezone";  // a market's time zone (IANA name)
inline constexpr const char * iconUrl = "icon-url";
inline constexpr const char * webUrl = "web-url";  // the instrument's page on its provider's website
inline constexpr const char * firstTradeDay = "first-trade-day";  // a day number: where an instrument's history begins
inline constexpr const char *mic =
    "mic"; // a market's ISO 10383 code: its key, said as a fact too
inline constexpr const char * currency = "currency";  // a portfolio's: a link to the currency node its value is shown in
inline constexpr const char * ledger = "ledger";      // a portfolio's: a link to the dataset node its holdings are derived from
}  // namespace predicate

// The units a quote can be in besides a currency (instruments of kind Unit): index points, and per cent for a rate. What a row is
// (a price or a rate) is decided by its quote's unit, never by its provider; see `isRate` (Period.h).
inline constexpr const char * kPointsUnit = "pts";
inline constexpr const char * kPercentUnit = "pct";

enum class NodeType : uint8_t { Instrument, Market, Dataset, Portfolio };

// Any node. The typed ids convert to it.
struct NodeRef {
  NodeType type = NodeType::Instrument;
  uint64_t id = 0;  // the node's key
  NodeRef() = default;
  NodeRef(InstrumentId i) : type(NodeType::Instrument), id(i.value) {}
  NodeRef(MarketId i) : type(NodeType::Market), id(i.value) {}
  NodeRef(DatasetId i) : type(NodeType::Dataset), id(i.value) {}
  NodeRef(PortfolioId i) : type(NodeType::Portfolio), id(i.value) {}
  friend bool operator==(const NodeRef &, const NodeRef &) = default;
};

// The node a key names, as a NodeRef: an instrument or a currency is an instrument.
inline NodeRef nodeOf(NodeKey key) {
  NodeRef n;
  n.id = key.value;
  switch (key.kind()) {
    case NodeKind::Market: n.type = NodeType::Market; break;
    case NodeKind::Dataset: n.type = NodeType::Dataset; break;
    case NodeKind::Portfolio: n.type = NodeType::Portfolio; break;
    default: n.type = NodeType::Instrument; break;  // an instrument or a currency
  }
  return n;
}

// A price, volume or other number a provider does not give is undefined (NaN), not zero.
inline constexpr double kUndefined = std::numeric_limits<double>::quiet_NaN();
inline bool isDefined(double v) { return v == v; }

// What an instrument is, in one vocabulary for all providers: each adapter maps its own types onto these. The provider's own type is kept beside it, under `predicate::type`.
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

// An instrument is not a record either: it is its `InstrumentId` (an ISIN, or a currency's or a unit's code), and what is said of it
// is facts: the symbol, the name, the kind and the provider's own type ("UETF", "EQUITY"), read with `Snapshot::symbol()`, `name()`,
// `kind()` and `said()`; the ISIN is the id itself (`Snapshot::isin()`). One listed on two markets is one instrument.

// A market and a dataset are not records: a market is its MIC (`MarketId`, the
// ISO 10383 code, four characters, which never changes) and a dataset is its
// code (`DatasetId`, one to ten letters or digits that its adapter defines and
// never changes); what is said of either, its name among that, is facts
// (`Snapshot::name()`, `attribute()`).
inline std::string datasetCode(DatasetId id) { return text(NodeKey{id.value}); }
inline std::string marketCode(MarketId id) { return text(NodeKey{id.value}); }

// The markets that are no venue: listed instruments traded off an exchange (the
// funds of a fund company), and no market at all (an index, a spot price, a
// currency pair that has none). A venue has its own MIC.
inline constexpr const char *kOffExchange = "XOFF";
inline constexpr const char *kNoMarket = "XXXX";

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

// The key of the transactions of a portfolio: what this dataset says about this portfolio (the user's own entries, later a broker's
// export). The holdings are derived from one dataset, the one the portfolio's `ledger` names.
struct LedgerKey {
  PortfolioId portfolio;
  DatasetId dataset;
  friend bool operator==(const LedgerKey &, const LedgerKey &) = default;
  friend bool operator<(const LedgerKey & a, const LedgerKey & b) {
    return std::tie(a.portfolio, a.dataset) < std::tie(b.portfolio, b.dataset);
  }
};

// A **fat edge** is a Listing: an instrument on a market in a quote currency, as one dataset says it. It is a row of fixed
// fields carrying its own key; it is small, and the price series (candles, books) are beside it under the same key. A
// **thin edge** is a triple, an Attribute of a node, above: it is not part of a market, so it has no dataset.

struct Listing {
  EdgeKey key;
  std::string symbol;      // what the market calls the instrument
  std::string identifier;  // the market's own id for it
  std::string orderBook;   // the id the price history is asked for by
  std::string type;        // the provider's own type for it
  std::string name;        // what the dataset calls it there ("EUR/USD", a fund's name on that market): what a row shows
  friend bool operator==(const Listing &, const Listing &) = default;
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
  size_t operator()(Id<Tag> i) const { return std::hash<uint64_t>()(i.value); }
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
struct std::hash<LedgerKey> {
  size_t operator()(const LedgerKey & k) const {
    return hashCombine(std::hash<PortfolioId>()(k.portfolio), std::hash<DatasetId>()(k.dataset));
  }
};
template <>
struct std::hash<EventKey> {
  size_t operator()(const EventKey & k) const {
    return hashCombine(std::hash<InstrumentId>()(k.instrument), std::hash<DatasetId>()(k.dataset));
  }
};
