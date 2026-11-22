#include "source/FundRows.h"

#include "model/Dates.h"
#include "source/InstrumentLists.h"

#include <algorithm>
#include <charconv>
#include <cmath>

using nlohmann::json;

std::optional<double> looseNumber(const json & o, const char * key) {
  auto it = o.find(key);
  if (it == o.end()) return std::nullopt;
  if (it->is_number()) {
    const double v = it->get<double>();
    return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
  }
  if (it->is_string()) {
    const std::string s = it->get<std::string>();
    double v = 0;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec == std::errc() && end == s.data() + s.size() && std::isfinite(v)) return v;
  }
  return std::nullopt;
}

std::optional<int64_t> looseInteger(const json & o, const char * key) {
  auto v = looseNumber(o, key);
  return v && *v == std::floor(*v) ? std::optional<int64_t>(static_cast<int64_t>(*v)) : std::nullopt;
}

int64_t navDay(int64_t ms) { return dayNumberOfInstant(ms / 1000, 12 * 3600); }

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

bool writeFund(Store & store, const std::string & datasetName, const std::string & marketName, const FundRow & f,
               const std::vector<Candle> & bars) {
  if (bars.empty() || f.isin.empty()) return false;
  Dataset dataset = store.addDataset({{}, datasetName});
  Market market = store.addMarket({{}, marketName});
  Instrument base = store.addInstrument({{}, "", f.name, InstrumentKind::Fund, f.isin, "FUND"});
  Instrument quote = currencyNode(store, "EUR");
  std::vector<Attribute> said;
  if (f.managementFee) said.push_back({"management-fee", *f.managementFee});
  if (f.subscriptionFee) said.push_back({"subscription-fee", *f.subscriptionFee});
  if (f.redemptionFee) said.push_back({"redemption-fee", *f.redemptionFee});
  if (f.risk) said.push_back({"fund-risk-class", *f.risk});
  if (f.rating) said.push_back({"fund-morningstar-rating", *f.rating});
  if (f.esg) said.push_back({"fund-esg-rating", *f.esg});
  store.setAttributes(base.id, said);
  const EdgeKey edge{base.id, quote.id, market.id, dataset.id};
  store.appendCandles(edge, Resolution::Day, bars);
  store.setListing({edge, f.symbol.empty() ? f.isin : f.symbol, f.identifier, "", "FUND", f.name});
  return true;
}
