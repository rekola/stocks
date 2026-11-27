#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "model/Isin.h"
#include "model/Period.h"
#include "source/InstrumentLists.h"
#include "source/NordnetSource.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/nordnet/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// The endpoints as the fixtures hold them: the ETFs are five rows, three then two, with a total of five; the
// certificates are four rows in one page.
HttpResult pages(const std::string & url) {
  if (url.find("certificatelist") != std::string::npos) return ok(fixture("certificatelist-1.json"));
  return ok(fixture(url.find("offset=3") != std::string::npos ? "etflist-2.json" : "etflist-1.json"));
}

bool hasHeader(const HttpHeaders & headers, const std::string & name, const std::string & value) {
  for (const auto & [n, v] : headers)
    if (n == name && v == value) return true;
  return false;
}

// The default lists with these names, in this order: a test does not depend on where a list stands among the others.
std::vector<NordnetSource::List> listsNamed(std::initializer_list<const char *> names) {
  std::vector<NordnetSource::List> out;
  for (const char * name : names)
    for (const NordnetSource::List & list : NordnetSource::defaultLists())
      if (list.name == name) out.push_back(list);
  return out;
}

// The ETFs and the certificates, which the fixtures above answer.
std::vector<NordnetSource::List> etfAndCertificateLists() {
  return listsNamed({"etflist", "certificatelist"});  // loaded at every load, as the lists have no interval
}

NordnetSource quick() { return NordnetSource(etfAndCertificateLists(), std::chrono::milliseconds(0)); }

}  // namespace

TEST(a_page_of_the_etf_list_becomes_listings_with_the_ids_the_history_needs) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("etflist-1.json"), page, error));
  CHECK(page.rows == 3 && page.total == 5 && page.skipped == 0 && page.listings.size() == 3);
  const NordnetListing & l = page.listings[0];
  CHECK(l.symbol == "FLXK" && l.isin == "IE00BHZRR030" && l.currency == "EUR" && l.type == "UETF");
  CHECK(l.instrumentId == 17059750 && l.marketId == 4 && l.marketSubId == 51 && l.identifier == "FLXK");
  CHECK(l.name == "Franklin FTSE Korea UCITS ETF" && l.issuer == "Franklin Templeton" && l.country == "DE");
  CHECK(page.listings[1].symbol == "XACT BULL" && page.listings[1].currency == "SEK" && page.listings[1].type == "KETF");
  CHECK(page.listings[2].marketId == 14 && page.listings[2].currency == "DKK");
}

TEST(the_returns_of_a_listing_are_its_changes_from_the_first_list_with_no_history) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("etflist-1.json"), page, error));
  const Changes & c = page.listings[0].changes;  // FLXK
  CHECK(c.pct[0] == -8.2 && c.pct[1] == -12.49 && c.pct[2] == -18.37 && c.pct[3] == -22.26 && c.pct[4] == 59.31 && c.pct[5] == 123.05);
  CHECK(!isDefined(c.pct[6]));  // FLXK has a `yield_max` (since the start) and no `yield_10y`
  NordnetSource::Page old;
  CHECK(NordnetSource::parsePage(fixture("etflist-1.json"), old, error));
  CHECK(old.listings[1].changes.pct[6] == 266.61);  // XACT BULL: yield_10y, not its yield_max (1012.19)
  // Written with the edge: a table can sort by any of them at once.
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);
  auto s = store->snapshot();
  const InstrumentId flxk = *s->findInstrumentByIsin("IE00BHZRR030");
  bool found = false;
  for (const EdgeKey & edge : s->edgesOf(flxk)) {
    found = true;
    CHECK(s->changes(edge).pct[static_cast<size_t>(Period::Week)] == -12.49 && s->changes(edge).pct[static_cast<size_t>(Period::Year3)] == 123.05 &&
          !isDefined(s->changes(edge).pct[static_cast<size_t>(Period::Year10)]));
  }
  CHECK(found);
}

TEST(the_days_high_and_low_of_a_listing_and_of_an_indicator_are_the_days_range_and_nothing_else_is_taken_for_a_longer_one) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("etflist-1.json"), page, error));
  const Ranges & r = page.listings[0].ranges;  // FLXK: price_info high 68.48, low 65.03
  CHECK(r.high[static_cast<size_t>(Period::Day)] == 68.48 && r.low[static_cast<size_t>(Period::Day)] == 65.03);
  for (size_t p = 1; p < kChangeCount; p++) CHECK(!isDefined(r.high[p]) && !isDefined(r.low[p]));  // the answer has no week, month or year range
  NordnetSource::Page funds;
  CHECK(NordnetSource::parsePage(fixture("fundlist-1.json"), funds, error));
  for (const NordnetListing & l : funds.listings) CHECK(!l.ranges.says() && l.hasBar && !isDefined(l.bar.high) && !isDefined(l.bar.low) && !isDefined(l.bar.open));  // a NAV: a close, no range
  // Stored with the edge: shown before any history is loaded.
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);
  auto s = store->snapshot();
  bool found = false;
  for (const EdgeKey & edge : s->edgesOf(*s->findInstrumentByIsin("IE00BHZRR030"))) {
    found = true;
    CHECK(s->ranges(edge).high[static_cast<size_t>(Period::Day)] == 68.48 && s->ranges(edge).low[static_cast<size_t>(Period::Day)] == 65.03);
    CHECK(!isDefined(s->ranges(edge).high[static_cast<size_t>(Period::Year)]) && s->queue(edge, Resolution::Day).empty());
  }
  CHECK(found);
  // An indicator row's price block: the same, the session's high and low.
  std::vector<NordnetIndicator> rows;
  CHECK(NordnetSource::parseIndicators(fixture("indicators-indices.json"), rows, error));
  for (const NordnetIndicator & i : rows)
    if (i.symbol == "DAX") CHECK(i.ranges.high[static_cast<size_t>(Period::Day)] == 25194.64 && i.ranges.low[static_cast<size_t>(Period::Day)] == 24801.22 && !isDefined(i.ranges.high[static_cast<size_t>(Period::Year)]));
}

