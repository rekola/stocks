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

TEST(an_instrument_is_an_etf_by_its_own_type_and_anything_else_is_other) {
  CHECK(NordnetSource::kindOf("UETF") == InstrumentKind::Etf && NordnetSource::kindOf("KETF") == InstrumentKind::Etf);
  CHECK(NordnetSource::kindOf("PTC") == InstrumentKind::Other && NordnetSource::kindOf("") == InstrumentKind::Other);
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
  CHECK(s->findInstrument("4GLD")->kind == InstrumentKind::Other);  // a certificate: no category of its own
  CHECK(s->findInstrument("EUR")->kind == InstrumentKind::Currency && s->findInstrument("EUR")->name == "Euro");
  CHECK(s->findInstrument("NOK")->name == "Norwegian krone");
  CHECK(s->markets().size() == 6);
  MarketId xetra = s->findMarket("Xetra")->id, stockholm = s->findMarket("Nasdaq Stockholm")->id;
  CHECK(s->findMarket("Nasdaq Copenhagen") && s->findMarket("First North Sweden") && s->findMarket("First North Finland"));
  CHECK(s->findMarket("Nordnet market 61"));
  CHECK(s->edges(xetra).size() == 3 && s->edges(stockholm).size() == 2);  // FLXK, XAIX, 4GLD; XACT BULL, XAIX
  CHECK(s->edges(s->findMarket("First North Sweden")->id).size() == 1);   // the certificate whose slug says Oslo
  DatasetId nordnet = s->findDataset("nordnet")->id;
  EdgeKey onXetra{xaix.id, s->findInstrument("EUR")->id, xetra, nordnet};
  EdgeKey onStockholm{xaix.id, s->findInstrument("SEK")->id, stockholm, nordnet};
  CHECK(s->candles(onXetra).candles.empty());  // discovery only: no bars yet
  bool a = false, b = false;
  for (const EdgeKey & k : s->edges(xetra)) a = a || k == onXetra;
  for (const EdgeKey & k : s->edges(stockholm)) b = b || k == onStockholm;
  CHECK(a && b);  // one instrument, two edges, each in its own currency
}

TEST(loading_twice_changes_nothing_but_the_first_time) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);
  uint64_t version = store->snapshot()->version();
  source.load(*store, http, listener);
  CHECK(store->snapshot()->version() == version);
  CHECK(store->snapshot()->instruments().size() == 12 && source.listings().size() == 9);
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
