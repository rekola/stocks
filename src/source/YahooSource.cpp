#include "source/YahooSource.h"

#include "model/Dates.h"
#include "source/InstrumentTypes.h"

#include <nlohmann/json.hpp>

#include <cmath>

using nlohmann::json;

namespace {

// The member `key` of `o`, if `o` is an object and has it.
const json * member(const json & o, const char * key) {
  if (!o.is_object()) return nullptr;
  auto it = o.find(key);
  return it == o.end() ? nullptr : &*it;
}

// Element i of a column of numbers, or nothing where Yahoo has a null (a day it has no price for).
std::optional<double> number(const json & column, size_t i) {
  if (!column.is_array() || i >= column.size() || !column[i].is_number()) return std::nullopt;
  double v = column[i].get<double>();
  return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
}

// The instrument an entry is about: its ISIN is in the data (Yahoo gives none); a currency is its code.
InstrumentId baseOf(Batch & store, const ListEntry & e) {
  if (e.kind == InstrumentKind::Currency) return currencyNode(store, e.base);
  return addInstrumentUnlessSaid(store, "", e.name, e.kind, e.isin);
}

// The ticker as a path segment: an index's caret is escaped.
std::string encoded(const std::string & ticker) {
  std::string out;
  for (char c : ticker) out += c == '^' ? std::string("%5E") : std::string(1, c);
  return out;
}

}  // namespace

bool YahooSource::parse(const std::string & body, const ListEntry & e, Store & store, std::string & error) {
  // What the answer holds is looked up with find() on what is checked to be an object: a const operator[] on a key that is not
  // there is undefined, and on what is not an object it throws.
  const json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  const json * chart = member(doc, "chart");
  if (!chart || !chart->is_object()) {
    error = "not a chart response";
    return false;
  }
  if (const json * failure = member(*chart, "error"); failure && failure->is_object()) {
    const json * description = member(*failure, "description");
    error = description && description->is_string() ? description->get<std::string>() : "the provider reported an error";
    return false;
  }
  const json * results = member(*chart, "result");
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
  const json & ts = *timestamps;
  const json & q = quotes->front();
  // A column the answer lacks is all nulls.
  static const json kNoColumn;
  auto column = [&](const char * key) -> const json & {
    const json * c = member(q, key);
    return c ? *c : kNoColumn;
  };
  const json &open = column("open"), &high = column("high"), &low = column("low"), &close = column("close"), &volume = column("volume");

  // A daily bar is keyed by its trading date, not by the instant Yahoo stamps it with: the start of the trading day in the
  // exchange's time zone, for which the chart's meta has today's offset from UTC (see tradingDateOfDailyStamp).
  int utcOffset = 0;
  if (auto meta = result.find("meta"); meta != result.end() && meta->is_object()) {
    auto offset = meta->find("gmtoffset");
    if (offset != meta->end() && offset->is_number_integer()) utcOffset = offset->get<int>();
  }
  std::vector<Candle> bars;
  for (size_t i = 0; i < ts.size(); i++) {
    auto o = number(open, i), h = number(high, i), l = number(low, i), c = number(close, i);
    if (!ts[i].is_number_integer() || !o || !h || !l || !c) continue;  // a day without a price
    bars.push_back({tradingDateOfDailyStamp(ts[i].get<int64_t>(), utcOffset), *o, *h, *l, *c, number(volume, i).value_or(kUndefined)});
  }
  if (bars.empty()) {
    error = "no candles";
    return false;
  }

  // One entry is one publication: the bars, the listing and what is said of the instrument together.
  store.batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset(kYahooDataset);
    const MarketId market = b.addMarket(e.mic, e.market);
    const InstrumentId base = baseOf(b, e);
    const InstrumentId quote = quoteNode(b, e.quote);
    const EdgeKey edge{base, quote, market, dataset};
    b.appendCandles(edge, Resolution::Day, bars);
    // What Yahoo calls it, its own type (below) and the name a row shows: a pair is named by its two currencies, not by the
    // base's name, which every pair of that base would share.
    Listing listing{edge, e.yahoo, "", "", "", e.kind == InstrumentKind::Currency ? e.base + "/" + e.quote : e.name};
    std::vector<Attribute> said;
    if (auto meta = result.find("meta"); meta != result.end() && meta->is_object()) {
      auto t = meta->find("instrumentType");
      if (t != meta->end() && t->is_string()) listing.type = t->get<std::string>();
      // Where the history begins, and the market's time zone (which a market has, not an instrument).
      auto first = meta->find("firstTradeDate");
      if (first != meta->end() && first->is_number_integer() && first->get<int64_t>() > 0)
        said.push_back({predicate::firstTradeDay, int64_t(dayNumberOfInstant(first->get<int64_t>(), utcOffset))});
      auto zone = meta->find("exchangeTimezoneName");
      if (zone != meta->end() && zone->is_string()) b.setAttribute(market, {predicate::timezone, zone->get<std::string>()});
    }
    b.setListing(listing);
    b.setAttributes(base, said);

    if (const json * events = member(result, "events"); events && events->is_object()) {
      std::vector<CorporateAction> actions;
      if (const json * dividends = member(*events, "dividends"); dividends && dividends->is_object())
        for (const auto & [key, d] : dividends->items()) {
          const json *date = member(d, "date"), *amount = member(d, "amount");
          if (date && amount && date->is_number_integer() && amount->is_number())
            actions.push_back({tradingDateOfDailyStamp(date->get<int64_t>(), utcOffset), ActionKind::Dividend, amount->get<double>(), quote, ""});
        }
      if (const json * splits = member(*events, "splits"); splits && splits->is_object())
        for (const auto & [key, sp] : splits->items()) {
          const json *date = member(sp, "date"), *numerator = member(sp, "numerator"), *denominator = member(sp, "denominator"),
                     *ratio = member(sp, "splitRatio");
          const double num = numerator && numerator->is_number() ? numerator->get<double>() : 0.0;
          const double den = denominator && denominator->is_number() ? denominator->get<double>() : 0.0;
          if (date && date->is_number_integer() && num > 0 && den > 0)
            actions.push_back({tradingDateOfDailyStamp(date->get<int64_t>(), utcOffset), ActionKind::Split, num / den, {},
                               ratio && ratio->is_string() ? ratio->get<std::string>() : std::string()});
        }
      b.appendActions({base, dataset}, actions);
    }
  });
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
  for (const ListEntry & e : list) {
    if (listener.cancelled()) return;
    std::string error;
    bool done = false;
    for (const char * host : kHosts) {  // the second host answers when the first does not
      HttpResult r = http.get(std::string("https://") + host + "/v8/finance/chart/" + encoded(e.yahoo) + "?range=" + range_ +
                              "&interval=1d&events=div%7Csplit");
      if (r.status == 429 || r.status == 403) {
        listener.message(r.status == 429 ? "yahoo: rate limited, stopping" : "yahoo: refused, stopping");
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
    if (pause_.count() > 0 && !listener.pause(pause_)) return;
  }
}
