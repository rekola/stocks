#include "source/NordnetSource.h"

#include "model/Dates.h"
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

// The indicator endpoint answers to this and to a Nordic locale only (the names come in that language).
const HttpHeaders kIndicatorHeaders = {{"Accept", "*/*"},
                                       {"Origin", "https://www.nordnet.fi"},
                                       {"Referer", "https://www.nordnet.fi/"},
                                       {"x-locale", "fi-FI"}};

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
  using Shape = List::Shape;
  const std::string indicators = "https://api.prod.nntech.io/instrument-screening/v1/indicator/web/list/";
  const std::string base = "https://www.nordnet.fi/api/2/instrument_search/query/";
  const std::string sorted = "sort_order=asc&sort_attribute=symbol";  // a key that does not move between pages
  return {{"etflist", base + "etflist?" + sorted, 500},
          {"certificatelist", base + "certificatelist?apply_filters=instrument_type%3DPTC&" + sorted, 1000},
          // Priced once a day (a NAV), at a time that differs by provider and is not known: so, hourly.
          {"fundlist", base + "fundlist?" + sorted, 1000, std::chrono::hours(1)},
          {"stocklist", base + "stocklist?" + sorted, 1000},  // about 12,000 rows: a dozen pages
          // One answer each, no sparkline (it is only the day's development in per cent).
          {"indices", indicators + "WORLDMAP", 0, std::chrono::milliseconds(0), Shape::Indicators, "Indices"},
          {"currencies", indicators + "CURRENCY_GLOBAL", 0, std::chrono::milliseconds(0), Shape::Indicators, "Currencies"},
          {"commodities", indicators + "COMMODITIES", 0, std::chrono::milliseconds(0), Shape::Indicators, "Commodities"}};
}

const char * NordnetSource::knownMarketName(int id) {
  static const std::pair<int, const char *> known[] = {{4, "Xetra"},
                                                       {11, "Nasdaq Stockholm"},
                                                       {14, "Nasdaq Copenhagen"},
                                                       {24, "Nasdaq Helsinki"},
                                                       {53, "First North Sweden"},
                                                       {54, "First North Finland"},
                                                       {55, "First North Denmark"}};
  for (const auto & [k, name] : known)
    if (k == id) return name;
  return nullptr;
}

