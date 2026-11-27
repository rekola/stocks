#include "source/GoldApiSource.h"

#include "model/Dates.h"
#include "source/InstrumentLists.h"
#include "source/KnownIsins.h"
#include "source/KnownMarkets.h"
#include "source/MetalUnits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

using json = nlohmann::json;

namespace {

constexpr const char * kBase = "https://api.gold-api.com";

std::string text(const json & row, const char * key) {
  auto it = row.find(key);
  return it != row.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// What a symbol is: a metal is a commodity, a coin is of no kind, and each has the ISIN source/KnownIsins.h gives it (the real one of the
// commodity, the one every other source and list gives it, so that gold here is the gold Nordnet's list has and not another instrument; a coin
// has one of ours). The API says none of it. A symbol that is not here has none, and the store refuses it and says so.
struct Known {
  const char * symbol;
  InstrumentKind kind;
  const char * type;
  const char * isin;
  const char * unit;  // what the instrument's price is for: it is tied to the ISIN (Nordnet's copper, which has the same ISIN, is per metric tonne)
  double factor;      // what the API's price is multiplied by to be for that unit: the answer does not say what its price is for, and copper is per pound
};

constexpr Known kKnown[] = {{"XAU", InstrumentKind::Commodity, "METAL", known_isin::kGold, "troy oz", 1},
                            {"XAG", InstrumentKind::Commodity, "METAL", known_isin::kSilver, "troy oz", 1},
                            {"XPT", InstrumentKind::Commodity, "METAL", known_isin::kPlatinum, "troy oz", 1},
                            {"XPD", InstrumentKind::Commodity, "METAL", known_isin::kPalladium, "troy oz", 1},
                            {"HG", InstrumentKind::Commodity, "METAL", known_isin::kCopper, "tonne", kPoundsPerTonne},  // the API quotes it per pound
                            {"BTC", InstrumentKind::Other, "CRYPTO", known_isin::kBitcoin, "coin", 1},
                            {"ETH", InstrumentKind::Other, "CRYPTO", known_isin::kEthereum, "coin", 1}};

const Known * knownSymbol(const std::string & symbol) {
  for (const Known & k : kKnown)
    if (symbol == k.symbol) return &k;
  return nullptr;
}

}  // namespace

bool GoldApiSource::parseSymbols(const std::string & body, std::vector<Symbol> & out, std::string & error) {
  out.clear();
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_array()) {
    error = "not a list of symbols";
    return false;
  }
  for (const json & row : doc) {
    if (!row.is_object()) continue;
    Symbol s{text(row, "symbol"), text(row, "name")};
    if (s.symbol.empty()) continue;
    if (s.name.empty()) s.name = s.symbol;
    out.push_back(std::move(s));
  }
  if (out.empty()) {
    error = "no symbols in the list";
    return false;
  }
  return true;
}

bool GoldApiSource::parsePrice(const std::string & body, Price & out, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object()) {
    error = "not a price";
    return false;
  }
  out = {};
  out.symbol = text(doc, "symbol");
  out.name = text(doc, "name");
  out.currency = text(doc, "currency");
  auto p = doc.find("price");
  if (p == doc.end() || !p->is_number()) {
    error = "no price";
    return false;
  }
  out.price = p->get<double>();
  if (!std::isfinite(out.price) || out.price <= 0) {
    error = "the price is not above zero";
    return false;
  }
  if (out.symbol.empty() || out.currency.empty()) {
    error = "no symbol or currency";
    return false;
  }
  // "2026-11-24T14:19:44Z": the instant of the price, which is the time of its quote.
  const auto time = instantOfIsoTimestamp(text(doc, "updatedAt"));
  if (!time) {
    error = "no update time";
    return false;
  }
  out.time = *time;
  return true;
}