TEST(a_market_is_named_by_its_nordnet_id_where_that_is_known) {
  CHECK(std::string(NordnetSource::knownMarketName(4)) == "Xetra");
  CHECK(std::string(NordnetSource::knownMarketName(11)) == "Nasdaq Stockholm");
  CHECK(std::string(NordnetSource::knownMarketName(14)) == "Nasdaq Copenhagen");
  CHECK(std::string(NordnetSource::knownMarketName(24)) == "Nasdaq Helsinki");  // the same market as the Yahoo lists name
  CHECK(std::string(NordnetSource::knownMarketName(53)) == "First North Sweden");
  CHECK(std::string(NordnetSource::knownMarketName(19)) == "Nasdaq" && std::string(NordnetSource::knownMarketMic(19)) == "XNAS");
  CHECK(std::string(NordnetSource::knownMarketName(49)) == "Euronext Growth Oslo");
  CHECK(std::string(NordnetSource::knownMarketName(61)) == "Nordic MTF");  // Nordic Growth Market's multilateral facility
  CHECK(NordnetSource::knownMarketName(58) == nullptr);  // not known
  CHECK(std::string(NordnetSource::knownMarketMic(78)) == kOffExchange);  // the funds: priced by their provider, on no venue
  CHECK(std::string(NordnetSource::knownMarketMic(24)) == "XHEL" &&
        NordnetSource::knownMarketMic(53) == std::string("FNSE") &&
        std::string(NordnetSource::knownMarketMic(61)) == "NMTF" && NordnetSource::knownMarketMic(58) == nullptr);
  // A code identifies a market, so two markets never share one (Oslo's growth market has its segment's, not Oslo Børs's), nor a name.
  std::set<std::string> codes, names;
  for (int id = 0; id < 200; id++) {
    if (const char * name = NordnetSource::knownMarketName(id)) CHECK(names.insert(name).second);
    if (const char * mic = NordnetSource::knownMarketMic(id)) CHECK(codes.insert(mic).second);
  }
  CHECK(names.size() >= 30);
}

TEST(a_page_of_certificates_has_the_same_shape) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("certificatelist-1.json"), page, error));
  CHECK(page.rows == 4 && page.total == 4 && page.skipped == 0 && page.listings.size() == 4);
  const NordnetListing & gold = page.listings[0];
  CHECK(gold.symbol == "4GLD" && gold.isin == "DE000A0S9GB0" && gold.currency == "EUR" && gold.type == "PTC");
  CHECK(gold.name == "Xetra-Gold ETC" && gold.marketId == 4 && gold.instrumentId == 16127916);
  CHECK(page.listings[1].currency == "NOK" && page.listings[1].marketId == 53);  // a Norwegian certificate on a Swedish market
}

TEST(rows_without_what_a_listing_needs_are_counted_and_left_out) {
  const char * body = R"({"rows":3,"total_hits":3,"results":[
    {"instrument_info":{"instrument_id":1,"symbol":"A","isin":"ZZ0000000016","currency":"EUR","name":"A fund"},"market_info":{"market_id":4,"identifier":"A"}},
    {"instrument_info":{"instrument_id":2,"symbol":"B","currency":"EUR"},"market_info":{"market_id":4}},
    {"instrument_info":{"instrument_id":3,"symbol":"C","isin":"ZZ0000000032","currency":"EUR"}}]})";
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(body, page, error));
  CHECK(page.rows == 3 && page.skipped == 2 && page.listings.size() == 1 && page.listings[0].symbol == "A");
  CHECK(page.listings[0].name == "A fund");  // the name where there is no long_name
}

TEST(what_is_not_a_page_is_refused_with_the_reason) {
  NordnetSource::Page page;
  std::string error;
  CHECK(!NordnetSource::parsePage("", page, error) && !error.empty());
  CHECK(!NordnetSource::parsePage("{\"results\": 5}", page, error));
  CHECK(!NordnetSource::parsePage(R"({"code":"NEXT_INVALID_PARAMETER","message":"Invalid parameter: N/A"})", page, error) &&
        error == "Invalid parameter: N/A");
}

TEST(loading_pages_through_every_list_and_stores_instruments_markets_and_edges) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);

  // Three requests: the ETFs with no offset and then where the first page ended, then the certificates.
  CHECK(http.urls.size() == 3);
  CHECK(http.urls[0].find("https://www.nordnet.fi/api/2/instrument_search/query/etflist?") == 0);
  CHECK(http.urls[0].find("limit=500") != std::string::npos && http.urls[0].find("offset") == std::string::npos);
  CHECK(http.urls[0].find("sort_attribute=symbol") != std::string::npos);  // a key that does not move between pages
  CHECK(http.urls[1].find("offset=3") != std::string::npos);
  CHECK(http.urls[2].find("https://www.nordnet.fi/api/2/instrument_search/query/certificatelist?apply_filters=instrument_type%3DPTC&") == 0);
  CHECK(http.urls[2].find("limit=1000") != std::string::npos && http.urls[2].find("offset") == std::string::npos);

  // The store is told after each page, not once at the end.
  CHECK(listener.changes == 3);
  // One certificate is on a market this table has no MIC for (58): it is said
  // once and left out.
  CHECK(listener.messages.size() == 2 &&
        listener.messages[0].find("Nordnet market 58") != std::string::npos);
  CHECK(listener.messages[1] ==
        "nordnet: 8 listings (etflist 5, certificatelist 3), 1 rows left out");
  CHECK(source.listings().size() == 8 &&
        source.listings()[0].list == "etflist" &&
        source.listings()[7].list == "certificatelist");

  auto s = store->snapshot();
  // Seven instruments (one ETF is listed on two markets) and the four quote
  // currencies.
  CHECK(s->nodeCount(NodeType::Instrument) == 11);
  InstrumentId xaix = *s->findInstrumentByIsin("IE00BGV5VN51");
  CHECK(s->kind(xaix) == InstrumentKind::Etf && s->isin(xaix) == "IE00BGV5VN51");
  CHECK(s->kind(*s->findInstrumentByIsin("DE000A0S9GB0")) == InstrumentKind::Certificate);  // a certificate, whoever lists it
  CHECK(s->kind(*s->findInstrument("EUR")) == InstrumentKind::Currency && s->name(*s->findInstrument("EUR")) == "Euro");
  CHECK(s->name(*s->findInstrument("DKK")) == "Danish krone");
  CHECK(s->nodeCount(NodeType::Market) == 5);
  MarketId xetra = *s->findMarket("XETR"), stockholm = *s->findMarket("XSTO");
  CHECK(s->findMarket("XCSE"));
  CHECK(s->edges(xetra).size() == 3 && s->edges(stockholm).size() == 2);  // FLXK, XAIX, 4GLD; XACT BULL, XAIX
  CHECK(!s->findInstrumentByIsin(
      "DE000SU696R4")); // on a market with no MIC: not made
  // No tick time: a node, and no edge.
  CHECK(s->findInstrumentByIsin("NO0010903685") &&
        s->edges(*s->findMarket("FNSE")).empty());
  DatasetId nordnet = *s->findDataset(kNordnetDataset);
  EdgeKey onXetra{xaix, *s->findInstrument("EUR"), xetra, nordnet};
  EdgeKey onStockholm{xaix, *s->findInstrument("SEK"), stockholm, nordnet};
  // Each listing's price block is its quote, at the time of the tick (16:30 UTC on the 20th), and no bar: the bars are the history's.
  const auto today = s->quote(onXetra);
  CHECK(today && today->last == 184.42 && today->time == 1795192200 && dayNumberOfInstant(today->time) == dayNumber(2026, 11, 20));
  CHECK(s->candles(onXetra, Resolution::Day).candles.empty());
  bool a = false, b = false;
  for (const EdgeKey & k : s->edges(xetra)) a = a || k == onXetra;
  for (const EdgeKey & k : s->edges(stockholm)) b = b || k == onStockholm;
  CHECK(a && b);  // one instrument, two edges, each in its own currency
}

