#include "source/NordnetHistory.h"
#include "source/DatasetCodes.h"
#include "source/NordnetHeaders.h"

#include "model/Dates.h"

#include <nlohmann/json.hpp>

#include <iterator>

using nlohmann::json;

namespace {

const char * const kUrl = "https://api.prod.nntech.io/market-data/v3/price-time-series/period/";

// What each request asks for: Nordnet's period and the resolution (the answer says what it returned).
struct Ask {
  const char * period;
  const char * resolution;
};
constexpr Ask kAsks[] = {{"MONTH_3", "DAY"}, {"DAY_1", "MIN_5"}, {"WEEK_1", "HOUR_1"}, {"YEAR_1", "WEEK"}, {"YEAR_3", "WEEK"}, {"YEAR_10", "WEEK"}};

// Which request feeds a period. A fund is priced once a day, so it has no intraday bars to ask for.
int requestOf(Period period, bool nav) {
  switch (period) {
    case Period::Day: return nav ? NordnetHistory::Quarter : NordnetHistory::Day;
    case Period::Week: return nav ? NordnetHistory::Quarter : NordnetHistory::Week;
    case Period::Year: return NordnetHistory::Year;
    case Period::Year3: return NordnetHistory::Year3;
    case Period::Year10: return NordnetHistory::Year10;
    default: return NordnetHistory::Quarter;  // the month is part of the quarter
  }
}

// The periods tried for daily bars, longest first, where the whole period came back coarser.
const char * const kDailyPeriods[] = {"YEAR_10", "YEAR_5", "YEAR_3", "YEAR_1"};

bool resolutionOf(const std::string & name, Resolution & out) {
  static const std::pair<const char *, Resolution> known[] = {{"MIN_1", Resolution::Min1},
                                                               {"MIN_2", Resolution::Min2},
                                                               {"MIN_5", Resolution::Min5},
                                                               {"MIN_15", Resolution::Min15},
                                                               {"MIN_30", Resolution::Min30},
                                                               {"HOUR_1", Resolution::Hour1},
                                                               {"HOUR_4", Resolution::Hour4},
                                                               {"DAY", Resolution::Day},
                                                               {"WEEK", Resolution::Week},
                                                               {"MONTH", Resolution::Month},
                                                               {"QUARTER", Resolution::Quarter}};
  for (const auto & [n, r] : known)
    if (name == n) {
      out = r;
      return true;
    }
  return false;
}

// The key of a bar: its slot number for an intraday one, else its date as a day number, and for a coarser bar the first day of
// the period it stands for. `ms` is the stamp the answer gives.
int64_t keyOf(Resolution r, int64_t ms) {
  if (!isDateResolution(r)) return slotOfInstant(r, ms / 1000);
  const int64_t day = dayNumberOfInstant(ms / 1000);
  switch (r) {
    case Resolution::Week: return weekStart(day);
    case Resolution::Month: return monthStart(day);
    case Resolution::Quarter: return quarterStart(day);
    default: return day;
  }
}

std::optional<double> number(const json & o, const char * key) {
  auto it = o.find(key);
  return it != o.end() && it->is_number() ? std::optional<double>(it->get<double>()) : std::nullopt;
}

// One request to the price time series: what came of it, and the bars where it answered.
HistoryProvider::Outcome fetch(Http & http, const std::string & book, const std::string & period, const std::string & resolution, bool nav,
                               NordnetHistory::Series & out) {
  std::string url = std::string(kUrl) + period + "/identifier/" + book + "?resolution=" + resolution;
  if (nav) url += "&fundType=FUND_NAV";
  HttpResult r = http.get(url, nordnet::marketDataHeaders(url));
  if (r.status == 403 || r.status == 429) return HistoryProvider::Outcome::Refused;
  if (r.status == 0 || r.status >= 500) return HistoryProvider::Outcome::Offline;
  if (!r.ok()) return HistoryProvider::Outcome::Missing;
  std::string error;
  if (!NordnetHistory::parse(r.body, nav, out, error)) return HistoryProvider::Outcome::Missing;
  return HistoryProvider::Outcome::Ok;
}

}  // namespace

bool NordnetHistory::parse(const std::string & body, bool nav, Series & out, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("pricePoints") || !doc["pricePoints"].is_array()) {
    error = "not a price time series";
    return false;
  }
  out = Series();
  auto name = doc.find("returnedResolution");
  out.supported = name != doc.end() && name->is_string() && resolutionOf(name->get<std::string>(), out.resolution);
  if (auto periods = doc.find("availablePeriods"); periods != doc.end() && periods->is_array())
    for (const json & p : *periods)
      if (p.is_string()) out.periods.push_back(p.get<std::string>());
  if (!out.supported) return true;  // a resolution we do not know: not stored
  if (auto previous = number(doc, "previousClose")) out.previousClose = *previous;
  for (const json & p : doc["pricePoints"]) {
    auto stamp = p.find("timeStamp");
    if (!p.is_object() || stamp == p.end() || !stamp->is_number_integer()) continue;
    const int64_t ms = stamp->get<int64_t>();
    // A bar that does not start on its slot's boundary cannot be keyed by the slot without losing its offset: left out.
    if (!isDateResolution(out.resolution) && !onSlotBoundary(out.resolution, ms / 1000)) continue;
    const int64_t key = keyOf(out.resolution, ms);
    Candle c;
    c.time = key;
    c.open = c.high = c.low = c.volume = kUndefined;
    if (nav) {
      // A fund's NAV: only a value, which is the close.
      out.kind = QueueKind::Scalar;
      auto value = number(p, "value");
      if (!value) continue;
      c.close = *value;
    } else {
      auto last = number(p, "last");
      if (!last) continue;
      c.close = *last;
      c.open = number(p, "open").value_or(kUndefined);
      c.high = number(p, "high").value_or(kUndefined);
      c.low = number(p, "low").value_or(kUndefined);
      // A volume of 0 is what a market with none (a commodity, an ETC) gives: no volume, not no trades.
      if (auto volume = number(p, "volume"); volume && *volume > 0) c.volume = *volume;
    }
    if (!out.bars.empty() && out.bars.back().time == c.time) out.bars.back() = c;  // the same period twice: the later
    else out.bars.push_back(c);
  }
  return true;
}

