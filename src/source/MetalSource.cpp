#include "source/MetalSource.h"

#include "model/Dates.h"
#include "model/Isin.h"
#include "model/NodeKey.h"
#include "source/InstrumentLists.h"
#include "source/KnownMarkets.h"
#include "source/SchemaFile.h"
#include "source/WeeklyBars.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>

using json = nlohmann::json;

namespace {

std::string textOf(const json & d, const char * key, const std::string & otherwise = {}) {
  const auto it = d.find(key);
  return it != d.end() && it->is_string() ? it->get<std::string>() : otherwise;
}

bool plain(const std::string & s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'; });
}

// "2026-11-28T11:36:00Z" for a URL (the colons percent-encoded), or "2026-11-28".
std::string timeText(Timestamp t, const std::string & format) {
  const int64_t days = t >= 0 ? t / 86400 : -((-t + 86399) / 86400);
  const Date d = civilFromUnixDays(days);
  const int64_t rest = t - days * 86400;
  char text[40];
  if (format == "date") std::snprintf(text, sizeof text, "%04d-%02u-%02u", d.year, d.month, d.day);
  else std::snprintf(text, sizeof text, "%04d-%02u-%02uT%02d%%3A%02d%%3A00Z", d.year, d.month, d.day, static_cast<int>(rest / 3600), static_cast<int>(rest % 3600 / 60));
  return text;
}

void replaceAll(std::string & s, const std::string & key, const std::string & value) {
  for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + value.size())) s.replace(at, key.size(), value);
}

// One bar's candle taken into the one before it: the same day (or a weekend that is a Friday's evening).
void join(Candle & into, const Candle & more) {
  if (isDefined(more.high)) into.high = isDefined(into.high) ? std::max(into.high, more.high) : more.high;
  if (isDefined(more.low)) into.low = isDefined(into.low) ? std::min(into.low, more.low) : more.low;
  if (isDefined(more.close)) into.close = more.close;
  if (isDefined(more.volume)) into.volume = isDefined(into.volume) ? into.volume + more.volume : more.volume;
}

}  // namespace