TEST(a_page_of_a_list_is_one_publication) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  auto lists = listsNamed({"certificatelist"});  // one page
  NordnetSource source(lists, std::chrono::milliseconds(0));
  const auto before = store->snapshot();
  source.load(*store, http, listener);
  const auto after = store->snapshot();
  CHECK(http.urls.size() == 1 && listener.changes == 1);
  CHECK(after->version() == before->version() + 1 && after->quoteRowCount() > 0 && before->quoteRowCount() == 0);
}

namespace {

// A page of the ETF list holding the first fixture row once per (market id, country, name), all of one ISIN.
std::string pageOfListings(const std::string & isin, const std::vector<std::tuple<int, std::string, std::string>> & rows) {
  using nlohmann::json;
  const json first = json::parse(fixture("etflist-1.json"))["results"][0];
  json page = {{"rows", rows.size()}, {"total_hits", rows.size()}, {"results", json::array()}};
  for (const auto & [market, country, name] : rows) {
    json row = first;
    row["instrument_info"]["isin"] = isin;
    row["instrument_info"]["long_name"] = name;
    row["market_info"]["market_id"] = market;
    row["exchange_info"] = {{"exchange_country", country}};
    page["results"].push_back(row);
  }
  return page.dump();
}

}  // namespace

namespace {

// A page of the ETF list with one row for each ISIN, all of the first fixture row otherwise.
std::string pageOfIsins(const std::vector<std::string> & isins) {
  using nlohmann::json;
  const json first = json::parse(fixture("etflist-1.json"))["results"][0];
  json page = {{"rows", isins.size()}, {"total_hits", isins.size()}, {"results", json::array()}};
  for (const std::string & isin : isins) {
    json row = first;
    row["instrument_info"]["isin"] = isin;
    page["results"].push_back(row);
  }
  return page.dump();
}

bool saidLeftOut(const Listener & listener, const std::string & what) {
  for (const std::string & m : listener.messages)
    if (m.find("left out") != std::string::npos && m.find(what) != std::string::npos) return true;
  return false;
}

}  // namespace

TEST(a_row_whose_isin_cannot_be_named_is_left_out_and_the_page_and_the_next_list_are_kept) {
  // IE00BHZRR031 has the wrong check digit: the store names no instrument for it.
  CHECK(isValidIsin("IE00BHZRR030") && !isValidIsin("IE00BHZRR031"));
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("certificatelist") != std::string::npos) return ok(fixture("certificatelist-1.json"));
    return ok(pageOfIsins({"IE00BHZRR030", "IE00BHZRR031"}));
  };
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);  // does not throw
  const auto s = store->snapshot();
  CHECK(s->findInstrumentByIsin("IE00BHZRR030").has_value() && !s->findInstrumentByIsin("IE00BHZRR031").has_value());
  CHECK(s->quoteRowCount() > 1);  // the good row's edge and the certificates' too
  CHECK(source.listings().size() > 1 && saidLeftOut(listener, "1 row left out") && saidLeftOut(listener, "IE00BHZRR031"));
  bool certificates = false;
  for (const std::string & url : http.urls) certificates = certificates || url.find("certificatelist") != std::string::npos;
  CHECK(certificates);
  for (const NordnetListing & l : source.listings()) CHECK(l.isin != "IE00BHZRR031");
}

TEST(a_row_of_an_indicator_list_with_a_bad_isin_is_left_out_and_the_next_list_is_kept) {
  using nlohmann::json;
  json indices = json::parse(fixture("indicators-indices.json"));
  const std::string bad = "FI0008900007";  // OMX Helsinki PI (FI0008900006) with a wrong check digit
  CHECK(indices[1].contains("isin") && !isValidIsin(bad));
  indices[1]["isin"] = bad;
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [&](const std::string & url) {
    if (url.find("WORLDMAP") != std::string::npos) return ok(indices.dump());
    if (url.find("CURRENCY_GLOBAL") != std::string::npos) return ok(fixture("indicators-currencies.json"));
    return ok(fixture("indicators-commodities.json"));
  };
  auto lists = listsNamed({"indices", "currencies", "commodities"});  // only the three indicator lists
  Listener listener;
  NordnetSource source(lists, std::chrono::milliseconds(0));
  source.load(*store, http, listener);  // does not throw
  const auto s = store->snapshot();
  CHECK(http.urls.size() == 3);  // the lists after the bad row are still asked for
  CHECK(!s->findInstrumentByIsin(bad).has_value() && s->findInstrumentByIsin("XC0009655157").has_value());
  CHECK(saidLeftOut(listener, "1 row left out") && saidLeftOut(listener, bad));
  CHECK(listener.messages.back().find("indices 3, currencies 2, commodities 3") != std::string::npos);
  for (const NordnetIndicator & i : source.indicators()) CHECK(i.isin != bad);
}

TEST(a_national_currency_list_is_loaded_like_the_global_one) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("indicators-currencies.json")); };
  auto lists = listsNamed({"currencies-no"});
  Listener listener;
  NordnetSource source(lists, std::chrono::milliseconds(0));
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 1 && http.urls[0].find("/list/NO_CURRENCY") != std::string::npos);
  CHECK(http.urls[0].find("spark") == std::string::npos);
  CHECK(source.indicators().size() == 2 && source.indicators()[0].list == "currencies-no");
  CHECK(store->snapshot()->findInstrument("EUR").has_value() && store->snapshot()->findInstrument("JPY").has_value());
}

TEST(a_currency_pair_in_two_lists_is_kept_once) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("indicators-currencies.json")); };  // the same two pairs from each list
  auto lists = listsNamed({"currencies", "currencies-no", "currencies-se"});
  Listener listener;
  NordnetSource source(lists, std::chrono::milliseconds(0));
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 3);
  CHECK(source.indicators().size() == 2 && source.indicators()[0].list == "currencies");
}

TEST(the_funds_of_nordnets_list_are_on_the_off_exchange_market_and_are_funds) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("fundlist-1.json"), page, error) && !page.listings.empty());
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("fundlist-1.json")); };
  auto lists = listsNamed({"fundlist"});
  Listener listener;
  NordnetSource(lists, std::chrono::milliseconds(0)).load(*store, http, listener);
  auto s = store->snapshot();
  const auto xoff = s->findMarket(kOffExchange);
  CHECK(xoff && !s->edges(*xoff).empty());  // none left out for want of a market
  const auto fund = s->findInstrumentByIsin(page.listings[0].isin);
  CHECK(fund && s->kind(*fund) == InstrumentKind::Fund);
}