std::string NordnetHistory::dataset() const { return kNordnetDataset; }
std::vector<std::string> NordnetHistory::datasets() const { return {kNordnetDataset, kMorningstarDataset}; }

std::optional<HistoryProvider::Need> NordnetHistory::need(const Snapshot & snapshot, const EdgeKey & edge, Period period) const {
  auto listing = snapshot.listing(edge);
  if (!listing || listing->orderBook.empty()) return std::nullopt;
  if (options_.depth == Depth::Full) {
    // More than today's bar: the bars are there already (loaded before, or from elsewhere).
    return Need{Whole, snapshot.lastCandles(edge, Resolution::Day, 6).candles.size() >= 6, 0};
  }
  const bool nav = snapshot.kind(edge.base) == InstrumentKind::Fund;
  const int request = requestOf(period, nav);
  // The store has the period's bars already: no request. A fund's day and week are its daily bars.
  const Period shown = nav && request == Quarter && period != Period::Month ? Period::Quarter : period;
  const Covered covered = periodCovered(snapshot, edge, shown);
  return Need{request, covered.covered, covered.fetched};
}

// What the summary keeps of what each request gives: the 5-minute bars of the day (one day), the hourly bars of the week (one week), the daily bars
// of the month and the quarter (three months) and the weekly bars of the year, three years and ten years (the weekly queue keeps ten); no queue of any other resolution.
std::optional<std::chrono::seconds> NordnetHistory::keep(Resolution resolution) const {
  constexpr int64_t kDay = 86400;
  switch (resolution) {
    case Resolution::Min5: return std::chrono::seconds(kDay);
    case Resolution::Hour1: return std::chrono::seconds(7 * kDay);
    case Resolution::Day: return std::chrono::seconds(92 * kDay);
    case Resolution::Week: return std::chrono::seconds(3650 * kDay);
    default: return std::nullopt;
  }
}

std::chrono::milliseconds NordnetHistory::lifetime(int request) const {
  if (options_.depth != Depth::Recent) return std::chrono::milliseconds(0);
  switch (request) {
    case Quarter: return options_.quarter_lifetime;
    case Day: return options_.day_lifetime;
    case Week: return options_.week_lifetime;
    case Year: return options_.year_lifetime;
    case Year3: return options_.year3_lifetime;
    case Year10: return options_.year10_lifetime;
    default: return std::chrono::milliseconds(0);
  }
}

HistoryProvider::Outcome NordnetHistory::load(Store & store, Http & http, const EdgeKey & edge, int request, const Wait & wait) {
  auto snapshot = store.snapshot();
  auto listing = snapshot->listing(edge);
  if (!listing || !snapshot->known(NodeRef(edge.base))) return Outcome::Missing;
  const bool nav = snapshot->kind(edge.base) == InstrumentKind::Fund;

  bool wrote = false, complete = false;
  Outcome failure = Outcome::Ok;
  auto ask = [&](const std::string & period, const std::string & resolution) -> bool {  // false: stop for this edge
    Series series;
    failure = fetch(http, listing->orderBook, period, resolution, nav, series);
    if (failure != Outcome::Ok) return false;
    if (series.supported && !series.bars.empty()) {
      store.appendCandles(edge, series.resolution, series.bars, series.kind);
      wrote = true;
      // The close before the session, for the day's change, belongs to the date of the bars it came with.
      if (request == Day && isDefined(series.previousClose) && !isDateResolution(series.resolution)) {
        const int64_t session = dayNumberOfInstant(instantOfSlot(series.resolution, series.bars.back().time));
        store.setPreviousClose(edge, session, series.previousClose);
      }
    }
    // Daily bars are what a model wants: the whole period came back coarser, so ask for the longest that gives days.
    complete = series.supported && series.resolution == Resolution::Day;
    return true;
  };
  // What the edge has when a later request fails: bars were written, so it is done; else the failure is the answer.
  const auto stopped = [&] { return wrote && failure == Outcome::Missing ? Outcome::Ok : failure; };

  if (request != Whole) {
    if (request < 0 || request >= static_cast<int>(std::size(kAsks))) return Outcome::Missing;
    ask(kAsks[request].period, kAsks[request].resolution);
    return failure;  // one request: the snapshot
  }
  if (!ask("ALL", "DAY")) return stopped();
  for (const char * period : kDailyPeriods) {
    if (complete) break;
    if (!wait()) return Outcome::Offline;  // stopping: the loader is going, so the edge is not marked
    if (!ask(period, "DAY")) return stopped();
  }
  return Outcome::Ok;
}