bool MetalSchema::parse(const std::string & text, MetalSchema & out, std::string & error) {
  const json d = json::parse(text, nullptr, /*allow_exceptions=*/false);
  if (d.is_discarded() || !d.is_object()) {
    error = "not JSON, or not an object";
    return false;
  }
  MetalSchema s;
  s.name = textOf(d, "name");
  s.dataset = textOf(d, "dataset");
  if (s.name.empty() || !plain(s.name)) {
    error = "no name (letters and digits)";
    return false;
  }
  if (!datasetKey(s.dataset)) {
    error = "dataset is the code of the dataset, one to eleven capitals, digits, hyphens, periods or hashes (it is never changed), not \"" + s.dataset + "\"";
    return false;
  }
  s.title = textOf(d, "title");
  s.market = textOf(d, "market", s.market);
  s.currencyLabel = textOf(d, "currencyLabel", s.currencyLabel);
  if (!plain(s.market) || s.market.size() != 4) {
    error = "market is the MIC of a market (four letters), not \"" + s.market + "\"";
    return false;
  }
  if (const auto it = d.find("pauseMs"); it != d.end() && it->is_number()) s.pause = std::chrono::milliseconds(it->get<int64_t>());
  if (const auto it = d.find("refreshHours"); it != d.end() && it->is_number()) s.refresh = std::chrono::milliseconds(static_cast<int64_t>(it->get<double>() * 3600 * 1000));
  if (const auto it = d.find("headers"); it != d.end() && it->is_object())
    for (const auto & [key, v] : it->items())
      if (v.is_string()) s.headers.push_back({key, v.get<std::string>()});
  s.dayKey = textOf(d, "dayKey", s.dayKey);
  if (s.dayKey != "start" && s.dayKey != "end") {
    error = "dayKey is \"start\" or \"end\"";
    return false;
  }
  if (const auto it = d.find("foldWeekends"); it != d.end() && it->is_boolean()) s.foldWeekends = it->get<bool>();
  const auto request = d.find("request");
  if (request == d.end() || !request->is_object() || textOf(*request, "url").empty()) {
    error = "no request with a url";
    return false;
  }
  s.url = textOf(*request, "url");
  s.interval = textOf(*request, "interval", s.interval);
  s.time = textOf(*request, "time", s.time);
  if (s.time != "iso" && s.time != "date") {
    error = "request.time is \"iso\" or \"date\"";
    return false;
  }
  if (const auto it = request->find("years"); it != request->end() && it->is_number_integer()) s.years = it->get<int>();
  if (s.years < 1 || s.years > 50) {
    error = "request.years is 1 to 50";
    return false;
  }
  if (const auto it = request->find("overlapDays"); it != request->end() && it->is_number_integer()) s.overlapDays = it->get<int>();
  if (s.overlapDays < 1 || s.overlapDays > 365) {
    error = "request.overlapDays is 1 to 365";
    return false;
  }
  if (const auto it = d.find("rows"); it != d.end() && it->is_string()) s.hints.rows = it->get<std::string>();
  if (const auto it = d.find("fields"); it != d.end() && it->is_object())
    for (const auto & [role, key] : it->items())
      if (key.is_string()) s.hints.fields[role] = key.get<std::string>();
  const auto items = d.find("items");
  if (items == d.end() || !items->is_array() || items->empty()) {
    error = "no items";
    return false;
  }
  for (const json & v : *items) {
    Item item;
    item.symbol = textOf(v, "symbol");
    item.name = textOf(v, "name");
    item.isin = textOf(v, "isin");
    item.type = textOf(v, "type", item.type);
    item.unit = textOf(v, "unit", item.unit);
    item.market = textOf(v, "market");
    if (!item.market.empty() && (!plain(item.market) || item.market.size() != 4)) {
      error = item.symbol + ": market is the MIC of a market (four letters)";
      return false;
    }
    if (const auto it = v.find("labels"); it != v.end() && it->is_object())
      for (const auto & [key, value] : it->items())
        if (value.is_string()) item.labels[key] = value.get<std::string>();
    if (!s.perItem() && item.labels.empty()) {
      error = item.symbol + ": the answer has every metal, so an item needs labels that tell its series (request.url has no {symbol})";
      return false;
    }
    if (!plain(item.symbol) || item.name.empty() || !isValidIsin(item.isin)) {
      error = "an item needs a symbol, a name and an ISIN that is one (\"" + item.symbol + "\")";
      return false;
    }
    if (const auto it = v.find("currencies"); it != v.end()) {
      if (!it->is_array() || it->empty()) {
        error = item.symbol + ": currencies is a list of currency codes";
        return false;
      }
      item.currencies.clear();
      for (const json & c : *it) {
        if (!c.is_string() || c.get<std::string>().size() != 3) {
          error = item.symbol + ": a currency is a code of three letters";
          return false;
        }
        item.currencies.push_back(c.get<std::string>());
      }
    }
    s.items.push_back(std::move(item));
  }
  out = std::move(s);
  return true;
}

bool MetalSchema::load(const std::string & nameOrPath, MetalSchema & out, std::string & error) {
  std::string text, path;
  if (!readSchemaFile("metals", nameOrPath, text, path, error)) return false;
  if (!parse(text, out, error)) {
    error = path + ": " + error;
    return false;
  }
  return true;
}

MetalSource::MetalSource(MetalSchema schema, std::function<Timestamp()> now) : schema_(std::move(schema)), now_(std::move(now)) {
  if (!now_) now_ = [] { return static_cast<Timestamp>(std::time(nullptr)); };
}