TEST(an_indicator_row_with_another_price_source_is_nordnets_own) {
  using nlohmann::json;
  json rows = json::parse(fixture("indicators-currencies.json"));
  rows[1]["price"]["priceSource"] = "SOMEONE";
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [&](const std::string &) { return ok(rows.dump()); };
  auto lists = listsNamed({"currencies"});
  Listener listener;
  NordnetSource source(lists, std::chrono::milliseconds(0));
  source.load(*store, http, listener);
  auto s = store->snapshot();
  const MarketId none = *s->findMarket("XXXX");
  CHECK(s->listing({*s->findInstrument("EUR"), *s->findInstrument("USD"), none, *s->findDataset(kMorningstarDataset)}));
  CHECK(s->listing({*s->findInstrument("USD"), *s->findInstrument("JPY"), none, *s->findDataset(kNordnetDataset)}));
  CHECK(source.historyProvider()->datasets() == (std::vector<std::string>{kNordnetDataset, kMorningstarDataset}));
}

TEST(listings_of_one_isin_with_different_names_leave_one_name_and_no_change_on_the_next_pass) {
  // Xetra (DE), Stockholm (SE) and Copenhagen (DK): the home listing of a DE ISIN is the first, of an IE one there is none.
  for (const std::string isin : {"DE0007164600", "IE00BHZRR030"}) {
    auto store = makeMemoryStore();
    FakeHttp http;
    const bool hasHome = isin.substr(0, 2) == "DE";
    http.answer = [&](const std::string &) {
      return ok(pageOfListings(isin, {{11, "SE", "Abroad"}, {4, "DE", "Home"}, {14, "DK", "Elsewhere"}}));
    };
    Listener listener;
    auto lists = listsNamed({"etflist"});
    NordnetSource source(lists, std::chrono::milliseconds(0));
    source.load(*store, http, listener);
    const auto id = store->snapshot()->findInstrumentByIsin(isin);
    CHECK(id.has_value());
    // With a home listing its name stands; without one, the first said does (the Xetra row is the second here).
    const std::string name = store->snapshot()->name(NodeRef(*id));
    CHECK(name == (hasHome ? "Home" : "Abroad"));
    const uint64_t version = store->snapshot()->version();
    source.load(*store, http, listener);
    CHECK(store->snapshot()->name(NodeRef(*id)) == name && store->snapshot()->version() == version);
  }
}

TEST(a_source_that_is_not_the_instruments_own_says_its_name_and_type_only_where_none_is_said) {
  auto store = makeMemoryStore();
  store->batch([&](Batch & b) {
    addInstrumentUnlessSaid(b, "", "First", InstrumentKind::Fund, "IE00BGV5VN51", "FUND");
    addInstrumentUnlessSaid(b, "", "Second", InstrumentKind::Fund, "IE00BGV5VN51", "OTHER");
  });
  const auto id = *store->snapshot()->findInstrumentByIsin("IE00BGV5VN51");
  CHECK(store->snapshot()->name(NodeRef(id)) == "First" && store->snapshot()->said(NodeRef(id), predicate::type) == "FUND");
}

TEST(loading_again_replaces_todays_bars_and_a_new_day_adds_one) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);
  auto first = store->snapshot();
  source.load(*store, http, listener);
  auto second = store->snapshot();
  // The same nodes and edges, and today's bar replaced, not added again.
  CHECK(second->nodeCount(NodeType::Instrument) ==
            first->nodeCount(NodeType::Instrument) &&
        second->nodeCount(NodeType::Instrument) == 11);
  CHECK(source.listings().size() == 8);
  MarketId xetra = *second->findMarket("XETR");
  CHECK(second->edges(xetra) == first->edges(xetra));
  for (const EdgeKey & k : second->edges(xetra)) CHECK(second->quote(k) && second->candles(k, Resolution::Day).candles.empty());
}

TEST(the_requests_look_like_a_browsers_and_carry_no_cookie) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  quick().load(*store, http, listener);
  for (const HttpHeaders & h : http.headers) {
    CHECK(hasHeader(h, "Accept", "application/json"));
    CHECK(hasHeader(h, "Client-Id", "NEXT"));
    CHECK(hasHeader(h, "Origin", "https://www.nordnet.fi"));
    CHECK(hasHeader(h, "Referer", "https://www.nordnet.fi/"));
    for (const auto & [name, value] : h) CHECK(name != "Cookie" && name != "Authorization");
  }
}

TEST(a_refusal_stops_every_list_and_says_why) {
  for (int status : {403, 429}) {
    auto store = makeMemoryStore();
    FakeHttp http;
    http.answer = [status](const std::string &) { return failed(status); };
    Listener listener;
    quick().load(*store, http, listener);
    CHECK(http.urls.size() == 1);  // the certificates are not tried after the ETFs were refused
    CHECK(listener.messages.size() == 1 && listener.messages[0] == "nordnet: etflist: HTTP " + std::to_string(status) + ", stopping");
    CHECK(store->snapshot()->version() == 0);
  }
}

TEST(another_failure_ends_only_that_list) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) { return url.find("etflist") != std::string::npos ? failed(500) : pages(url); };
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 2);  // the ETFs once, then the certificates
  CHECK(listener.messages.size() == 3 &&
        listener.messages[0] == "nordnet: etflist: HTTP 500");
  CHECK(listener.messages[2] ==
        "nordnet: 3 listings (certificatelist 3), 1 rows left out");
  CHECK(source.listings().size() == 3);

  FakeHttp none;
  none.answer = [](const std::string &) { return failed(0); };
  Listener l2;
  quick().load(*store, none, l2);
  CHECK(none.urls.size() == 2 && l2.messages.size() == 2 && l2.messages[0] == "nordnet: etflist: no answer");
}

TEST(an_answer_that_is_not_a_list_ends_that_list_and_keeps_what_came_before) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("offset=3") != std::string::npos) return ok("<html>");
    return pages(url);
  };
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 3);  // the second ETF page, then the certificates
  CHECK(listener.messages.size() == 3 &&
        listener.messages[0] == "nordnet: etflist: not an instrument list");
  CHECK(listener.messages[2] ==
        "nordnet: 6 listings (etflist 3, certificatelist 3), 1 rows left out");
  CHECK(store->snapshot()->nodeCount(NodeType::Instrument) ==
        3 + 3 + 4); // three ETFs, three certificates and their currencies
}

