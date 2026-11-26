#include "source/GoldPriceHistory.h"

#include "model/Dates.h"
#include "source/GoldPriceSource.h"

#include <cellui/NumberParse.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

using json = nlohmann::json;

namespace {

constexpr const char * kBars = "https://api.goldprice.dev/v1/bars";
constexpr int kLimit = 1000;  // bars a page: the answer pages by cursor, and a month is a few dozen

std::optional<double> amount(const json & row, const char * key) {
  auto it = row.find(key);
  if (it == row.end()) return std::nullopt;
  if (it->is_number()) return it->get<double>();
  if (!it->is_string()) return std::nullopt;
  const std::string s = it->get<std::string>();
  double v = 0;
  const auto [end, ec] = cellui::parseFloat(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size() || !std::isfinite(v)) return std::nullopt;
  return v;
}

std::string isoDate(int64_t day) {
  const Date d = dateOfDayNumber(day);
  char buffer[16];
  std::snprintf(buffer, sizeof buffer, "%04d-%02u-%02u", d.year, d.month, d.day);
  return buffer;
}

}  // namespace

std::string GoldPriceHistory::dataset() const { return kGoldPriceDataset; }

std::optional<HistoryProvider::Need> GoldPriceHistory::need(const Snapshot & snapshot, const EdgeKey & edge, Period period) const {
  // Daily bars only: the day and the week are intraday, which the plan has none of.
  if (period == Period::Day || period == Period::Week) return std::nullopt;
  if (!snapshot.listing(edge)) return std::nullopt;
  const Covered covered = periodCovered(snapshot, edge, period);
  return Need{0, covered.covered, covered.fetched};
}

bool GoldPriceHistory::parse(const std::string & body, Bars & out, std::string & error) {
  out = {};
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("bars") || !doc["bars"].is_array()) {
    error = "not a list of bars";
    return false;
  }
  for (const json & row : doc["bars"]) {
    if (!row.is_object()) continue;
    auto start = row.find("bar_start");
    const auto close = amount(row, "close");
    if (start == row.end() || !start->is_string() || !close || *close <= 0) continue;
    const auto day = dayNumberOfIsoDate(start->get<std::string>().substr(0, 10));
    if (!day) continue;
    Candle c;
    c.time = *day;
    c.open = amount(row, "open").value_or(kUndefined);
    c.high = amount(row, "high").value_or(kUndefined);
    c.low = amount(row, "low").value_or(kUndefined);
    c.close = *close;
    c.volume = amount(row, "volume").value_or(kUndefined);
    out.bars.push_back(c);
  }
  // The answer is newest first: the store takes them oldest first.
  std::sort(out.bars.begin(), out.bars.end(), [](const Candle & a, const Candle & b) { return a.time < b.time; });
  if (auto next = doc.find("next_cursor"); next != doc.end() && next->is_string()) out.next_cursor = next->get<std::string>();
  return true;
}

HistoryProvider::Outcome GoldPriceHistory::load(Store & store, Http & http, const EdgeKey & edge, int, const Wait & wait) {
  const auto snapshot = store.snapshot();
  const auto listing = snapshot->listing(edge);
  const std::string quote = snapshot->symbol(edge.quote);
  if (!listing || listing->symbol.empty() || quote.empty()) return Outcome::Missing;
  const int64_t today = dayNumberOfInstant(static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
  const std::string symbol = listing->symbol + "/" + quote;
  std::string cursor;
  std::vector<Candle> all;
  for (int page = 0; page < 20; page++) {  // a month is one page: the bound is for a cursor that does not end
    std::string url = std::string(kBars) + "?symbol=" + symbol + "&interval=1d&from=" + isoDate(today - options_.days) + "&to=" + isoDate(today) +
                      "&limit=" + std::to_string(kLimit);
    if (!cursor.empty()) url += "&cursor=" + cursor;
    const HttpResult r = http.get(url, {});
    if (r.status == 429) return Outcome::Refused;
    if (r.status == 0 || r.status >= 500) return Outcome::Offline;
    if (!r.ok()) return Outcome::Missing;  // 403 plan_gated is for this edge (the plan has not its bars), not a refusal of the API
    Bars bars;
    std::string error;
    if (!parse(r.body, bars, error)) return Outcome::Missing;
    all.insert(all.end(), bars.bars.begin(), bars.bars.end());
    if (bars.next_cursor.empty()) break;
    cursor = bars.next_cursor;
    if (!wait()) return Outcome::Offline;
  }
  if (all.empty()) return Outcome::Missing;  // the plan has no bars of this symbol and currency
  std::sort(all.begin(), all.end(), [](const Candle & a, const Candle & b) { return a.time < b.time; });
  store.appendCandles(edge, Resolution::Day, all);
  return Outcome::Ok;
}
