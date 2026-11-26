#include "source/FundRows.h"

#include "model/Dates.h"
#include "model/Isin.h"
#include "source/InstrumentLists.h"
#include "source/KnownMarkets.h"

#include <cellui/NumberParse.h>

#include <algorithm>
#include <cstdio>
#include <cmath>

using nlohmann::json;

std::optional<double> looseNumber(const json & v) {
  if (v.is_number()) {
    const double d = v.get<double>();
    return std::isfinite(d) ? std::optional<double>(d) : std::nullopt;
  }
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    double d = 0;
    auto [end, ec] = cellui::parseFloat(s.data(), s.data() + s.size(), d);
    if (ec == std::errc() && end == s.data() + s.size() && std::isfinite(d)) return d;
  }
  return std::nullopt;
}

std::optional<int64_t> looseInteger(const json & v) {
  auto d = looseNumber(v);
  return d && *d == std::floor(*d) ? std::optional<int64_t>(static_cast<int64_t>(*d)) : std::nullopt;
}

std::optional<double> looseNumber(const json & o, const char * key) {
  auto it = o.find(key);
  return it == o.end() ? std::nullopt : looseNumber(*it);
}

std::optional<int64_t> looseInteger(const json & o, const char * key) {
  auto it = o.find(key);
  return it == o.end() ? std::nullopt : looseInteger(*it);
}

std::optional<int64_t> looseDay(const json & v) {
  if (v.is_string()) {
    const std::string s = v.get<std::string>();
    int y = 0;
    unsigned m = 0, d = 0;
    if (s.size() >= 10 && s[4] == '-' && s[7] == '-' && std::sscanf(s.c_str(), "%d-%u-%u", &y, &m, &d) == 3 && m >= 1 && m <= 12 && d >= 1 && d <= 31)
      return dayNumber(y, m, d);
    // The Finnish way: 25.11.2026 (a day or month may be one digit).
    int used = 0;
    if (std::sscanf(s.c_str(), "%u.%u.%d%n", &d, &m, &y, &used) == 3 && static_cast<size_t>(used) == s.size() && y >= 1000 && y <= 9999 && m >= 1 &&
        m <= 12 && d >= 1 && d <= 31)
      return dayNumber(y, m, d);
  }
  auto n = looseNumber(v);
  if (!n || *n <= 0) return std::nullopt;
  const int64_t seconds = *n > 1e11 ? static_cast<int64_t>(*n / 1000) : static_cast<int64_t>(*n);  // milliseconds are the larger numbers
  return dayNumberOfInstant(seconds, 3 * 3600);
}

int64_t navDay(int64_t ms) { return dayNumberOfInstant(ms / 1000, 3 * 3600); }

namespace {

// The points of `doc`: itself if it is the list of pairs, else the "data" of the first series that has one.
const json * pointsOf(const json & doc) {
  if (doc.is_array() && !doc.empty() && doc[0].is_array()) return &doc;
  if (doc.is_array()) {
    for (const json & series : doc)
      if (series.is_object())
        if (auto d = series.find("data"); d != series.end() && d->is_array()) return &*d;
  }
  if (doc.is_object())
    if (auto d = doc.find("data"); d != doc.end() && d->is_array()) return &*d;
  return nullptr;
}

}  // namespace

bool parseNavSeries(const std::string & body, std::vector<Candle> & bars, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    // A series wrapped in a script's text: what is between `"data":` and the end of its array.
    const size_t key = body.find("\"data\"");
    const size_t open = key == std::string::npos ? key : body.find('[', key);
    if (open != std::string::npos) {
      int depth = 0;
      for (size_t i = open; i < body.size(); i++) {
        if (body[i] == '[') depth++;
        else if (body[i] == ']' && --depth == 0) {
          doc = json::parse(body.substr(open, i - open + 1), nullptr, /*allow_exceptions=*/false);
          break;
        }
      }
    }
  }
  const json * points = doc.is_discarded() ? nullptr : pointsOf(doc);
  if (!points) {
    error = "not a price history";
    return false;
  }
  bars.clear();
  for (const json & p : *points) {
    if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) continue;
    const double value = p[1].get<double>();
    if (!std::isfinite(value)) continue;
    bars.push_back({navDay(static_cast<int64_t>(p[0].get<double>())), kUndefined, kUndefined, kUndefined, value, kUndefined});
  }
  std::stable_sort(bars.begin(), bars.end(), [](const Candle & a, const Candle & b) { return a.time < b.time; });
  std::vector<Candle> unique;  // one bar to a day: the later in the answer stands
  for (const Candle & c : bars) {
    if (!unique.empty() && unique.back().time == c.time) unique.back() = c;
    else unique.push_back(c);
  }
  bars = std::move(unique);
  if (bars.empty()) {
    error = "no prices";
    return false;
  }
  return true;
}

bool tooSparse(const std::vector<Candle> & bars) {
  constexpr size_t kLook = 30;      // the latest gaps looked at: a fund's pricing can change over its life
  constexpr int64_t kMaxDays = 10;  // a weekly fund is kept, a monthly one is not
  if (bars.size() < 2) return false;
  std::vector<int64_t> gaps;
  for (size_t i = bars.size() > kLook ? bars.size() - kLook : 1; i < bars.size(); i++) gaps.push_back(bars[i].time - bars[i - 1].time);
  std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
  return gaps[gaps.size() / 2] > kMaxDays;
}

std::vector<Attribute> attributesOf(const FundRow & f) {
  std::vector<Attribute> said;
  if (!f.url.empty()) said.push_back({predicate::webUrl, f.url});
  if (f.managementFee) said.push_back({"management-fee", *f.managementFee});
  if (f.subscriptionFee) said.push_back({"subscription-fee", *f.subscriptionFee});
  if (f.redemptionFee) said.push_back({"redemption-fee", *f.redemptionFee});
  if (f.risk) said.push_back({"risk-class", *f.risk});
  if (f.rating) said.push_back({"morningstar-rating", *f.rating});
  if (f.esg) said.push_back({"esg-rating", *f.esg});
  said.insert(said.end(), f.extra.begin(), f.extra.end());
  return said;
}

std::optional<EdgeKey> writeFund(Batch &store, const std::string &datasetName,
                                 const FundRow &f,
                                 const std::vector<Candle> &bars) {
  if (bars.empty() || !isValidIsin(f.isin)) return std::nullopt;
  const DatasetId dataset = store.addDataset(datasetName);
  const MarketId market = store.addMarket(kOffExchange, marketNameOfMic(kOffExchange));
  const InstrumentId base = addInstrumentUnlessSaid(store, "", f.name, InstrumentKind::Fund, f.isin, "FUND");
  const InstrumentId quote = currencyNode(store, "EUR");
  // A part that cannot be named (the ISIN's check digit was right, but the store refused something else): nothing is written.
  if (!market.valid() || !base.valid() || !quote.valid() || !dataset.valid()) return std::nullopt;
  store.setAttributes(base, attributesOf(f));
  const EdgeKey edge{base, quote, market, dataset};
  store.appendCandles(edge, Resolution::Day, bars);
  store.setListing({edge, f.symbol.empty() ? f.isin : f.symbol, f.identifier, "", "FUND", f.name});
  return edge;
}