TEST(an_empty_page_ends_the_load_even_if_the_total_says_more) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("offset") != std::string::npos) return ok(R"({"rows":0,"total_hits":999,"results":[]})");
    return pages(url);
  };
  Listener listener;
  quick().load(*store, http, listener);
  CHECK(http.urls.size() == 3);  // the empty page ends the ETFs; the certificates follow
}

TEST(a_cancelled_load_stops_before_the_next_request) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  struct Cancelling : Listener {
    bool cancelled() const override { return changes > 0; }
  } listener;
  quick().load(*store, http, listener);
  CHECK(http.urls.size() == 1);
}

TEST(a_new_day_adds_a_bar_to_the_series) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);
  // The next day: the Xetra listing of XAIX ticks a day later.
  http.answer = [](const std::string & url) {
    HttpResult r = pages(url);
    for (size_t at; (at = r.body.find("1795192200000")) != std::string::npos;) r.body.replace(at, 13, "1795278600000");
    return r;
  };
  source.load(*store, http, listener);
  auto s = store->snapshot();
  InstrumentId xaix = *s->findInstrumentByIsin("IE00BGV5VN51");
  EdgeKey onXetra{xaix, *s->findInstrument("EUR"), *s->findMarket("XETR"),
                  *s->findDataset(kNordnetDataset)};
  CHECK(s->quote(onXetra) && s->quote(onXetra)->time == 1795278600 && s->candles(onXetra, Resolution::Day).candles.empty());
}

TEST(a_listing_without_a_price_is_a_node_and_has_no_edge) {
  const char * body = R"({"rows":3,"total_hits":3,"results":[
    {"instrument_info":{"instrument_id":1,"symbol":"A","isin":"ZZ0000000016","currency":"EUR","name":"Traded","instrument_type":"UETF"},
     "market_info":{"market_id":4,"identifier":"A"},
     "price_info":{"last":{"price":10.0},"open":{"price":9.5},"high":{"price":10.5},"low":{"price":9.0},"tick_timestamp":1795192200000,"turnover_volume":7}},
    {"instrument_info":{"instrument_id":2,"symbol":"B","isin":"ZZ0000000024","currency":"EUR","name":"Not traded","instrument_type":"UETF"},
     "market_info":{"market_id":4,"identifier":"B"},"price_info":{}},
    {"instrument_info":{"instrument_id":3,"symbol":"C","isin":"ZZ0000000032","currency":"EUR","name":"No open","instrument_type":"UETF"},
     "market_info":{"market_id":4,"identifier":"C"},
     "price_info":{"last":{"price":10.0},"high":{"price":10.5},"low":{"price":9.0},"tick_timestamp":1795192200000}}]})";
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [&](const std::string & url) {
    return url.find("certificatelist") != std::string::npos ? ok(R"({"rows":0,"total_hits":0,"results":[]})") : ok(body);
  };
  Listener listener;
  quick().load(*store, http, listener);
  auto s = store->snapshot();
  CHECK(s->findInstrumentByIsin("ZZ0000000016") && s->findInstrumentByIsin("ZZ0000000024") && s->findInstrumentByIsin("ZZ0000000032"));  // all three are nodes
  CHECK(s->edges(*s->findMarket("XETR")).size() ==
        1); // only the traded one is seen
  CHECK(s->edges(*s->findMarket("XETR"))[0].base ==
        *s->findInstrumentByIsin("ZZ0000000016"));
}

TEST(a_halted_instrument_shows_its_previous_close_and_has_no_bar) {
  const char * body = R"({"rows":1,"total_hits":1,"results":[
    {"instrument_info":{"instrument_id":1,"symbol":"LEHTO","isin":"ZZ0000000016","currency":"EUR","name":"Suspended","instrument_type":"ESH"},
     "market_info":{"market_id":4,"identifier":"LEHTO"},
     "price_info":{"last":{"price":0},"close":{"price":0.0318},"open":{"price":0},"high":{"price":0},"low":{"price":0},"bid":{"price":0},"ask":{"price":0},"tick_timestamp":1795192200000}}]})";
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(body, page, error));
  CHECK(page.listings.size() == 1 && page.listings[0].hasQuote && page.listings[0].quote.last == 0.0318 && !page.listings[0].hasBar);
}

TEST(an_instrument_keeps_the_type_nordnet_gives_it_and_edges_can_be_selected_by_it) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  quick().load(*store, http, listener);
  auto s = store->snapshot();
  CHECK(s->said(NodeRef(*s->findInstrumentByIsin("IE00BHZRR030")), predicate::type) == "UETF" && s->said(NodeRef(*s->findInstrumentByIsin("SE0001342395")), predicate::type) == "KETF");
  CHECK(s->said(NodeRef(*s->findInstrumentByIsin("DE000A0S9GB0")), predicate::type) == "PTC" && s->said(NodeRef(*s->findInstrument("EUR")), predicate::type).empty());
  MarketId xetra = *s->findMarket("XETR");
  auto certificates = s->edges(xetra, [&](InstrumentId i) { return s->said(NodeRef(i), predicate::type) == "PTC"; });
  auto funds = s->edges(xetra, [&](InstrumentId i) { return s->kind(i) == InstrumentKind::Etf; });
  auto byKind = s->edges(xetra, [&](InstrumentId i) { return s->kind(i) == InstrumentKind::Certificate; });
  CHECK(byKind == certificates);  // the type and the combined kind agree here
  CHECK(certificates.size() == 1 && certificates[0].base == *s->findInstrumentByIsin("DE000A0S9GB0"));
  CHECK(funds.size() == 2);  // FLXK and XAIX
}

TEST(the_listings_keep_the_book_id_the_history_is_asked_for_by) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("certificatelist-1.json"), page, error));
  CHECK(page.listings[0].bookId == "c3a7d13a-dd10-4a4f-b969-beeb9bfb12d1");  // the example Xetra-Gold ETC row
  CHECK(page.listings[0].hasBar && page.listings[0].bar.close == 103.78 && page.listings[0].bar.volume == 825020);
  CHECK(!page.listings[1].hasBar);  // no tick time in this one
}

TEST(discovery_asks_to_be_repeated_every_five_minutes) {
  CHECK(NordnetSource().refreshInterval() == std::chrono::minutes(5));
  CHECK(NordnetSource(NordnetSource::defaultLists(), std::chrono::milliseconds(0), std::chrono::seconds(30)).refreshInterval() == std::chrono::seconds(30));
}

TEST(a_page_of_stocks_has_the_same_shape_and_a_bar_from_the_day_so_far) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("stocklist-1.json"), page, error));
  CHECK(page.rows == 3 && page.total > 3 && page.skipped == 0 && page.listings.size() == 3);
  const NordnetListing & first = page.listings[0];
  CHECK(first.symbol == "0M0" && first.isin == "DE000A3ESL51" && first.currency == "EUR" && first.type == "ESH");
  CHECK(first.marketId == 4 && first.hasBar && first.bar.close == 1.55);
  CHECK(NordnetSource::kindOfType(first.type) == InstrumentKind::Company);
}