void GoldApiSource::load(Store & store, Http & http, SourceListener & listener) {
  if (listener.cancelled()) return;
  HttpResult r = http.get(std::string(kBase) + "/symbols", {});
  std::string error;
  std::vector<Symbol> symbols;
  if (!r.ok() || !parseSymbols(r.body, symbols, error)) {
    listener.message("goldapi: symbols: " + (!r.ok() ? (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)) : error) +
                     (known_.empty() ? "" : "; using the list from before"));
    if (known_.empty()) return;
    symbols = known_;
  } else {
    known_ = symbols;
  }
  // Gold first: the estimate of physical gold waits for it.
  std::stable_sort(symbols.begin(), symbols.end(), [](const Symbol & a, const Symbol & b) { return (a.symbol == "XAU") > (b.symbol == "XAU"); });
  bool first = true;
  for (const Symbol & s : symbols) {
    std::vector<Price> prices;
    for (const std::string & currency : currencies_) {
      if (listener.cancelled()) return;
      if (!first && pause_.count() > 0 && !listener.pause(pause_)) return;
      first = false;
      HttpResult answer = http.get(std::string(kBase) + "/price/" + s.symbol + "/" + currency, {});
      if (!answer.ok()) {
        const bool refused = answer.status == 429 || answer.status == 403;
        listener.message("goldapi: " + s.symbol + "/" + currency + ": " +
                         (answer.status == 0 ? (answer.error.empty() ? std::string("no answer") : answer.error) : "HTTP " + std::to_string(answer.status)) +
                         (refused ? ", stopping" : ""));
        if (refused) return;
        continue;
      }
      Price price;
      if (!parsePrice(answer.body, price, error)) {
        listener.message("goldapi: " + s.symbol + "/" + currency + ": " + error);
        continue;
      }
      // What was asked for, not another symbol or currency in its place.
      if (price.symbol != s.symbol || price.currency != currency) {
        listener.message("goldapi: " + s.symbol + "/" + currency + ": the answer is for " + price.symbol + "/" + price.currency);
        continue;
      }
      prices.push_back(std::move(price));
    }
    if (prices.empty()) continue;
    // One symbol, both currencies: one publication.
    store.batch([&](Batch & b) {
      const DatasetId dataset = b.addDataset(kGoldApiDataset, "goldapi");
      const MarketId market = b.addMarket(kNoMarket, marketNameOfMic(kNoMarket));
      const Known * known = knownSymbol(s.symbol);
      const InstrumentKind kind = known ? known->kind : InstrumentKind::Other;
      InstrumentId base;
      if (known && *known->isin) {
        // The instrument another source made by this ISIN: its name and type stand where they are said, and ours are said only where none is.
        base = addInstrumentUnlessSaid(b, s.symbol, s.name, kind, known->isin, known->type);
      } else {
        std::vector<Attribute> rest = {{predicate::name, s.name}, {predicate::symbol, s.symbol}, {predicate::type, known ? known->type : "SPOT"}};
        if (kind != InstrumentKind::Other) rest.push_back({predicate::kind, kindName(kind)});
        base = b.resolveInstrument({}, rest);
      }
      if (!base.valid()) return;
      for (const Price & p : prices) {
        const InstrumentId quote = currencyNode(b, p.currency);
        if (!quote.valid()) continue;
        const EdgeKey edge{base, quote, market, dataset};
        // The quote only, in the unit of the instrument, so that this source's copper and another's, of one ISIN, are the same number. A bar is
        // history, which this API has none of: a live price written as a bar lands wherever the program happened to be open.
        Quote quote;
        quote.last = p.price * (known ? known->factor : 1.0);
        quote.time = p.time;
        b.setQuote(edge, quote);
        // The unit is in the name the row shows: a price of copper is for a tonne and one of gold for a troy ounce.
        b.setListing({edge, s.symbol, "", "", known ? known->type : "SPOT", known ? s.name + ", per " + known->unit : s.name});
      }
    });
    listener.changed();
  }
}
