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
  if (s.url.find("{symbol}") == std::string::npos) {
    error = "request.url has no {symbol}: an answer that has every metal is not read yet";
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

std::string MetalSource::url(const MetalSchema & schema, const MetalSchema::Item & item, const std::string & currency, Timestamp now) {
  std::string out = schema.url;
  replaceAll(out, "{symbol}", item.symbol);
  replaceAll(out, "{currency}", currency);
  replaceAll(out, "{interval}", schema.interval);
  replaceAll(out, "{from}", timeText(now - static_cast<Timestamp>(schema.years) * 365 * 86400, schema.time));
  replaceAll(out, "{to}", timeText(now, schema.time));
  return out;
}

std::vector<Candle> MetalSource::days(const MetalSchema & schema, const series::Series & s) {
  std::vector<Candle> keyed;
  for (const series::Bar & b : s.bars) {
    const int64_t key = dayNumberOfInstant(schema.dayKey == "end" && b.end > b.time ? b.end - 1 : b.time);
    keyed.push_back({key, b.open, b.high, b.low, b.close, b.volume});
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

void MetalSource::load(Store & store, Http & http, SourceListener & listener) {
  const Timestamp now = now_();
  bool first = true;
  for (const MetalSchema::Item & item : schema_.items)
    for (const std::string & currency : item.currencies) {
      if (listener.cancelled()) return;
      if (!first && !listener.pause(schema_.pause)) return;
      first = false;
      const std::string label = schema_.name + ": " + item.symbol + "/" + currency;
      const std::string address = url(schema_, item, currency, now);
      const HttpResult r = http.get(address, browserHeaders(address, schema_.headers));
      if (r.status == 429 || r.status == 403) {
        listener.message(label + (r.status == 429 ? ": rate limited, stopping" : ": refused, stopping"));
        return;
      }
      if (!r.ok()) {
        listener.message(label + ": " + (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)));
        continue;
      }
      const series::Result read = series::read(r.body, schema_.hints);
      if (!read.ok) {
        listener.message(label + ": " + read.error);
        continue;
      }
      if (read.series.size() != 1) {
        listener.message(label + ": the answer has " + std::to_string(read.series.size()) + " series, not the one asked for");
        continue;
      }
      const series::Series & s = read.series[0];
      if (s.bars.empty()) {
        listener.message(label + ": no bars in the answer");
        continue;
      }
      if (s.barSeconds < 20 * 3600 || s.barSeconds > 28 * 3600) {
        listener.message(label + ": asked for daily bars, the answer's are " + std::to_string(s.barSeconds) + " seconds long; not written");
        continue;
      }
      const std::vector<Candle> daily = days(schema_, s);
      if (daily.empty()) continue;
      const std::vector<Candle> weeks = weeksOfDays(daily);  // the 3 and 10 years read weeks, which the answer has none of
      Quote last;
      last.last = s.bars.back().close;
      last.time = std::min(s.bars.back().end > 0 ? s.bars.back().end : s.bars.back().time, now);  // the bar that is still going on has its price now
      store.batch([&](Batch & b) {
        const DatasetId dataset = b.addDataset(schema_.dataset, schema_.title.empty() ? schema_.dataset : schema_.title);
        const MarketId market = b.addMarket(schema_.market, marketNameOfMic(schema_.market));
        const InstrumentId base = addInstrumentUnlessSaid(b, item.symbol, item.name, InstrumentKind::Commodity, item.isin, item.type);
        const InstrumentId quote = currencyNode(b, currency);
        if (!base.valid() || !quote.valid() || !dataset.valid() || !market.valid()) return;
        const EdgeKey edge{base, quote, market, dataset};
        b.appendCandles(edge, Resolution::Day, daily);
        b.appendCandles(edge, Resolution::Week, weeks);
        if (isDefined(last.last)) b.setQuote(edge, last);
        b.setListing({edge, item.symbol, "", "", item.type, item.name + ", per " + item.unit});
      });
      listener.changed();
    }
}
