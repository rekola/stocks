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

// The endpoint as the fixtures hold it: five rows, three then two, with a total of five.
HttpResult pages(const std::string & url) {
  return ok(fixture(url.find("offset=3") != std::string::npos ? "etflist-2.json" : "etflist-1.json"));
}

bool hasHeader(const HttpHeaders & headers, const std::string & name, const std::string & value) {
  for (const auto & [n, v] : headers)
    if (n == name && v == value) return true;
  return false;
}

NordnetSource quick() { return NordnetSource(500, std::chrono::milliseconds(0)); }

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

TEST(a_market_is_named_by_the_exchange_code_in_the_slug) {
  NordnetListing l;
  l.marketId = 4;
  l.slug = "franklin-ftse-korea-ucits-flxk-xeta";
  CHECK(NordnetSource::marketName(l) == "Xetra");
  l.slug = "x-xsto";
  CHECK(NordnetSource::marketName(l) == "Nasdaq Stockholm");
  l.slug = "x-xcse";
  CHECK(NordnetSource::marketName(l) == "Nasdaq Copenhagen");
  l.slug = "x-xhel";
  CHECK(NordnetSource::marketName(l) == "Nasdaq Helsinki");  // the same market as the Yahoo lists name
  l.slug = "x-xzzz";
  CHECK(NordnetSource::marketName(l) == "Nordnet market 4");  // an unknown code: the id
  l.slug = "";
  CHECK(NordnetSource::marketName(l) == "Nordnet market 4");
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

TEST(loading_pages_through_the_list_and_stores_instruments_markets_and_edges) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = pages;
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);

  // Two requests: the first with no offset, the second where the first ended.
  CHECK(http.urls.size() == 2);
  CHECK(http.urls[0].find("https://www.nordnet.fi/api/2/instrument_search/query/etflist?") == 0);
  CHECK(http.urls[0].find("limit=500") != std::string::npos && http.urls[0].find("offset") == std::string::npos);
  CHECK(http.urls[0].find("sort_attribute=symbol") != std::string::npos);  // a key that does not move between pages
  CHECK(http.urls[1].find("offset=3") != std::string::npos);
  CHECK(listener.changes == 2);
  CHECK(listener.messages.size() == 1 && listener.messages[0] == "nordnet: 5 ETFs");
  CHECK(source.listings().size() == 5);

  auto s = store->snapshot();
  // Four ETFs (one of them listed on two markets) and the three quote currencies.
  CHECK(s->instruments().size() == 7);
  Instrument xaix = *s->findInstrumentByIsin("IE00BGV5VN51");
  CHECK(xaix.symbol == "XAIX" && xaix.kind == InstrumentKind::Etf && xaix.isin == "IE00BGV5VN51");
  CHECK(s->findInstrument("EUR")->kind == InstrumentKind::Currency && s->findInstrument("EUR")->name == "Euro");
  CHECK(s->markets().size() == 3);
  MarketId xetra = s->findMarket("Xetra")->id, stockholm = s->findMarket("Nasdaq Stockholm")->id;
  CHECK(s->findMarket("Nasdaq Copenhagen"));
  CHECK(s->edges(xetra).size() == 2 && s->edges(stockholm).size() == 2);  // FLXK and XAIX; XACT BULL and XAIX
  DatasetId nordnet = s->findDataset("nordnet-1d")->id;
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
  CHECK(store->snapshot()->instruments().size() == 7 && source.listings().size() == 5);
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

TEST(a_refusal_stops_the_load_and_says_why) {
  for (int status : {403, 429}) {
    auto store = makeMemoryStore();
    FakeHttp http;
    http.answer = [status](const std::string &) { return failed(status); };
    Listener listener;
    quick().load(*store, http, listener);
    CHECK(http.urls.size() == 1);
    CHECK(listener.messages.size() == 1 && listener.messages[0] == "nordnet: HTTP " + std::to_string(status) + ", stopping");
    CHECK(store->snapshot()->version() == 0);
  }
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(0); };
  Listener listener;
  quick().load(*store, http, listener);
  CHECK(listener.messages.size() == 1 && listener.messages[0] == "nordnet: no answer");
}

TEST(an_answer_that_is_not_a_list_stops_the_load_and_keeps_what_came_before) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    return url.find("offset=3") != std::string::npos ? ok("<html>") : ok(fixture("etflist-1.json"));
  };
  Listener listener;
  NordnetSource source = quick();
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 2);
  CHECK(listener.messages.size() == 2 && listener.messages[0] == "nordnet: not an instrument list" && listener.messages[1] == "nordnet: 3 ETFs");
  CHECK(store->snapshot()->instruments().size() == 6);  // three ETFs and three currencies
}

TEST(an_empty_page_ends_the_load_even_if_the_total_says_more) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    return url.find("offset") != std::string::npos ? ok(R"({"rows":0,"total_hits":999,"results":[]})") : ok(fixture("etflist-1.json"));
  };
  Listener listener;
  quick().load(*store, http, listener);
  CHECK(http.urls.size() == 2);
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