std::string MetalSource::url(const MetalSchema & schema, const MetalSchema::Item & item, const std::string & currency, Timestamp now, Timestamp since) {
  std::string out = schema.url;
  replaceAll(out, "{symbol}", item.symbol);
  replaceAll(out, "{currency}", currency);
  replaceAll(out, "{interval}", schema.interval);
  replaceAll(out, "{from}", timeText(since > 0 ? since : now - static_cast<Timestamp>(schema.years) * 365 * 86400, schema.time));
  replaceAll(out, "{to}", timeText(now, schema.time));
  return out;
}

std::vector<Candle> MetalSource::days(const MetalSchema & schema, const series::Series & s) {
  std::vector<Candle> keyed;
  for (const series::Bar & b : s.bars) {
    // A price a day is the close; a bid and an ask are the mean of them.
    const double close = isDefined(b.close) ? b.close : (isDefined(b.bid) && isDefined(b.ask) ? (b.bid + b.ask) / 2 : isDefined(b.bid) ? b.bid : b.ask);
    if (!isDefined(close)) continue;
    const int64_t key = dayNumberOfInstant(schema.dayKey == "end" && b.end > b.time ? b.end - 1 : b.time);
    keyed.push_back({key, b.open, b.high, b.low, close, b.volume});
  }
  std::stable_sort(keyed.begin(), keyed.end(), [](const Candle & a, const Candle & b) { return a.time < b.time; });
  std::vector<Candle> out;
  for (const Candle & c : keyed) {
    if (!out.empty() && out.back().time == c.time) {
      join(out.back(), c);
    } else if (schema.foldWeekends && weekday(c.time) >= 5) {
      if (!out.empty()) join(out.back(), c);  // a Friday evening's few hours; at the start of the data, nothing before it to join
    } else {
      out.push_back(c);
    }
  }
  return out;
}

Timestamp MetalSource::sinceOf(const Snapshot & snapshot, const MetalSchema::Item & item, const std::string & currency) const {
  const auto base = snapshot.findInstrumentByIsin(item.isin);
  const auto quote = snapshot.findInstrument(currency);
  const auto market = snapshot.findMarket(item.market.empty() ? schema_.market : item.market);
  const auto dataset = snapshot.findDataset(schema_.dataset);
  if (!base || !quote || !market || !dataset) return 0;
  const EdgeKey edge{*base, *quote, *market, *dataset};
  const auto days = snapshot.coverage(edge, Resolution::Day);
  const auto weeks = snapshot.coverage(edge, Resolution::Week);
  // The first load writes a metal whole, so an edge that has weekly bars has had it. Half a year of them says it is not a stray.
  if (!days || !weeks || weeks->count < 26) return 0;
  return (days->last_key - schema_.overlapDays - kUnixEpochDayNumber) * 86400;
}

bool MetalSource::write(Store & store, const MetalSchema::Item & item, const std::string & currency, const series::Series & s, Timestamp now, const std::string & label,
                        SourceListener & listener) {
  if (s.bars.empty()) {
    listener.message(label + ": no bars in the answer");
    return false;
  }
  // The bar length is the answer's, not what was asked for. Where the rows have no end it is the median step between them, and a few rows over a weekend step three days; with a
  // single row, and no end, it is not known (0), which cannot be judged to be wrong.
  const bool ends = s.bars.front().end > 0;
  if (s.barSeconds != 0 && (s.barSeconds < 20 * 3600 || s.barSeconds > (ends ? 28 : 100) * 3600)) {
    listener.message(label + ": asked for daily bars, the answer's are " + std::to_string(s.barSeconds) + " seconds long; not written");
    return false;
  }
  const std::vector<Candle> daily = days(schema_, s);
  if (daily.empty()) {
    listener.message(label + ": no prices in the answer");
    return false;
  }
  const std::vector<Candle> weeks = weeksOfDays(daily);  // the 3 and 10 years read weeks, which the answer has none of
  // One value a day (no high or low anywhere) is a value queue, as a rate is.
  const bool value = std::none_of(daily.begin(), daily.end(), [](const Candle & c) { return isDefined(c.high) || isDefined(c.low) || isDefined(c.open); });
  const QueueKind kind = value ? QueueKind::Scalar : QueueKind::Candle;
  Quote last;
  last.last = daily.back().close;
  last.time = std::min(s.bars.back().end > 0 ? s.bars.back().end : s.bars.back().time, now);  // the bar that is still going on has its price now
  const std::string mic = item.market.empty() ? schema_.market : item.market;
  store.batch([&](Batch & b) {
    const DatasetId dataset = b.addDataset(schema_.dataset, schema_.title.empty() ? schema_.dataset : schema_.title);
    const MarketId market = b.addMarket(mic, marketNameOfMic(mic));
    const InstrumentId base = addInstrumentUnlessSaid(b, item.symbol, item.name, InstrumentKind::Commodity, item.isin, item.type);
    const InstrumentId quote = currencyNode(b, currency);
    if (!base.valid() || !quote.valid() || !dataset.valid() || !market.valid()) return;
    const EdgeKey edge{base, quote, market, dataset};
    b.appendCandles(edge, Resolution::Day, daily, kind);
    b.appendCandles(edge, Resolution::Week, weeks, kind);
    b.setQuote(edge, last);
    b.setListing({edge, item.symbol, "", "", item.type, item.name + ", per " + item.unit});
  });
  listener.changed();
  return true;
}