const char * NordnetSource::knownMarketMic(int id) {
  switch (id) {
    case 11: return "XSTO";
    case 14: return "XCSE";
    case 24: return "XHEL";
    default: return nullptr;
  }
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
      const int64_t tick = integer(p, "tick_timestamp") / 1000;  // the answer is in milliseconds; seconds from here on
      // A fund has only its NAV: the bar's close, which is also the last price while the slot is still in progress (the
      // NAV of the day is dated by its tick). Its open, high and low are undefined, and so is a volume the answer lacks.
      const bool fund = kindOfNordnetType(l.type) == InstrumentKind::Fund;
      if (fund && last) open = high = low = kUndefined;
      if ((fund || (open && high && low)) && last && tick > 0) {
        auto volume = p.find("turnover_volume");  // written to the bar when the answer has it; volume weights the bars
        l.hasBar = true;
        // Keyed by the trading date, not by the instant: the date of the last tick in UTC (a market east of UTC+9 around
        // its open would need its own time zone).
        l.bar = {dayNumberOfInstant(tick), *open, *high, *low, *last,
                 volume != p.end() && volume->is_number() ? volume->get<double>() : kUndefined};
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

bool NordnetSource::parseIndicators(const std::string & body, std::vector<NordnetIndicator> & rows, std::string & error) {
  json doc = json::parse(body, nullptr, /*allow_exceptions=*/false);
  if (doc.is_discarded() || !doc.is_array()) {
    error = "not an indicator list";
    return false;
  }
  for (const json & row : doc) {
    if (!row.is_object()) continue;
    NordnetIndicator r;
    r.instrumentId = text(row, "instrumentId");
    r.isin = text(row, "isin");
    r.name = text(row, "name");
    r.orderBookId = text(row, "orderBookId");
    r.symbol = text(row, "symbol");
    r.type = text(row, "type");
    r.country = text(row, "countryCode");
    if (r.symbol.empty() || r.orderBookId.empty() || r.type.empty()) continue;
    if (auto p = row.find("price"); p != row.end() && p->is_object()) {
      // The prices are bare numbers here, not {"price": ...} as in the search lists.
      auto number = [&](const char * key) -> std::optional<double> {
        auto it = p->find(key);
        return it != p->end() && it->is_number() ? std::optional<double>(it->get<double>()) : std::nullopt;
      };
      const int64_t tick = integer(*p, "timestamp") / 1000;  // milliseconds in the answer
      auto h = number("high"), l = number("low"), c = number("last");
      if (h && l && c && tick > 0) {
        r.hasBar = true;
        // No open and no volume: undefined. Keyed by the date of the last tick in UTC.
        r.bar = {dayNumberOfInstant(tick), kUndefined, *h, *l, *c, kUndefined};
      }
    }
    rows.push_back(std::move(r));
  }
  return true;
}

size_t NordnetSource::loadIndicators(const List & list, Store & store, Http & http, SourceListener & listener, bool & stop) {
  HttpResult r = http.get(list.url, kIndicatorHeaders);
  if (!r.ok()) {
    const bool refused = r.status == 429 || r.status == 403;
    listener.message("nordnet: " + list.name + ": " +
                     (r.status == 0 ? (r.error.empty() ? std::string("no answer") : r.error) : "HTTP " + std::to_string(r.status)) +
                     (refused ? ", stopping" : ""));
    stop = refused;
    return 0;
  }
  std::vector<NordnetIndicator> rows;
  std::string error;
  if (!parseIndicators(r.body, rows, error)) {
    listener.message("nordnet: " + list.name + ": " + error);
    return 0;
  }
  Dataset dataset = store.addDataset({{}, "nordnet"});
  Market market = store.addMarket({{}, list.market});
  size_t written = 0;
  for (NordnetIndicator & row : rows) {
    row.list = list.name;
    Instrument base, quote;
    if (row.type == "CURRENCY") {
      // A pair is an edge between two currencies: the name is "EUR/USD".
      const size_t slash = row.name.find('/');
      if (slash == std::string::npos) continue;
      base = currencyNode(store, row.name.substr(0, slash));
      quote = currencyNode(store, row.name.substr(slash + 1));
    } else {
      // An index or a commodity is its ISIN where Nordnet has one, else the id Nordnet gives it.
      std::vector<Attribute> identifying;
      if (!row.isin.empty()) identifying.push_back({predicate::isin, row.isin});
      if (!row.instrumentId.empty()) identifying.push_back({predicate::nordnetInstrument, row.instrumentId});
      if (identifying.empty()) continue;
      const InstrumentKind kind = row.type == "INDEX" ? InstrumentKind::Index
                                  : row.type == "COMMODITY" ? InstrumentKind::Commodity
                                                            : InstrumentKind::Other;
      std::vector<Attribute> rest = {{predicate::name, row.name}, {predicate::type, row.type}};
      if (kind != InstrumentKind::Other) rest.push_back({predicate::kind, kindName(kind)});
      base = store.resolveInstrument(identifying, rest);
      // An index is in points. The answer gives a commodity no currency; the owner's table has them all in dollars.
      quote = row.type == "INDEX" ? quoteNode(store, "pts") : currencyNode(store, "USD");
    }
    if (row.hasBar) {
      const EdgeKey edge{base.id, quote.id, market.id, dataset.id};
      store.appendCandles(edge, Resolution::Day, {row.bar});
      store.setEdgeAttribute(edge, {predicate::symbol, row.symbol});
      store.setEdgeAttribute(edge, {predicate::orderBook, row.orderBookId});
      store.setEdgeAttribute(edge, {predicate::type, row.type});
    }
    written++;
  }
  indicators_.insert(indicators_.end(), rows.begin(), rows.end());
  if (written) listener.changed();
  return written;
}

void NordnetSource::load(Store & store, Http & http, SourceListener & listener) {
  std::string counts;  // "etflist 2304, certificatelist 1097": the lists loaded now
  int skipped = 0;
  bool stop = false;
  for (const List & list : lists_) {
    if (stop) break;
    const auto now = std::chrono::steady_clock::now();
    auto last = loaded_.find(list.name);
    if (last != loaded_.end() && now - last->second < list.interval) continue;  // not due: what it gave stays
    loaded_[list.name] = now;
    std::erase_if(listings_, [&](const NordnetListing & l) { return l.list == list.name; });
    std::erase_if(indicators_, [&](const NordnetIndicator & i) { return i.list == list.name; });
    if (list.shape == List::Shape::Indicators) {
      if (listener.cancelled()) return;
      const size_t n = loadIndicators(list, store, http, listener, stop);
      if (n) counts += (counts.empty() ? "" : ", ") + list.name + " " + std::to_string(n);
      if (!stop && pause_.count() > 0) std::this_thread::sleep_for(pause_);
      continue;
    }
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
        // The market with Nordnet's id, or the one with the name where that is known (one a provider with that name
        // found first); else a new one, to be named later.
        std::vector<Attribute> says = {{predicate::nordnetId, std::to_string(l.marketId)}};
        // The code is looked for before the name: it is what identifies a market, the way an ISIN does an instrument.
        if (const char * mic = knownMarketMic(l.marketId)) says.push_back({predicate::mic, mic});
        if (const char * name = knownMarketName(l.marketId)) says.push_back({predicate::name, name});
        Market market = store.resolveMarket(says);
        Instrument base = store.addInstrument({{}, "", l.name, kindOfNordnetType(l.type), l.isin, l.type});
        Instrument quote = currencyNode(store, l.currency);
        // Today's bar makes the edge; a listing with none stays a node nothing selects.
        if (l.hasBar) {
          const EdgeKey edge{base.id, quote.id, market.id, dataset.id};
          store.appendCandles(edge, Resolution::Day, {l.bar});
          // What the market says of the instrument is said on the edge: what it calls it and the ids it is asked for by.
          store.setEdgeAttribute(edge, {predicate::symbol, l.symbol});
          store.setEdgeAttribute(edge, {predicate::identifier, l.identifier});
          store.setEdgeAttribute(edge, {predicate::orderBook, l.bookId});
        }
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
  if (!counts.empty())
    listener.message("nordnet: " + std::to_string(listings_.size() + indicators_.size()) + " listings (" + counts + ")" +
                     (skipped ? ", " + std::to_string(skipped) + " rows left out" : std::string()));
}