TEST(a_fund_is_a_bar_with_only_its_nav_as_the_close) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("fundlist-1.json"), page, error));
  CHECK(page.listings.size() == 3);
  const NordnetListing & fund = page.listings[0];
  CHECK(fund.type == "FUCITS" && fund.currency == "USD" && fund.marketId == 78 && !fund.isin.empty());
  // Only a last price (the NAV) and its tick: that is the close; the open, high, low and volume are undefined.
  CHECK(fund.hasBar && fund.bar.close == 22.76);
  CHECK(!isDefined(fund.bar.open) && !isDefined(fund.bar.high) && !isDefined(fund.bar.low) && !isDefined(fund.bar.volume));
  CHECK(fund.bar.time == dayNumberOfInstant(1795132800));  // dated by its tick
  CHECK(!fund.bookId.empty());  // the id the history is asked for by, with the NAV parameter
  CHECK(NordnetSource::kindOfType(fund.type) == InstrumentKind::Fund);
}

TEST(a_stock_bar_carries_the_volume_when_the_answer_has_it) {
  NordnetSource::Page page;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("certificatelist-1.json"), page, error));
  CHECK(page.listings[0].bar.volume == 825020);
  CHECK(NordnetSource::parsePage(fixture("stocklist-1.json"), page, error));
  CHECK(page.listings.front().hasBar);
  // A halted one (zero prices, a previous close) has its final price as a quote and no bar for today.
  CHECK(!page.listings.back().hasBar && page.listings.back().hasQuote && page.listings.back().quote.last == 92.1);
}

TEST(the_default_lists_are_the_etfs_the_certificates_the_funds_and_the_stocks) {
  auto lists = NordnetSource::defaultLists();
  CHECK(lists.size() == 13);
  // The three small indicator lists go first, the commodities (the gold spot that physical gold is estimated from) before the pairs
  // (that value it in euros): none of them waits for the minutes the big lists take.
  CHECK(lists[0].name == "commodities" && lists[1].name == "currencies" && lists[2].name == "currencies-no" &&
        lists[3].name == "currencies-se" && lists[4].name == "currencies-fi" && lists[5].name == "currencies-dk" &&
        lists[6].name == "currencies-de" && lists[7].name == "indices");
  for (size_t i = 2; i < 7; i++) CHECK(lists[i].url.find("_CURRENCY") != std::string::npos);
  for (const auto & l : lists) CHECK(l.url.find("spark") == std::string::npos);  // no list asks for a sparkline
  for (size_t i = 0; i < 8; i++) CHECK(lists[i].shape == NordnetSource::List::Shape::Indicators);
  CHECK(lists[7].market == "Indices");
  CHECK(lists[8].name == "etflist" && lists[9].name == "certificatelist" && lists[10].name == "fundlist" && lists[11].name == "stocklist");
  CHECK(lists[11].url.find("/stocklist?") != std::string::npos && lists[11].pageSize == 1000);
  for (size_t i = 8; i < 12; i++) CHECK(lists[i].shape != NordnetSource::List::Shape::Indicators);
  // The news is last: its items are about the instruments the lists before it have found.
  CHECK(lists[12].name == "news" && lists[12].shape == NordnetSource::List::Shape::News &&
        lists[12].url.find("/news/v1/news/preview/main?includeInstruments=true") != std::string::npos);
}

TEST(
    a_market_is_its_mic_and_a_listing_on_one_nordnet_has_no_mic_for_is_left_out) {
  auto store = makeMemoryStore();
  // Another provider saw Helsinki first, and named it.
  MarketId helsinki = store->addMarket("XHEL", "Helsinki (other name)");
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  quick().load(*store, http, listener);
  auto s = store->snapshot();

  // Xetra is its code, named as Nordnet's list names it; Helsinki keeps the
  // name it was given.
  MarketId xetra = *s->findMarket("XETR");
  CHECK(marketCode(xetra) == "XETR" && s->name(xetra) == "Xetra" &&
        s->attributes(xetra).size() == 3); // the code, the name and the country
  CHECK(s->name(helsinki) == "Helsinki (other name)");
  // Markets Nordnet gives only an id for are not markets here: no edge is made
  // on them, and the load says so.
  CHECK(!s->findMarket("XNYS"));
  bool said = false;
  for (const std::string &m : listener.messages)
    said = said || m.find("no known MIC") != std::string::npos;
  CHECK(said);
  const size_t markets = s->nodeCount(NodeType::Market);
  quick().load(*store, http, listener);
  CHECK(store->snapshot()->nodeCount(NodeType::Market) == markets);
}

TEST(every_kind_of_node_keeps_its_attributes_the_same_way) {
  auto store = makeMemoryStore();
  InstrumentId i = store->addInstrument("NOKIA", "Nokia", InstrumentKind::Company, "FI0009000681", "ESH");
  DatasetId d = store->addDataset(kNordnetDataset);
  MarketId m = store->addMarket("XHEL");
  store->setAttribute(i, {"nordnet-id", "16"});
  store->setAttribute(d, {"description", "Nordnet"});
  auto s = store->snapshot();
  CHECK(s->findByAttribute(NodeType::Instrument, "nordnet-id", "16")->id ==
        i.value);
  CHECK(!s->findByAttribute(NodeType::Market, "nordnet-id",
                            "16")); // by type: another node's, not this one's
  CHECK(s->attributes(i).size() == 6 && s->attributes(d).size() == 2 &&
        s->attributes(m).size() ==
            2); // its code and its name, which is the code until it is said

  // The instrument's name changes and the view follows; unchanged says nothing new.
  store->addInstrument("NOKIA", "Nokia Oyj", InstrumentKind::Company, "FI0009000681", "ESH");
  CHECK(store->snapshot()->name(i) == "Nokia Oyj");
  const uint64_t version = store->snapshot()->version();
  store->addInstrument("NOKIA", "Nokia Oyj", InstrumentKind::Company, "FI0009000681", "ESH");
  store->addInstrument("NOKIA", "", InstrumentKind::Other, "FI0009000681", "");  // says nothing: empty and unknown
  CHECK(store->snapshot()->version() == version && store->snapshot()->kind(i) == InstrumentKind::Company);
}

TEST(a_currency_is_the_node_the_store_has_for_its_symbol) {
  auto store = makeMemoryStore();
  InstrumentId made = currencyNode(*store, "EUR");
  CHECK(store->snapshot()->kind(made) == InstrumentKind::Currency && store->snapshot()->name(made) == "Euro");
  // Another provider's use of the symbol gets the same node, and describes nothing.
  const uint64_t version = store->snapshot()->version();
  CHECK(currencyNode(*store, "EUR") == made && store->snapshot()->version() == version);
  // A node already there under the symbol, made by anyone, is the one it gets.
  InstrumentId other = store->addInstrument("XYZ", "Xyz", InstrumentKind::Other);
  CHECK(currencyNode(*store, "XYZ") == other && store->snapshot()->kind(currencyNode(*store, "XYZ")) == InstrumentKind::Other);
}

