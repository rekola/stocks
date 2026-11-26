#include "source/GoldPriceSource.h"

#include "model/Dates.h"
#include "source/GoldPriceHistory.h"
#include "source/InstrumentLists.h"
#include "source/KnownIsins.h"
#include "source/KnownMarkets.h"
#include "source/MetalUnits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>

using json = nlohmann::json;

namespace {

constexpr const char * kPrices = "https://api.goldprice.dev/v1/prices";

std::string text(const json & row, const char * key) {
  auto it = row.find(key);
  return it != row.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// A price is a string ("3551.34"), though a number is read too.
std::optional<double> amount(const json & row, const char * key) {
  auto it = row.find(key);
  if (it == row.end()) return std::nullopt;
  if (it->is_number()) return it->get<double>();
  if (!it->is_string()) return std::nullopt;
  const std::string s = it->get<std::string>();
  double v = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size()) return std::nullopt;
  return v;
}

// What a symbol is, and what its price is multiplied by to be for the unit of the instrument for each unit the API says (the unit goes with the
// ISIN: Nordnet's copper is per tonne, the API's per pound).
struct Known {
  const char * symbol;
  const char * name;
  const char * isin;
  const char * unit;  // the instrument's
  struct Conversion {
    const char * api_unit;
    double factor;
  } conversions[2];
};

constexpr Known kKnown[] = {{"XAU", "Gold", known_isin::kGold, "troy oz", {{"troy_ounce", 1}, {nullptr, 0}}},
                            {"XAG", "Silver", known_isin::kSilver, "troy oz", {{"troy_ounce", 1}, {nullptr, 0}}},
                            {"HG", "Copper", known_isin::kCopper, "tonne", {{"pound", kPoundsPerTonne}, {nullptr, 0}}}};

const Known * knownSymbol(const std::string & symbol) {
  for (const Known & k : kKnown)
    if (symbol == k.symbol) return &k;
  return nullptr;
}

}  // namespace

GoldPriceSource::GoldPriceSource(std::chrono::milliseconds refresh, std::vector<std::string> currencies)
    : refresh_(refresh), currencies_(std::move(currencies)), history_(std::make_shared<GoldPriceHistory>()) {}

bool GoldPriceSource::parsePrices(const std::string & body, std::vector<Price> & out, std::string & error) {
  out.clear();
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("symbols") || !doc["symbols"].is_array()) {
    error = "not a list of prices";
    return false;
  }
  for (const json & row : doc["symbols"]) {
    if (!row.is_object()) continue;
    Price p;
    p.symbol = text(row, "symbol");
    p.currency = text(row, "quote_currency");
    p.unit = text(row, "unit");
    const auto price = amount(row, "price");
    // "2026-11-25T14:22:01.418143+00:00": the date is the first ten characters, which is UTC for the offset +00:00 the API writes.
    const auto day = dayNumberOfIsoDate(text(row, "computed_at").substr(0, 10));
    if (p.symbol.empty() || p.currency.empty() || p.unit.empty() || !price || !std::isfinite(*price) || *price <= 0 || !day) continue;
    p.price = *price;
    p.day = *day;
    if (auto stale = row.find("is_stale"); stale != row.end() && stale->is_boolean()) p.stale = stale->get<bool>();
    // Spot only: the API names the contract (a future would be another price of the metal).
    if (const std::string type = text(row, "contract_type"); !type.empty() && type != "spot") continue;
    out.push_back(std::move(p));
  }
  if (out.empty()) {
    error = "no prices in the answer";
    return false;
  }
  return true;
}

void GoldPriceSource::load(Store & store, Http & http, SourceListener & listener) {
  if (listener.cancelled()) return;
  const HttpResult r = http.get(kPrices, {});
  std::vector<Price> prices;
  std::string error;
  if (!r.ok() || !parsePrices(r.body, prices, error)) {
    listener.message("goldprice: " + (!r.ok() ? (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)) : error));
    return;
  }
  // Gold first: the estimate of physical gold waits for it.
  std::stable_sort(prices.begin(), prices.end(), [](const Price & a, const Price & b) { return (a.symbol == "XAU") > (b.symbol == "XAU"); });
  std::vector<std::string> symbols;
  for (const Price & p : prices)
    if (std::find(symbols.begin(), symbols.end(), p.symbol) == symbols.end()) symbols.push_back(p.symbol);

  int stale = 0;
  for (const std::string & symbol : symbols) {
    const Known * known = knownSymbol(symbol);
    if (!known) continue;  // a symbol we have no ISIN for: the store would refuse it
    struct Row {
      std::string currency;
      double value;
      int64_t day;
    };
    std::vector<Row> rows;
    for (const Price & p : prices) {
      if (p.symbol != symbol) continue;
      if (!currencies_.empty() && std::find(currencies_.begin(), currencies_.end(), p.currency) == currencies_.end()) continue;
      if (p.stale) {
        stale++;
        continue;
      }
      double factor = 0;
      for (const auto & c : known->conversions)
        if (c.api_unit && p.unit == c.api_unit) factor = c.factor;
      if (factor <= 0) {
        listener.message("goldprice: " + symbol + "/" + p.currency + ": a price per " + p.unit + " is not one this adapter knows");
        continue;
      }
      rows.push_back({p.currency, p.price * factor, p.day});
    }
    if (rows.empty()) continue;
    store.batch([&](Batch & b) {
      const DatasetId dataset = b.addDataset(kGoldPriceDataset, "goldprice.dev");
      const MarketId market = b.addMarket(kNoMarket, marketNameOfMic(kNoMarket));
      const InstrumentId base = addInstrumentUnlessSaid(b, symbol, known->name, InstrumentKind::Commodity, known->isin, "METAL");
      if (!base.valid()) return;
      for (const Row & row : rows) {
        const InstrumentId quote = currencyNode(b, row.currency);
        if (!quote.valid()) continue;
        const EdgeKey edge{base, quote, market, dataset};
        b.appendCandles(edge, Resolution::Day, {{row.day, kUndefined, kUndefined, kUndefined, row.value, kUndefined}});
        b.setListing({edge, symbol, "", "", "METAL", std::string(known->name) + ", per " + known->unit});
      }
    });
    listener.changed();
  }
  if (stale > 0) listener.message("goldprice: " + std::to_string(stale) + " stale price" + (stale == 1 ? "" : "s") + " not stored");
}