void MetalSource::load(Store & store, Http & http, SourceListener & listener) {
  const Timestamp now = now_();
  const auto snapshot = store.snapshot();
  const auto ask = [&](const std::string & address, const std::string & label, series::Result & out) {
    const HttpResult r = http.get(address, browserHeaders(address, schema_.headers));
    if (r.status == 429 || r.status == 403) {
      listener.message(label + (r.status == 429 ? ": rate limited, stopping" : ": refused, stopping"));
      return 2;
    }
    if (!r.ok()) {
      listener.message(label + ": " + (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)));
      return 1;
    }
    out = series::read(r.body, schema_.hints);
    if (!out.ok) {
      listener.message(label + ": " + out.error);
      return 1;
    }
    return 0;
  };

  if (schema_.perItem()) {
    bool first = true;
    for (const MetalSchema::Item & item : schema_.items)
      for (const std::string & currency : item.currencies) {
        if (listener.cancelled()) return;
        if (!first && !listener.pause(schema_.pause)) return;
        first = false;
        const std::string label = schema_.name + ": " + item.symbol + "/" + currency;
        series::Result read;
        const int got = ask(url(schema_, item, currency, now, sinceOf(*snapshot, item, currency)), label, read);
        if (got == 2) return;
        if (got == 1) continue;
        if (read.series.size() != 1) {
          listener.message(label + ": the answer has " + std::to_string(read.series.size()) + " series, not the one asked for");
          continue;
        }
        write(store, item, currency, read.series[0], now, label, listener);
      }
    return;
  }

  // One request for every metal and currency: from the earliest day that any of them needs.
  Timestamp since = -1;
  for (const MetalSchema::Item & item : schema_.items)
    for (const std::string & currency : item.currencies) {
      const Timestamp one = sinceOf(*snapshot, item, currency);
      since = one == 0 ? 0 : (since < 0 ? one : std::min(since, one));
      if (since == 0) break;
    }
  series::Result read;
  const std::string address = url(schema_, schema_.items[0], schema_.items[0].currencies[0], now, std::max<Timestamp>(since, 0));
  if (listener.cancelled()) return;
  if (ask(address, schema_.name, read) != 0) return;
  for (const MetalSchema::Item & item : schema_.items)
    for (const std::string & currency : item.currencies) {
      const std::string label = schema_.name + ": " + item.symbol + "/" + currency;
      const series::Series * found = nullptr;
      for (const series::Series & candidate : read.series) {
        bool same = candidate.label(schema_.currencyLabel) == currency;
        for (const auto & [key, value] : item.labels) same = same && candidate.label(key) == value;
        if (same) found = &candidate;
      }
      if (!found) {
        listener.message(label + ": not in the answer");
        continue;
      }
      write(store, item, currency, *found, now, label, listener);
    }
}