TEST(an_instrument_listed_on_two_markets_is_one_node_with_an_edge_and_a_say_for_each) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  quick().load(*store, http, listener);
  auto s = store->snapshot();
  InstrumentId xaix = *s->findInstrumentByIsin("IE00BGV5VN51");
  MarketId xetra = *s->findMarket("XETR"), stockholm = *s->findMarket("XSTO");
  DatasetId nordnet = *s->findDataset(kNordnetDataset);
  const EdgeKey onXetra{xaix, *s->findInstrument("EUR"), xetra, nordnet};
  const EdgeKey onStockholm{xaix, *s->findInstrument("SEK"), stockholm, nordnet};
  CHECK(s->symbol(onXetra) == "XAIX" && s->symbol(onStockholm) == "XAIX");
  CHECK(*s->findEdge("XAIX", xetra) == onXetra && *s->findEdge("XAIX", stockholm) == onStockholm);
  // Each market's own id for it, and the order book its history is asked for by.
  CHECK(s->listing(onXetra)->identifier == "XAIX" && s->listing(onStockholm)->identifier == "419021");
  CHECK(!s->listing(onXetra)->orderBook.empty() && s->listing(onXetra)->type == "UETF");
  CHECK(s->symbol(xaix).empty());  // the instrument itself has none: a market gives it
}

TEST(an_indicator_list_gives_instruments_with_an_isin_and_a_bar_with_no_open_and_no_volume) {
  std::vector<NordnetIndicator> rows;
  std::string error;
  CHECK(NordnetSource::parseIndicators(fixture("indicators-indices.json"), rows, error));
  CHECK(rows.size() == 4);
  const NordnetIndicator * dax = nullptr, * bric = nullptr;
  for (const NordnetIndicator & r : rows) {
    if (r.symbol == "DAX") dax = &r;
    if (r.symbol == "BRIC50D") bric = &r;
  }
  CHECK(dax && dax->isin == "DE0008469008" && dax->type == "INDEX" && dax->country == "DE" && !dax->orderBookId.empty());
  CHECK(dax->hasBar && dax->bar.close == 24802.78 && dax->bar.high == 25194.64 && dax->bar.low == 24801.22);
  CHECK(!isDefined(dax->bar.open) && !isDefined(dax->bar.volume));
  CHECK(bric && bric->isin.empty() && !bric->instrumentId.empty());  // some have no ISIN
  CHECK(!NordnetSource::parseIndicators("{}", rows, error) && error == "not an indicator list");
}

TEST(the_indicator_lists_make_indices_in_points_currency_pairs_and_commodities_in_dollars) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("WORLDMAP") != std::string::npos) return ok(fixture("indicators-indices.json"));
    if (url.find("CURRENCY_GLOBAL") != std::string::npos) return ok(fixture("indicators-currencies.json"));
    return ok(fixture("indicators-commodities.json"));
  };
  auto lists = listsNamed({"indices", "currencies", "commodities"});  // only the three indicator lists
  Listener listener;
  NordnetSource(lists, std::chrono::milliseconds(0)).load(*store, http, listener);
  CHECK(http.urls.size() == 3 && http.urls[0].find("/instrument-screening/v1/indicator/web/list/WORLDMAP") != std::string::npos);
  CHECK(http.urls[0].find("spark") == std::string::npos);  // no sparkline
  auto s = store->snapshot();
  // Every row of these lists names Morningstar as its price source: its edges are Morningstar's, not Nordnet's.
  DatasetId nordnet = *s->findDataset(kMorningstarDataset);
  CHECK(s->name(nordnet) == "Morningstar");

  // An index: its ISIN, in points, on no market.
  InstrumentId dax = *s->findInstrumentByIsin("DE0008469008");
  CHECK(s->kind(dax) == InstrumentKind::Index && s->said(NodeRef(dax), predicate::type) == "INDEX");
  const InstrumentId points = *s->findInstrument("pts");
  CHECK(s->kind(points) == InstrumentKind::Unit);
  const EdgeKey onDax{dax, points, *s->findMarket("XXXX"), nordnet};
  CHECK(s->quote(onDax) && s->candles(onDax, Resolution::Day).candles.empty());
  CHECK(s->symbol(onDax) == "DAX");
  CHECK(!s->listing(onDax)->orderBook.empty() && s->listing(onDax)->type == "INDEX");  // what its history is asked for by
  CHECK(asText(*s->attribute(dax, "country")) == "DE");

  // One with no ISIN has the one source/KnownIsins.h has for its symbol: the real one of the index and of
  // the commodity (XC) for lead.
  const std::string bric = "US26075E1047", leadIsin = "XC0005705527";
  CHECK(s->findInstrumentByIsin(bric) && s->kind(*s->findInstrumentByIsin(bric)) == InstrumentKind::Index);
  CHECK(s->kind(*s->findInstrumentByIsin(leadIsin)) == InstrumentKind::Commodity);

  // A metal the London exchange prices has only a last price, and it is a quote.
  const InstrumentId lead = *s->findInstrumentByIsin(leadIsin);
  const auto leadQuote = s->quote({lead, *s->findInstrument("USD"), *s->findMarket("XXXX"), nordnet});
  CHECK(leadQuote && leadQuote->last == 1808.7);
  CHECK(s->listing(
             {lead, *s->findInstrument("USD"), *s->findMarket("XXXX"), nordnet})
            ->name == "Lyijy");

  // A pair is an edge between two currency nodes, named for the two.
  CHECK(s->listing({*s->findInstrument("EUR"), *s->findInstrument("USD"),
                    *s->findMarket("XXXX"), nordnet})
            ->name == "EUR/USD");
  const EdgeKey eurusd{*s->findInstrument("EUR"), *s->findInstrument("USD"),
                       *s->findMarket("XXXX"), nordnet};
  CHECK(s->quote(eurusd) && s->symbol(eurusd) == "EURUSD");

  // A commodity with an ISIN, quoted in dollars; one with none has Nordnet's id.
  InstrumentId gold = *s->findInstrumentByIsin("XC0009655157");
  CHECK(s->kind(gold) == InstrumentKind::Commodity);
  CHECK(s->quote({gold, *s->findInstrument("USD"), *s->findMarket("XXXX"), nordnet}));
  CHECK(listener.messages.size() == 1 && listener.messages[0].find("indices 4, currencies 2, commodities 3") != std::string::npos);

  // Loading again replaces the quotes and makes nothing new.
  const size_t instruments = s->nodeCount(NodeType::Instrument);
  FakeHttp again;
  again.answer = http.answer;
  Listener second;
  NordnetSource(lists, std::chrono::milliseconds(0)).load(*store, again, second);
  CHECK(store->snapshot()->nodeCount(NodeType::Instrument) == instruments);
}

