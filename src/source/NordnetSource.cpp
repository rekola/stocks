#include "source/NordnetSource.h"

#include "source/InstrumentLists.h"
#include "source/InstrumentTypes.h"

#include <nlohmann/json.hpp>

#include <optional>
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

// A price in the price block: {"price": 65.03, "decimals": 2}.
std::optional<double> price(const json & block, const char * key) {
  auto it = block.find(key);
  if (it == block.end() || !it->is_object()) return std::nullopt;
  auto p = it->find("price");
  return p != it->end() && p->is_number() ? std::optional<double>(p->get<double>()) : std::nullopt;
}



}  // namespace

std::vector<NordnetSource::List> NordnetSource::defaultLists() {
  const std::string base = "https://www.nordnet.fi/api/2/instrument_search/query/";
  const std::string sorted = "sort_order=asc&sort_attribute=symbol";  // a key that does not move between pages
  return {{"etflist", base + "etflist?" + sorted, 500},
          {"certificatelist", base + "certificatelist?apply_filters=instrument_type%3DPTC&" + sorted, 1000}};
}

std::string NordnetSource::marketName(const NordnetListing & l) {
  static const std::pair<int, const char *> known[] = {{4, "Xetra"},
                                                       {11, "Nasdaq Stockholm"},
                                                       {14, "Nasdaq Copenhagen"},
                                                       {24, "Nasdaq Helsinki"},
                                                       {53, "First North Sweden"},
                                                       {54, "First North Finland"},
                                                       {55, "First North Denmark"}};
  for (const auto & [id, name] : known)
    if (l.marketId == id) return name;
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
    if (row.is_object() && row.contains("nnx_info") && row["nnx_info"].is_object()) {
      l.slug = text(row["nnx_info"], "display_slug");
      l.bookId = text(row["nnx_info"], "market_data_order_book_id");
    }
    if (row.is_object() && row.contains("price_info") && row["price_info"].is_object()) {
      const json & p = row["price_info"];
      auto open = price(p, "open"), high = price(p, "high"), low = price(p, "low"), last = price(p, "last");
      const int64_t tick = integer(p, "tick_timestamp");
      if (open && high && low && last && tick > 0) {
        auto volume = p.find("turnover_volume");
        l.hasBar = true;
        l.bar = {tick - tick % interval::day, *open, *high, *low, *last,
                 volume != p.end() && volume->is_number() ? volume->get<double>() : 0.0};
      }
    }
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
  std::string counts;  // "etflist 2304, certificatelist 1097"
  int skipped = 0;
  bool stop = false;
  for (const List & list : lists_) {
    if (stop) break;
    size_t found = 0, offset = 0;
    for (int request = 0; request < 10000; request++) {  // a bound, in case total_hits is wrong
      if (listener.cancelled()) return;
      std::string url = list.url + "&limit=" + std::to_string(list.pageSize);
      if (offset) url += "&offset=" + std::to_string(offset);
      HttpResult r = http.get(url, kHeaders);
      if (!r.ok()) {
        const bool refused = r.status == 429 || r.status == 403;
        listener.message("nordnet: " + list.name + ": " +
                         (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)) +
                         (refused ? ", stopping" : ""));
        stop = refused;  // a refusal is for all the lists; another failure only ends this one
        break;
      }
      Page page;
      std::string error;
      if (!parsePage(r.body, page, error)) {
        listener.message("nordnet: " + list.name + ": " + error);
        break;
      }
      skipped += page.skipped;

      // To the store as each page arrives, not all at the end.
      Dataset dataset = store.addDataset({{}, "nordnet"});
      for (NordnetListing & l : page.listings) {
        l.list = list.name;
        Market market = store.addMarket({{}, marketName(l)});
        Instrument base = store.addInstrument({{}, l.symbol, l.name, kindOfNordnetType(l.type), l.isin, l.type});
        const char * label = currencyName(l.currency);
        Instrument quote = store.addInstrument({{}, l.currency, *label ? label : l.currency, InstrumentKind::Currency});
        // Today's bar makes the edge; a listing with none stays a node nothing selects.
        if (l.hasBar) store.appendCandles({base.id, quote.id, market.id, dataset.id}, interval::day, {l.bar});
      }
      found += page.listings.size();
      listings_.insert(listings_.end(), page.listings.begin(), page.listings.end());
      if (!page.listings.empty()) listener.changed();

      offset += static_cast<size_t>(page.rows);
      if (page.rows == 0 || offset >= static_cast<size_t>(page.total)) break;
      if (pause_.count() > 0) std::this_thread::sleep_for(pause_);
    }
    if (found) counts += (counts.empty() ? "" : ", ") + list.name + " " + std::to_string(found);
    if (!stop && pause_.count() > 0) std::this_thread::sleep_for(pause_);
  }
  if (!listings_.empty())
    listener.message("nordnet: " + std::to_string(listings_.size()) + " listings (" + counts + ")" +
                     (skipped ? ", " + std::to_string(skipped) + " rows left out" : std::string()));
}
