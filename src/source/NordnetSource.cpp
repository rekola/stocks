#include "source/NordnetSource.h"

#include "source/Universe.h"

#include <nlohmann/json.hpp>

#include <thread>

using nlohmann::json;

namespace {

// What the endpoint expects of a browser; no cookie is sent.
const HttpHeaders kHeaders = {{"Accept", "application/json"},
                              {"Accept-Language", "en-US,en;q=0.9,fi;q=0.8"},
                              {"Origin", "https://www.nordnet.fi"},
                              {"Referer", "https://www.nordnet.fi/"},
                              {"Client-Id", "NEXT"}};

std::string text(const json & o, const char * key) {
  auto it = o.find(key);
  return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int64_t integer(const json & o, const char * key) {
  auto it = o.find(key);
  return it != o.end() && it->is_number_integer() ? it->get<int64_t>() : 0;
}

}  // namespace

std::string NordnetSource::marketName(const NordnetListing & l) {
  static const std::pair<const char *, const char *> known[] = {
    {"xeta", "Xetra"}, {"xsto", "Nasdaq Stockholm"}, {"xcse", "Nasdaq Copenhagen"}, {"xhel", "Nasdaq Helsinki"},
    {"xosl", "Oslo Bors"}};
  std::string code = l.slug.substr(l.slug.rfind('-') == std::string::npos ? 0 : l.slug.rfind('-') + 1);
  for (const auto & [c, n] : known)
    if (code == c) return n;
  return "Nordnet market " + std::to_string(l.marketId);
}

bool NordnetSource::parsePage(const std::string & body, Page & page, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("results") || !doc["results"].is_array()) {
    error = doc.is_object() && doc.contains("message") && doc["message"].is_string() ? doc["message"].get<std::string>()
                                                                                       : "not an instrument list";
    return false;
  }
  page.total = static_cast<int>(integer(doc, "total_hits"));
  page.rows = static_cast<int>(doc["results"].size());
  for (const json & row : doc["results"]) {
    NordnetListing l;
    if (row.is_object() && row.contains("instrument_info") && row["instrument_info"].is_object()) {
      const json & i = row["instrument_info"];
      l.instrumentId = integer(i, "instrument_id");
      l.symbol = text(i, "symbol");
      l.name = text(i, "long_name").empty() ? text(i, "name") : text(i, "long_name");
      l.isin = text(i, "isin");
      l.currency = text(i, "currency");
      l.type = text(i, "instrument_type");
      l.issuer = text(i, "issuer_name");
      l.clearingPlace = text(i, "clearing_place");
    }
    if (row.is_object() && row.contains("market_info") && row["market_info"].is_object()) {
      const json & m = row["market_info"];
      l.marketId = static_cast<int>(integer(m, "market_id"));
      l.marketSubId = static_cast<int>(integer(m, "market_sub_id"));
      l.identifier = text(m, "identifier");
    }
    if (row.is_object() && row.contains("nnx_info") && row["nnx_info"].is_object())
      l.slug = text(row["nnx_info"], "display_slug");
    if (row.is_object() && row.contains("exchange_info") && row["exchange_info"].is_object())
      l.country = text(row["exchange_info"], "exchange_country");
    if (l.instrumentId == 0 || l.symbol.empty() || l.isin.empty() || l.currency.empty() || l.marketId == 0) {
      page.skipped++;
      continue;
    }
    page.listings.push_back(std::move(l));
  }
  return true;
}

void NordnetSource::load(Store & store, Http & http, SourceListener & listener) {
  listings_.clear();
  const std::string base =
      "https://www.nordnet.fi/api/2/instrument_search/query/etflist?sort_order=asc&sort_attribute=symbol&limit=" +
      std::to_string(pageSize_);
  int skipped = 0;
  size_t offset = 0;
  for (int request = 0; request < 1000; request++) {  // a bound, in case total_hits is wrong
    if (listener.cancelled()) return;
    HttpResult r = http.get(offset ? base + "&offset=" + std::to_string(offset) : base, kHeaders);
    if (!r.ok()) {
      listener.message("nordnet: " +
                       (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)) +
                       (r.status == 429 || r.status == 403 ? ", stopping" : ""));
      break;
    }
    Page page;
    std::string error;
    if (!parsePage(r.body, page, error)) {
      listener.message("nordnet: " + error);
      break;
    }
    skipped += page.skipped;

    Dataset dataset = store.addDataset({{}, "nordnet-1d"});
    for (const NordnetListing & l : page.listings) {
      Market market = store.addMarket({{}, marketName(l)});
      Instrument base = store.addInstrument({{}, l.symbol, l.name, InstrumentKind::Etf, l.isin});
      const char * label = currencyName(l.currency);
      Instrument quote = store.addInstrument({{}, l.currency, *label ? label : l.currency, InstrumentKind::Currency});
      store.addEdge({base.id, quote.id, market.id, dataset.id});
    }
    listings_.insert(listings_.end(), page.listings.begin(), page.listings.end());
    if (!page.listings.empty()) listener.changed();

    offset += static_cast<size_t>(page.rows);
    if (page.rows == 0 || offset >= static_cast<size_t>(page.total)) break;
    if (pause_.count() > 0) std::this_thread::sleep_for(pause_);
  }
  if (!listings_.empty())
    listener.message("nordnet: " + std::to_string(listings_.size()) + " ETFs" +
                     (skipped ? ", " + std::to_string(skipped) + " rows left out" : std::string()));
}
