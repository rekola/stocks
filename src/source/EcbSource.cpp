#include "source/EcbSource.h"

#include "model/Dates.h"
#include "source/InstrumentLists.h"

#include <cellui/NumberParse.h>

#include <chrono>
#include <cstdio>
#include <map>

namespace {

// One line of comma-separated fields, a field in double quotes where it holds a comma.
std::vector<std::string> fieldsOf(const std::string & line) {
  std::vector<std::string> out;
  std::string field;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); i++) {
    const char c = line[i];
    if (quoted) {
      if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') field += '"', i++;
      else if (c == '"') quoted = false;
      else field += c;
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      out.push_back(std::move(field));
      field.clear();
    } else if (c != '\r') {
      field += c;
    }
  }
  out.push_back(std::move(field));
  return out;
}

// "2026-11-02" as a day number, 0 if it is not a date.
int64_t dayOf(const std::string & s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return 0;
  auto number = [&](size_t from, size_t n) {
    int v = 0;
    for (size_t i = from; i < from + n; i++) {
      if (s[i] < '0' || s[i] > '9') return -1;
      v = v * 10 + (s[i] - '0');
    }
    return v;
  };
  const int y = number(0, 4), m = number(5, 2), d = number(8, 2);
  if (y < 0 || m < 1 || m > 12 || d < 1 || d > 31) return 0;
  return dayNumber(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

}  // namespace

std::string EcbSource::url(int days, int64_t now) {
  const Date start = days > 0 ? civilFromUnixDays(now / 86400 - days) : Date{1999, 1, 1};
  char date[16];
  std::snprintf(date, sizeof date, "%04d-%02u-%02u", start.year, start.month, start.day);
  return std::string("https://data-api.ecb.europa.eu/service/data/EXR/D..EUR.SP00.A?format=csvdata&detail=dataonly&startPeriod=") + date;
}

bool EcbSource::parse(const std::string & csv, std::vector<EcbRate> & rates, std::string & error) {
  size_t pos = 0;
  auto nextLine = [&](std::string & line) {
    if (pos >= csv.size()) return false;
    const size_t end = csv.find('\n', pos);
    line = csv.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    pos = end == std::string::npos ? csv.size() : end + 1;
    return true;
  };
  std::string line;
  if (!nextLine(line)) {
    error = "not an ECB answer";
    return false;
  }
  const std::vector<std::string> head = fieldsOf(line);
  int currency = -1, denominator = -1, period = -1, value = -1;
  for (size_t i = 0; i < head.size(); i++) {
    if (head[i] == "CURRENCY") currency = static_cast<int>(i);
    else if (head[i] == "CURRENCY_DENOM") denominator = static_cast<int>(i);
    else if (head[i] == "TIME_PERIOD") period = static_cast<int>(i);
    else if (head[i] == "OBS_VALUE") value = static_cast<int>(i);
  }
  if (currency < 0 || denominator < 0 || period < 0 || value < 0) {
    error = "not an ECB answer";
    return false;
  }
  const size_t widest = static_cast<size_t>(std::max({currency, denominator, period, value}));
  while (nextLine(line)) {
    if (line.empty()) continue;
    const std::vector<std::string> f = fieldsOf(line);
    if (f.size() <= widest || f[static_cast<size_t>(denominator)] != "EUR") continue;  // the euro is the base of all of them
    const int64_t day = dayOf(f[static_cast<size_t>(period)]);
    // Never through the locale: cellui's parseFloat.
    const std::string & text = f[static_cast<size_t>(value)];
    double rate = 0;
    if (day == 0 || text.empty() || cellui::parseFloat(text.data(), text.data() + text.size(), rate).ec != std::errc() || !(rate > 0)) continue;
    rates.push_back({f[static_cast<size_t>(currency)], day, rate});
  }
  return true;
}

void EcbSource::load(Store & store, Http & http, SourceListener & listener) {
  const int64_t now = options_.now ? options_.now()
                                   : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  HttpResult r = http.get(url(options_.days, now), {{"Accept", "text/csv"}});
  if (!r.ok()) {
    listener.message("ecb: " + (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)));
    return;
  }
  std::vector<EcbRate> rates;
  std::string error;
  if (!parse(r.body, rates, error)) {
    listener.message("ecb: " + error);
    return;
  }
  std::map<std::string, std::vector<Candle>> by;  // by currency, in the order of the answer: oldest first
  for (const EcbRate & rate : rates) by[rate.currency].push_back({rate.day, kUndefined, kUndefined, kUndefined, rate.rate, kUndefined});

  // The dataset says whose it is: the ECB's terms ask that it be cited as the source.
  Dataset dataset = store.addDataset({{}, "ecb"});
  store.setAttributes(dataset.id, {{"description", "Euro foreign exchange reference rates"},
                                   {"source", "European Central Bank"},
                                   {"url", "https://data.ecb.europa.eu"},
                                   {"attribution", "Source: ECB. Published for information, not for transactions."}});
  Market market = store.addMarket({{}, "Currencies"});
  Instrument euro = currencyNode(store, "EUR");
  for (const auto & [code, bars] : by) {
    Instrument quote = currencyNode(store, code);
    const EdgeKey edge{euro.id, quote.id, market.id, dataset.id};
    store.appendCandles(edge, Resolution::Day, bars);
    store.setListing({edge, "EUR" + code, "", "", "ECB reference rate", "EUR/" + code});
  }
  if (!by.empty()) listener.changed();
}
