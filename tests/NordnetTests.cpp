#include "TestFramework.h"

#include "FakeHttp.h"
#include "source/NordnetSource.h"

#include <fstream>
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

NordnetSource quick() { return NordnetSource(NordnetSource::defaultLists(), std::chrono::milliseconds(0)); }

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

TEST(a_market_is_named_by_its_nordnet_id_where_that_is_known) {
  NordnetListing l;
  l.slug = "franklin-ftse-korea-ucits-flxk-xeta";
  l.marketId = 4;
  CHECK(NordnetSource::marketName(l) == "Xetra");
  l.marketId = 11;
  CHECK(NordnetSource::marketName(l) == "Nasdaq Stockholm");
  l.marketId = 14;
  CHECK(NordnetSource::marketName(l) == "Nasdaq Copenhagen");
  l.marketId = 24;
  CHECK(NordnetSource::marketName(l) == "Nasdaq Helsinki");  // the same market as the Yahoo lists name
  l.marketId = 53;
  CHECK(NordnetSource::marketName(l) == "First North Sweden");
  l.slug = "bull-omxs30-x1-nn-onse";  // the code of its home exchange, on a market that is another
  CHECK(NordnetSource::marketName(l) == "First North Sweden");
  l.marketId = 61;
  CHECK(NordnetSource::marketName(l) == "Nordnet market 61");  // not known: the id
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
    {"instrument_info":{"instrument_id":1,"symbol":"A","isin":"X1","currency":"EUR","name":"A fund"},"market_info":{"market_id":4,"identifier":"A"}},
    {"instrument_info":{"instrument_id":2,"symbol":"B","currency":"EUR"},"market_info":{"market_id":4}},
    {"instrument_info":{"instrument_id":3,"symbol":"C","isin":"X3","currency":"EUR"}}]})";
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
  CHECK(listener.messages.size() == 1 && listener.messages[0] == "nordnet: 9 listings (etflist 5, certificatelist 4)");
  CHECK(source.listings().size() == 9 && source.listings()[0].list == "etflist" && source.listings()[8].list == "certificatelist");

  auto s = store->snapshot();
  // Eight instruments (one ETF is listed on two markets) and the four quote currencies.
  CHECK(s->instruments().size() == 12);
  Instrument xaix = *s->findInstrumentByIsin("IE00BGV5VN51");
  CHECK(xaix.symbol == "XAIX" && xaix.kind == InstrumentKind::Etf && xaix.isin == "IE00BGV5VN51");
  CHECK(s->findInstrument("4GLD")->kind == InstrumentKind::Certificate);  // a certificate, whoever lists it
  CHECK(s->findInstrument("EUR")->kind == InstrumentKind::Currency && s->findInstrument("EUR")->name == "Euro");
  CHECK(s->findInstrument("NOK")->name == "Norwegian krone");
  CHECK(s->markets().size() == 6);
  MarketId xetra = s->findMarket("Xetra")->id, stockholm = s->findMarket("Nasdaq Stockholm")->id;
  CHECK(s->findMarket("Nasdaq Copenhagen") && s->findMarket("First North Sweden") && s->findMarket("First North Finland"));
  CHECK(s->findMarket("Nordnet market 61"));
  CHECK(s->edges(xetra).size() == 3 && s->edges(stockholm).size() == 2);  // FLXK, XAIX, 4GLD; XACT BULL, XAIX
  // The certificate on First North Sweden has no tick time, so no bar and no edge: a node nothing selects.
  CHECK(s->findInstrument("BULL OMXS30 X1 NN") && s->edges(s->findMarket("First North Sweden")->id).empty());
  DatasetId nordnet = s->findDataset("nordnet")->id;
  EdgeKey onXetra{xaix.id, s->findInstrument("EUR")->id, xetra, nordnet};
  EdgeKey onStockholm{xaix.id, s->findInstrument("SEK")->id, stockholm, nordnet};
  // Each listing's price block is today's bar, in the daily series; daily bars start at midnight UTC.
  CandleSeries today = s->candles(onXetra, interval::day);
  CHECK(today.intervalMs == 86400000 && today.candles.size() == 1);
  CHECK(today.candles[0].time == 1795132800000 && today.candles[0].open == 188.94 && today.candles[0].high == 189.56);
  CHECK(today.candles[0].low == 184.42 && today.candles[0].close == 184.42 && today.candles[0].volume == 75161);
  bool a = false, b = false;
  for (const EdgeKey & k : s->edges(xetra)) a = a || k == onXetra;
  for (const EdgeKey & k : s->edges(stockholm)) b = b || k == onStockholm;
  CHECK(a && b);  // one instrument, two edges, each in its own currency
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
  CHECK(second->instruments().size() == first->instruments().size() && second->instruments().size() == 12);
  CHECK(source.listings().size() == 9);
  MarketId xetra = second->findMarket("Xetra")->id;
  CHECK(second->edges(xetra) == first->edges(xetra));
  for (const EdgeKey & k : second->edges(xetra)) CHECK(second->candles(k, interval::day).candles.size() == 1);
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
  CHECK(listener.messages.size() == 2 && listener.messages[0] == "nordnet: etflist: HTTP 500");
  CHECK(listener.messages[1] == "nordnet: 4 listings (certificatelist 4)");
  CHECK(source.listings().size() == 4);

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
  CHECK(listener.messages.size() == 2 && listener.messages[0] == "nordnet: etflist: not an instrument list");
  CHECK(listener.messages[1] == "nordnet: 7 listings (etflist 3, certificatelist 4)");
  CHECK(store->snapshot()->instruments().size() == 3 + 3 + 4 + 1);  // three ETFs, four certificates and their currencies
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
  Instrument xaix = *s->findInstrumentByIsin("IE00BGV5VN51");
  EdgeKey onXetra{xaix.id, s->findInstrument("EUR")->id, s->findMarket("Xetra")->id, s->findDataset("nordnet")->id};
  CandleSeries c = s->candles(onXetra, interval::day);
  CHECK(c.candles.size() == 2 && c.candles[0].time == 1795132800000 && c.candles[1].time == 1795219200000);
}

