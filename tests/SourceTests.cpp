#include "TestFramework.h"

#include "FakeHttp.h"
#include "Synthetic.h"
#include "source/SourceRunner.h"
#include "source/YahooSource.h"

#include <atomic>
#include <fstream>
#include <functional>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/yahoo/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// A small universe of its own, so the tests do not depend on what the lists on disk hold.
const Universe & testUniverse() {
  static const Universe u = {
    {"Nasdaq Helsinki", "NOKIA", "EUR", "Nokia", "NOKIA.HE", InstrumentKind::Company},
    {"Nasdaq Helsinki", "FORTUM", "EUR", "Fortum", "FORTUM.HE", InstrumentKind::Company},
    {"Nasdaq Helsinki", "SAMPO", "EUR", "Sampo", "SAMPO.HE", InstrumentKind::Company},
    {"Commodities", "XAU", "USD", "Gold", "GC=F", InstrumentKind::Commodity},
    {"Commodities", "XAG", "USD", "Silver", "SI=F", InstrumentKind::Commodity},
    {"Currencies", "EUR", "USD", "Euro", "EURUSD=X", InstrumentKind::Currency},
  };
  return u;
}

const UniverseEntry & entry(const std::string & yahoo) {
  for (const UniverseEntry & e : testUniverse())
    if (yahoo == e.yahoo) return e;
  static const UniverseEntry none{};
  return none;
}

EdgeKey edgeOf(const Snapshot & s, const char * base, const char * quote, const char * market, const char * dataset) {
  return {s.findInstrument(base)->id, s.findInstrument(quote)->id, s.findMarket(market)->id, s.findDataset(dataset)->id};
}

}  // namespace

TEST(a_yahoo_chart_becomes_candles_on_the_edge_of_its_market) {
  auto store = makeMemoryStore();
  std::string error;
  CHECK(YahooSource::parse(fixture("nokia.json"), entry("NOKIA.HE"), *store, error));
  auto s = store->snapshot();
  EdgeKey k = edgeOf(*s, "NOKIA", "EUR", "Nasdaq Helsinki", "yahoo-1d");
  CandleSeries c = s->candles(k);
  CHECK(c.intervalMs == 86400000 && c.candles.size() > 400);
  bool ordered = true, sane = true;
  for (size_t i = 0; i < c.candles.size(); i++) {
    if (i && c.candles[i].time <= c.candles[i - 1].time) ordered = false;
    const Candle & b = c.candles[i];
    if (!(b.low <= b.high && b.low <= b.close && b.close <= b.high && b.volume >= 0)) sane = false;
  }
  CHECK(ordered && sane);
  CHECK(s->instrument(k.base)->kind == InstrumentKind::Company && s->instrument(k.base)->name == "Nokia");
  CHECK(s->edges(k.market).size() == 1);
}

TEST(yahoo_dividends_become_corporate_actions_in_the_quote_currency) {
  auto store = makeMemoryStore();
  std::string error;
  CHECK(YahooSource::parse(fixture("nokia.json"), entry("NOKIA.HE"), *store, error));
  auto s = store->snapshot();
  EventKey k{s->findInstrument("NOKIA")->id, s->findDataset("yahoo-1d")->id};
  auto actions = s->actions(k);
  CHECK(actions.size() >= 2);
  for (const CorporateAction & a : actions) {
    CHECK(a.kind == ActionKind::Dividend && a.value > 0 && a.currency == s->findInstrument("EUR")->id);
  }
  CHECK(actions[0].time < actions[1].time);
  CHECK(s->events(k.instrument).size() == 1);
}

TEST(a_yahoo_split_is_new_shares_per_old) {
  const char * body = R"({"chart":{"result":[{"timestamp":[1700000000,1700086400],"events":{"splits":{"1700086400":
      {"date":1700086400,"numerator":2,"denominator":1,"splitRatio":"2:1"}}},"indicators":{"quote":[{
      "open":[1,2],"high":[1,2],"low":[1,2],"close":[1,2],"volume":[5,6]}]}}],"error":null}})";
  auto store = makeMemoryStore();
  std::string error;
  CHECK(YahooSource::parse(body, entry("NOKIA.HE"), *store, error));
  auto s = store->snapshot();
  auto actions = s->actions({s->findInstrument("NOKIA")->id, s->findDataset("yahoo-1d")->id});
  CHECK(actions.size() == 1 && actions[0].kind == ActionKind::Split && actions[0].value == 2.0 && actions[0].note == "2:1");
  CHECK(actions[0].time == 1700086400000);
}

TEST(days_yahoo_has_no_price_for_are_skipped) {
  auto store = makeMemoryStore();
  std::string error;
  CHECK(YahooSource::parse(fixture("gold.json"), entry("GC=F"), *store, error));
  auto s = store->snapshot();
  CandleSeries c = s->candles(edgeOf(*s, "XAU", "USD", "Commodities", "yahoo-1d"));
  CHECK(c.candles.size() == 504);  // 506 days, two of them null
  CHECK(s->instrument(s->findInstrument("XAU")->id)->kind == InstrumentKind::Commodity);
}

