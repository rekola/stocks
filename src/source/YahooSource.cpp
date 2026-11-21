#include "source/YahooSource.h"

#include "model/Dates.h"
#include "source/InstrumentTypes.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <thread>

using nlohmann::json;

namespace {

// Element i of a column of numbers, or nothing where Yahoo has a null (a day it has no price for).
std::optional<double> number(const json & column, size_t i) {
  if (!column.is_array() || i >= column.size() || !column[i].is_number()) return std::nullopt;
  double v = column[i].get<double>();
  return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

// The instrument an entry is about. Yahoo gives no ISIN, so it cannot make a security: it uses the one another source
// made, found by the symbol that the entry's market calls it. A currency is its code, which is enough to make.
bool findBase(Store & store, const ListEntry & e, Instrument & base, std::string & error) {
  if (e.kind == InstrumentKind::Currency) {
    base = currencyNode(store, e.base);
    return true;
  }
  auto snapshot = store.snapshot();
  auto market = snapshot->findMarket(e.market);
  auto edge = market ? snapshot->findEdge(predicate::symbol, e.base, market->id) : std::nullopt;
  auto found = edge ? snapshot->instrument(edge->base) : std::nullopt;
  if (!found) {
    error = "no instrument called " + e.base + " on " + e.market + " yet";
    return false;
  }
  base = *found;
  return true;
}

}  // namespace

bool YahooSource::parse(const std::string & body, const ListEntry & e, Store & store, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("chart")) {
    error = "not a chart response";
    return false;
  }
  const json & chart = doc["chart"];
  if (chart.contains("error") && chart["error"].is_object()) {
    error = chart["error"].value("description", std::string("the provider reported an error"));
    return false;
  }
  if (!chart.contains("result") || !chart["result"].is_array() || chart["result"].empty()) {
    error = "no result";
    return false;
  }
  const json & result = chart["result"][0];
  if (!result.contains("timestamp") || !result["timestamp"].is_array() || !result.contains("indicators") ||
      !result["indicators"].contains("quote") || result["indicators"]["quote"].empty()) {
    error = "no candles";
    return false;
  }
  const json & ts = result["timestamp"];
  const json & q = result["indicators"]["quote"][0];

  // A daily bar is keyed by its trading date, not by the instant Yahoo stamps it with: the start of the trading day in the
  // exchange's time zone, for which the chart's meta has today's offset from UTC (see tradingDateOfDailyStamp).
  int utcOffset = 0;
  if (auto meta = result.find("meta"); meta != result.end() && meta->is_object()) {
    auto offset = meta->find("gmtoffset");
    if (offset != meta->end() && offset->is_number_integer()) utcOffset = offset->get<int>();
  }
  std::vector<Candle> bars;
  for (size_t i = 0; i < ts.size(); i++) {
    auto open = number(q["open"], i), high = number(q["high"], i), low = number(q["low"], i), close = number(q["close"], i);
    if (!ts[i].is_number_integer() || !open || !high || !low || !close) continue;  // a day without a price
    bars.push_back({tradingDateOfDailyStamp(ts[i].get<int64_t>(), utcOffset), *open, *high, *low, *close, number(q["volume"], i).value_or(kUndefined)});
  }
  if (bars.empty()) {
    error = "no candles";
    return false;
  }

  Instrument base;
  if (!findBase(store, e, base, error)) return false;
  Dataset dataset = store.addDataset({{}, "yahoo"});
  Market market = store.addMarket({{}, e.market});
  Instrument quote = currencyNode(store, e.quote);
  const EdgeKey edge{base.id, quote.id, market.id, dataset.id};
  store.appendCandles(edge, Resolution::Day, bars);
  store.setEdgeAttribute(edge, {predicate::symbol, e.yahoo});  // what Yahoo calls it
  if (auto meta = result.find("meta"); meta != result.end() && meta->is_object()) {
    auto t = meta->find("instrumentType");
    if (t != meta->end() && t->is_string()) store.setEdgeAttribute(edge, {predicate::type, t->get<std::string>()});
  }

  if (result.contains("events") && result["events"].is_object()) {
    const json & events = result["events"];
    std::vector<CorporateAction> actions;
    if (events.contains("dividends") && events["dividends"].is_object())
      for (const auto & [key, d] : events["dividends"].items())
        if (d.contains("date") && d.contains("amount") && d["date"].is_number_integer() && d["amount"].is_number())
          actions.push_back({tradingDateOfDailyStamp(d["date"].get<int64_t>(), utcOffset), ActionKind::Dividend, d["amount"].get<double>(), quote.id, ""});
    if (events.contains("splits") && events["splits"].is_object())
      for (const auto & [key, s] : events["splits"].items()) {
        double num = s.value("numerator", 0.0), den = s.value("denominator", 0.0);
        if (s.contains("date") && s["date"].is_number_integer() && num > 0 && den > 0)
          actions.push_back({tradingDateOfDailyStamp(s["date"].get<int64_t>(), utcOffset), ActionKind::Split, num / den, {}, s.value("splitRatio", std::string())});
      }
    store.appendActions({base.id, dataset.id}, actions);
  }
  return true;
}

void YahooSource::load(Store & store, Http & http, SourceListener & listener) {
  static const char * const kHosts[] = {"query1.finance.yahoo.com", "query2.finance.yahoo.com"};
  InstrumentList list = list_;
  if (fromFiles_) {
    std::string error;
    list = resolveList(spec_, error);
    if (list.empty()) {
      listener.message("yahoo: " + error);
      return;
    }
  }
  size_t waiting = 0;  // entries with no instrument yet: no request is made for them
  for (const ListEntry & e : list) {
    if (listener.cancelled()) return;
    std::string error;
    {
      Instrument base;
      if (!findBase(store, e, base, error)) {
        waiting++;
        continue;
      }
    }
    bool done = false;
    for (const char * host : kHosts) {  // the second host answers when the first does not
      HttpResult r = http.get(std::string("https://") + host + "/v8/finance/chart/" + e.yahoo + "?range=" + range_ +
                              "&interval=1d&events=div%7Csplit");
      if (r.status == 429) {
        listener.message("yahoo: rate limited, stopping");
        return;
      }
      if (!r.ok()) {
        error = r.status == 0 ? (r.error.empty() ? "no answer" : r.error) : "HTTP " + std::to_string(r.status);
        continue;
      }
      done = parse(r.body, e, store, error);
      break;  // the provider answered; asking another host would give the same
    }
    if (done) listener.changed();
    else listener.message(std::string("yahoo: ") + e.yahoo + ": " + error);
    if (pause_.count() > 0) std::this_thread::sleep_for(pause_);
  }
  if (waiting)
    listener.message("yahoo: " + std::to_string(waiting) + " of " + std::to_string(list.size()) +
                     " entries wait for an instrument that another source makes (Yahoo makes none)");
}
