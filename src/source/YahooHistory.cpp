#include "source/YahooHistory.h"

#include "model/Dates.h"
#include "source/DatasetCodes.h"

#include <nlohmann/json.hpp>

#include <cmath>

using nlohmann::json;

namespace {

const json * member(const json & o, const char * key) {
  if (!o.is_object()) return nullptr;
  auto it = o.find(key);
  return it == o.end() ? nullptr : &*it;
}

std::optional<double> number(const json * column, size_t i) {
  if (!column || !column->is_array() || i >= column->size() || !(*column)[i].is_number()) return std::nullopt;
  const double v = (*column)[i].get<double>();
  return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

std::string encoded(const std::string & ticker) {
  std::string out;
  for (char c : ticker) out += c == '^' ? std::string("%5E") : std::string(1, c);
  return out;
}

}  // namespace

std::string YahooHistory::dataset() const { return kYahooDataset; }

std::optional<HistoryProvider::Need> YahooHistory::need(const Snapshot & snapshot, const EdgeKey & edge, Period period) const {
  if (period != Period::Year3 && period != Period::Year10) return std::nullopt;  // the list's daily bars are those
  auto listing = snapshot.listing(edge);
  if (!listing || listing->symbol.empty()) return std::nullopt;
  const Covered covered = periodCovered(snapshot, edge, period);
  return Need{0, covered.covered, covered.fetched};
}

bool YahooHistory::parse(const std::string & body, std::vector<Candle> & bars, std::string & error) {
  const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  const json * chart = member(doc, "chart");
  const json * results = chart ? member(*chart, "result") : nullptr;
  if (!results || !results->is_array() || results->empty() || !results->front().is_object()) {
    error = "no result";
    return false;
  }
  const json & result = results->front();
  const json * timestamps = member(result, "timestamp");
  const json * indicators = member(result, "indicators");
  const json * quotes = indicators ? member(*indicators, "quote") : nullptr;
  if (!timestamps || !timestamps->is_array() || !quotes || !quotes->is_array() || quotes->empty() || !quotes->front().is_object()) {
    error = "no candles";
    return false;
  }
  int utcOffset = 0;
  if (const json * meta = member(result, "meta")) {
    const json * offset = member(*meta, "gmtoffset");
    if (offset && offset->is_number_integer()) utcOffset = offset->get<int>();
  }
  const json & q = quotes->front();
  const json *open = member(q, "open"), *high = member(q, "high"), *low = member(q, "low"), *close = member(q, "close"), *volume = member(q, "volume");
  bars.clear();
  for (size_t i = 0; i < timestamps->size(); i++) {
    const json & t = (*timestamps)[i];
    auto o = number(open, i), h = number(high, i), l = number(low, i), c = number(close, i);
    if (!t.is_number_integer() || !o || !h || !l || !c) continue;
    const int64_t key = weekStart(tradingDateOfDailyStamp(t.get<int64_t>(), utcOffset));
    const Candle bar{key, *o, *h, *l, *c, number(volume, i).value_or(kUndefined)};
    if (!bars.empty() && bars.back().time == key) bars.back() = bar;  // the same week twice: the later
    else bars.push_back(bar);
  }
  if (bars.empty()) {
    error = "no candles";
    return false;
  }
  return true;
}

HistoryProvider::Outcome YahooHistory::load(Store & store, Http & http, const EdgeKey & edge, int /*request*/, const Wait &) {
  static const char * const kHosts[] = {"query1.finance.yahoo.com", "query2.finance.yahoo.com"};
  auto listing = store.snapshot()->listing(edge);
  if (!listing || listing->symbol.empty()) return Outcome::Missing;
  Outcome failure = Outcome::Offline;
  for (const char * host : kHosts) {  // the second host answers when the first does not
    HttpResult r = http.get(std::string("https://") + host + "/v8/finance/chart/" + encoded(listing->symbol) + "?range=10y&interval=1wk");
    if (r.status == 403 || r.status == 429) return Outcome::Refused;
    if (r.status == 0 || r.status >= 500) continue;
    if (!r.ok()) return Outcome::Missing;
    std::vector<Candle> bars;
    std::string error;
    if (!parse(r.body, bars, error)) return Outcome::Missing;
    store.appendCandles(edge, Resolution::Week, bars);
    return Outcome::Ok;
  }
  return failure;
}
