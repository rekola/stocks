#include "source/KarSource.h"

#include "source/InstrumentLists.h"
#include "source/KnownIsins.h"
#include "source/KnownMarkets.h"
#include "source/WeeklyBars.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>

using json = nlohmann::json;

namespace {

constexpr const char * kEndpoint = "https://karasmussen.fi/wp-json/price-history/v1/prices";
constexpr const char * kPage = "https://karasmussen.fi/paivan-hinnat/";  // the page whose script asks, which is the Referer
constexpr const char * kMarket = "XLBM";                                  // the London Bullion Market
constexpr const char * kFirstDay = "2019-11-01";                         // the first price of any metal is of 2019-11-13

struct Metal {
  const char * code;  // the answer's
  const char * symbol, *name, *isin;
};
constexpr Metal kMetals[] = {{"AU", "XAU", "Gold", known_isin::kGold},
                             {"AG", "XAG", "Silver", known_isin::kSilver},
                             {"PT", "XPT", "Platinum", known_isin::kPlatinum},
                             {"PD", "XPD", "Palladium", known_isin::kPalladium}};

const Metal * metalOfCode(const std::string & code) {
  for (const Metal & m : kMetals)
    if (code == m.code) return &m;
  return nullptr;
}

std::optional<double> number(const json & row, const char * key) {
  const auto it = row.find(key);
  if (it == row.end() || !it->is_number()) return std::nullopt;
  const double v = it->get<double>();
  return std::isfinite(v) && v > 0 ? std::optional<double>(v) : std::nullopt;
}

std::string dateText(int64_t dayNumber) {
  const Date d = dateOfDayNumber(dayNumber);
  char text[16];
  std::snprintf(text, sizeof text, "%04d-%02u-%02u", d.year, d.month, d.day);
  return text;
}

}  // namespace

KarSource::KarSource(std::chrono::milliseconds refresh, std::function<Timestamp()> now) : refresh_(refresh), now_(std::move(now)) {
  if (!now_) now_ = [] { return static_cast<Timestamp>(std::time(nullptr)); };
}

bool KarSource::parse(const std::string & body, Parsed & out, std::string & error) {
  out = {};
  const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_array()) {
    error = "not a list of metals";
    return false;
  }
  bool any = false;
  for (const json & metal : doc) {
    if (!metal.is_object() || !metal.contains("Prices") || !metal["Prices"].is_array() || !metal.contains("Code") || !metal["Code"].is_string()) continue;
    any = true;
    const std::string code = metal["Code"].get<std::string>();
    if (!metalOfCode(code)) continue;
    for (const json & price : metal["Prices"]) {
      if (!price.is_object()) {
        out.skipped++;
        continue;
      }
      const auto currency = price.find("Currency");
      if (currency == price.end() || !currency->is_string() || currency->get<std::string>() != "USD") continue;  // the others are the dollar's, converted
      // "2026-11-26T15:03:00": UTC, though it says no zone.
      const auto time = price.contains("TradeTime") && price["TradeTime"].is_string() ? instantOfIsoTimestamp(price["TradeTime"].get<std::string>() + "Z") : std::nullopt;
      const auto bid = number(price, "Bid"), ask = number(price, "Ask");
      if (!time || !bid || !ask || *bid > *ask) {
        out.skipped++;
        continue;
      }
      out.rows.push_back({code, *time, (*bid + *ask) / 2});
    }
  }
  if (!any) {
    error = "no metals in the answer";
    return false;
  }
  return true;
}

std::string KarSource::url(Timestamp last) { return std::string(kEndpoint) + "?from=" + kFirstDay + "&to=" + dateText(dayNumberOfInstant(last)); }

void KarSource::load(Store & store, Http & http, SourceListener & listener) {
  if (listener.cancelled()) return;
  const std::string address = url(now_());
  const HttpResult r = http.get(address, browserHeaders(address, {{"Referer", kPage}}));
  Parsed parsed;
  std::string error;
  if (!r.ok() || !parse(r.body, parsed, error)) {
    listener.message("kar: " + (!r.ok() ? (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)) : error));
    return;
  }
  if (parsed.skipped > 0) listener.message("kar: " + std::to_string(parsed.skipped) + " rows that are not prices left out");

  struct Series {
    std::vector<Candle> bars;
    Row last;
  };
  std::map<std::string, Series> series;  // by metal
  for (const Row & row : parsed.rows) {
    Series & s = series[row.metal];
    s.bars.push_back({dayNumberOfInstant(row.time), kUndefined, kUndefined, kUndefined, row.price, kUndefined});  // one value a day, as a rate is
    if (s.last.time == 0 || row.time > s.last.time) s.last = row;
  }
  if (series.empty()) {
    listener.message("kar: no prices in the answer");
    return;
  }
  store.batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset(kKarDataset, "KAR");
    const MarketId market = b.addMarket(kMarket, marketNameOfMic(kMarket));  // the fix is the London Bullion Market's price
    const InstrumentId quote = currencyNode(b, "USD");
    for (auto & [code, s] : series) {
      const Metal * metal = metalOfCode(code);
      std::sort(s.bars.begin(), s.bars.end(), [](const Candle & x, const Candle & y) { return x.time < y.time; });
      const InstrumentId base = addInstrumentUnlessSaid(b, metal->symbol, metal->name, InstrumentKind::Commodity, metal->isin, "METAL");
      if (!base.valid() || !quote.valid()) continue;
      const std::vector<Candle> weeks = weeksOfDays(s.bars);  // the 3 and 10 years read weeks, which the answer has none of
      Quote last;
      last.last = s.last.price;
      last.time = s.last.time;
      const EdgeKey edge{base, quote, market, dataset};
      b.appendCandles(edge, Resolution::Day, s.bars, QueueKind::Scalar);
      b.appendCandles(edge, Resolution::Week, weeks, QueueKind::Scalar);
      b.setQuote(edge, last);
      b.setListing({edge, metal->symbol, "", "", "METAL", std::string(metal->name) + ", per troy oz"});
    }
  });
  listener.changed();
}