TEST(a_currency_pair_is_an_edge_between_two_currencies) {
  auto store = makeMemoryStore();
  std::string error;
  CHECK(YahooSource::parse(fixture("eurusd.json"), entry("EURUSD=X"), *store, error));
  auto s = store->snapshot();
  EdgeKey k = edgeOf(*s, "EUR", "USD", "Currencies", "yahoo-1d");
  CHECK(s->lastCandles(k, 1).candles.size() == 1);
  CHECK(s->instrument(k.base)->kind == InstrumentKind::Currency && s->instrument(k.quote)->name == "US dollar");
}

TEST(a_response_that_holds_no_data_is_refused_and_changes_nothing) {
  const char * bad[] = {
    "", "not json", "{}", R"({"chart":{"result":null,"error":{"code":"Not Found","description":"No data found"}}})",
    R"({"chart":{"result":[],"error":null}})",
    R"({"chart":{"result":[{"timestamp":[1],"indicators":{"quote":[{"open":[null],"high":[null],"low":[null],"close":[null]}]}}]}})"};
  for (const char * body : bad) {
    auto store = makeMemoryStore();
    std::string error;
    CHECK(!YahooSource::parse(body, entry("NOKIA.HE"), *store, error));
    CHECK(!error.empty());
    CHECK(store->snapshot()->version() == 0);
  }
  auto store = makeMemoryStore();
  std::string error;
  CHECK(!YahooSource::parse(R"({"chart":{"result":null,"error":{"description":"No data found"}}})", entry("NOKIA.HE"), *store, error));
  CHECK(error == "No data found");
}

TEST(loading_asks_for_every_instrument_and_reports_the_ones_that_fail) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("/NOKIA.HE?") != std::string::npos) return ok(fixture("nokia.json"));
    if (url.find("/GC=F?") != std::string::npos) return ok(fixture("gold.json"));
    if (url.find("/EURUSD=X?") != std::string::npos) return ok(fixture("eurusd.json"));
    if (url.find("/FORTUM.HE?") != std::string::npos) return ok("garbage");
    return failed(404);
  };
  Listener listener;
  YahooSource yahoo(testUniverse(), "1y", std::chrono::milliseconds(0));
  yahoo.load(*store, http, listener);
  CHECK(listener.changes == 3);
  CHECK(listener.messages.size() == testUniverse().size() - 3);
  bool fortum = false, notFound = false;
  for (const std::string & m : listener.messages) {
    fortum = fortum || m == "yahoo: FORTUM.HE: not a chart response";
    notFound = notFound || m == "yahoo: SAMPO.HE: HTTP 404";
  }
  CHECK(fortum && notFound);
  CHECK(http.urls.front().find("https://query1.finance.yahoo.com/v8/finance/chart/") == 0);
  CHECK(http.urls.front().find("range=1y&interval=1d&events=div%7Csplit") != std::string::npos);
  CHECK(store->snapshot()->edges(store->snapshot()->findMarket("Nasdaq Helsinki")->id).size() == 1);
}

TEST(loading_tries_the_second_host_when_the_first_does_not_answer) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("query1") != std::string::npos) return failed(0);
    return url.find("/NOKIA.HE?") != std::string::npos ? ok(fixture("nokia.json")) : failed(404);
  };
  Listener listener;
  YahooSource(testUniverse(), "1y", std::chrono::milliseconds(0)).load(*store, http, listener);
  CHECK(listener.changes == 1);
  bool second = false;
  for (const std::string & u : http.urls) second = second || (u.find("query2") != std::string::npos && u.find("NOKIA.HE") != std::string::npos);
  CHECK(second);
}

TEST(a_rate_limit_stops_the_load) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(429); };
  Listener listener;
  YahooSource(testUniverse(), "1y", std::chrono::milliseconds(0)).load(*store, http, listener);
  CHECK(http.urls.size() == 1 && listener.messages.size() == 1 && listener.changes == 0);
}

TEST(a_cancelled_load_stops_between_requests) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };
  struct Cancelling : Listener {
    bool cancelled() const override { return !messages.empty(); }
  } listener;
  YahooSource(testUniverse(), "1y", std::chrono::milliseconds(0)).load(*store, http, listener);
  CHECK(http.urls.size() == 2 && listener.messages.size() == 1);  // both hosts for the first instrument, then stop
}

TEST(the_runner_runs_the_sources_on_its_own_thread_and_reports_changes) {
  std::shared_ptr<Store> store = makeMemoryStore();
  std::atomic<int> changes{0};
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::make_unique<SyntheticSource>(testUniverse()));
  SourceRunner runner(store, std::move(sources), [&] { changes++; }, [](std::string) {});
  runner.wait();
  CHECK(changes == 1);
  CHECK(store->snapshot()->edges(store->snapshot()->findMarket("Currencies")->id).size() == 1);
}