TEST(a_listing_without_a_price_is_a_node_and_has_no_edge) {
  const char * body = R"({"rows":3,"total_hits":3,"results":[
    {"instrument_info":{"instrument_id":1,"symbol":"A","isin":"X1","currency":"EUR","name":"Traded","instrument_type":"UETF"},
     "market_info":{"market_id":4,"identifier":"A"},
     "price_info":{"last":{"price":10.0},"open":{"price":9.5},"high":{"price":10.5},"low":{"price":9.0},"tick_timestamp":1795192200000,"turnover_volume":7}},
    {"instrument_info":{"instrument_id":2,"symbol":"B","isin":"X2","currency":"EUR","name":"Not traded","instrument_type":"UETF"},
     "market_info":{"market_id":4,"identifier":"B"},"price_info":{}},
    {"instrument_info":{"instrument_id":3,"symbol":"C","isin":"X3","currency":"EUR","name":"No open","instrument_type":"UETF"},
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
  CHECK(s->findInstrument("A") && s->findInstrument("B") && s->findInstrument("C"));  // all three are nodes
  CHECK(s->edges(s->findMarket("Xetra")->id).size() == 1);                             // only the traded one is seen
  CHECK(s->edges(s->findMarket("Xetra")->id)[0].base == s->findInstrument("A")->id);
}

TEST(an_instrument_keeps_the_type_nordnet_gives_it_and_edges_can_be_selected_by_it) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  quick().load(*store, http, listener);
  auto s = store->snapshot();
  CHECK(s->findInstrument("FLXK")->type == "UETF" && s->findInstrument("XACT BULL")->type == "KETF");
  CHECK(s->findInstrument("4GLD")->type == "PTC" && s->findInstrument("EUR")->type.empty());
  MarketId xetra = s->findMarket("Xetra")->id;
  auto certificates = s->edges(xetra, [](const Instrument & i) { return i.type == "PTC"; });
  auto funds = s->edges(xetra, [](const Instrument & i) { return i.kind == InstrumentKind::Etf; });
  auto byKind = s->edges(xetra, [](const Instrument & i) { return i.kind == InstrumentKind::Certificate; });
  CHECK(byKind == certificates);  // the type and the combined kind agree here
  CHECK(certificates.size() == 1 && certificates[0].base == s->findInstrument("4GLD")->id);
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