// The object of a predicate among the facts a parsed row has.
static const Value * said(const NordnetListing & l, const char * predicate) {
  for (const Attribute & a : l.attributes)
    if (a.predicate == predicate) return &a.value;
  return nullptr;
}

TEST(what_a_row_says_of_the_instrument_becomes_triples_of_the_predicates_it_has) {
  NordnetSource::Page stocks, funds, certificates;
  std::string error;
  CHECK(NordnetSource::parsePage(fixture("stocklist-1.json"), stocks, error));
  CHECK(NordnetSource::parsePage(fixture("fundlist-1.json"), funds, error));
  CHECK(NordnetSource::parsePage(fixture("certificatelist-1.json"), certificates, error));

  // A stock with figures and a calendar; one with none says nothing of them.
  CHECK(!said(stocks.listings[0], "pe") && !said(stocks.listings[0], "market-cap"));  // empty blocks: nothing said
  const NordnetListing & abbvie = stocks.listings[1];
  CHECK(asReal(*said(abbvie, "pe")) == 48.8 && asReal(*said(abbvie, "eps")) == 2.36);
  CHECK(asReal(*said(abbvie, "market-cap")) == 456283309908.0);
  CHECK(asReal(*said(abbvie, "dividend-amount")) == 1.66 && asText(*said(abbvie, "dividend-currency")) == "EUR");
  CHECK(*asInteger(*said(abbvie, "dividend-ex-day")) < *asInteger(*said(abbvie, "dividend-pay-day")));  // the ex-date first
  CHECK(asText(*said(abbvie, "report-type")) == "FOURTH_QUARTER_EARNINGS_RESULTS" && !said(abbvie, "pb"));
  CHECK(!said(abbvie, "category") && !said(abbvie, "leverage"));
  CHECK(asBoolean(*said(abbvie, "tradable")) == true && asInteger(*said(abbvie, "owners")) == 1);

  // A fund.
  const NordnetListing & fund = funds.listings[0];
  CHECK(asText(*said(fund, "category")) == "Greater China Equity" && asInteger(*said(fund, "risk-class")) == 5);
  CHECK(asInteger(*said(fund, "morningstar-rating")) == 2);
  CHECK(asReal(*said(fund, "yearly-fee")) == 1.93 && asReal(*said(fund, "net-assets")) == 255946937.0);
  CHECK(asBoolean(*said(fund, "low-carbon")) == true && asText(*said(fund, "universe-currency")) == "EUR");
  CHECK(asInteger(*said(fund, "start-day")) == dayNumberOfInstant(704376000) && !said(fund, "pe"));
  CHECK(asInteger(*said(fund, "owners")) == 95 && asInteger(*said(fund, "collateral-percent")) == 70);
  CHECK(asText(*said(fund, "sfdr-article")) == "Artikla 8");

  // A certificate: leverage, direction, the underlying.
  const NordnetListing & bull = certificates.listings[1];
  CHECK(asInteger(*said(bull, "leverage")) == 1 && asText(*said(bull, "direction")) == "Long" && asBoolean(*said(bull, "high-risk")) == false);
  CHECK(asText(*said(bull, "underlying-name")) == "OMX Stockholm 30 Index" && asInteger(*said(bull, "underlying-id")) == 16104939);
  CHECK(said(bull, "first-trading-day") && !said(bull, "management-fee"));
  CHECK(asText(*said(bull, "issuer")) == "Nordea Bank");
  CHECK(asReal(*said(certificates.listings[0], "management-fee")) == 0.0);  // zero is a fact; none is not
}

TEST(what_a_row_says_reaches_the_instrument_and_a_reload_changes_nothing) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  quick().load(*store, http, listener);
  auto s = store->snapshot();
  const NodeRef xaix = *s->findInstrumentByIsin("IE00BGV5VN51");
  CHECK(s->attribute(xaix, "tradable") && s->attribute(xaix, "category") && !s->attribute(xaix, "pe"));  // an ETF has no P/E
  const NodeRef gold = *s->findInstrumentByIsin("DE000A0S9GB0");
  CHECK(asInteger(*s->attribute(gold, "leverage")) == 1);

  // Loading again says nothing new: only today's bars are written again.
  const size_t said = s->attributes(xaix).size();
  FakeHttp again;
  again.answer = pages;
  quick().load(*store, again, listener);
  CHECK(store->snapshot()->attributes(xaix).size() == said);
}

TEST(isins_are_checked_by_their_check_digit) {
  for (const char * isin : {"DE0008469008", "US78378X1072", "RU000A0JP7K5", "XC0009655157", "FI0009000681", "US12573F1084", "XC0005705584", "XC0005705527"})
    CHECK(isValidIsin(isin));
  CHECK(!isValidIsin("DE0008469009") && !isValidIsin("DE000846900") && !isValidIsin(""));
}

TEST(nordnet_types_are_mapped_onto_the_one_vocabulary_and_certificates_are_named) {
  CHECK(NordnetSource::kindOfType("UETF") == InstrumentKind::Etf && NordnetSource::kindOfType("KETF") == InstrumentKind::Etf);
  CHECK(NordnetSource::kindOfType("PTC") == InstrumentKind::Certificate);
  CHECK(NordnetSource::kindOfType("ESH") == InstrumentKind::Company && NordnetSource::kindOfType("PESH") == InstrumentKind::Company);
  CHECK(NordnetSource::kindOfType("ESHMTF") == InstrumentKind::Company && NordnetSource::kindOfType("ESHOTC") == InstrumentKind::Company);
  CHECK(NordnetSource::kindOfType("FUCITS") == InstrumentKind::Fund && NordnetSource::kindOfType("FOTH") == InstrumentKind::Fund);
  CHECK(NordnetSource::kindOfType("") == InstrumentKind::Other && NordnetSource::kindOfType("SOMETHING_NEW") == InstrumentKind::Other);
}

TEST(search_rows_give_the_instrument_the_id_the_news_refers_to_it_by) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  quick().load(*store, http, listener);
  auto s = store->snapshot();
  const InstrumentId flxk = *s->findInstrumentByIsin("IE00BHZRR030");
  CHECK(asText(*s->attribute(flxk, "nordnet-instrument")) == "e9cab3b7-681a-4676-9790-6cdcdd9e7527");
  // And it finds the instrument by that id.
  CHECK(s->findByAttribute(NodeType::Instrument, "nordnet-instrument", "e9cab3b7-681a-4676-9790-6cdcdd9e7527")->id == flxk.value);
}
