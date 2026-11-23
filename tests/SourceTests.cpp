#include "TestFramework.h"

#include "FakeHttp.h"
#include "Synthetic.h"
#include "model/Dates.h"
#include "source/AutoSource.h"
#include "source/FundSource.h"
#include "source/NordnetSource.h"
#include "source/SourceRunner.h"
#include "source/YahooSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <set>
#include <filesystem>
#include <thread>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/yahoo/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// A small list of its own, so the tests do not depend on what the lists on disk hold.
const InstrumentList & testList() {
  static const InstrumentList u = {
      {"Nasdaq Helsinki", "NOKIA", "EUR", "Nokia", "NOKIA.HE",
       InstrumentKind::Company, "FI0009000681", "XHEL"},
      {"Nasdaq Helsinki", "FORTUM", "EUR", "Fortum", "FORTUM.HE",
       InstrumentKind::Company, "FI0009007132", "XHEL"},
      {"Nasdaq Helsinki", "SAMPO", "EUR", "Sampo", "SAMPO.HE",
       InstrumentKind::Company, "FI4000552500", "XHEL"},
      {"Commodities", "XAU", "USD", "Gold", "GC=F", InstrumentKind::Commodity,
       "XC0009655157"},
      {"Commodities", "XAG", "USD", "Silver", "SI=F", InstrumentKind::Commodity,
       "XC0009653103"},
      {"Currencies", "EUR", "USD", "Euro", "EURUSD=X", InstrumentKind::Currency,
       ""},
  };
  return u;
}

const ListEntry & entry(const std::string & yahoo) {
  for (const ListEntry & e : testList())
    if (yahoo == e.yahoo) return e;
  static const ListEntry none{};
  return none;
}

// An edge by what is in the list: an instrument's ISIN, or a currency's code.
EdgeKey edgeOf(const Snapshot & s, const char * base, const char * quote, const char * market, const char * dataset) {
  auto b = s.findInstrumentByIsin(base);
  return {b ? *b : *s.findInstrument(base), *s.findInstrument(quote), *s.findMarket(market), *s.findDataset(dataset)};
}

constexpr const char * kNokia = "FI0009000681", * kGold = "XC0009655157";

}  // namespace

TEST(a_yahoo_chart_becomes_candles_on_the_edge_of_its_market) {
  auto store = makeMemoryStore();
    std::string error;
  CHECK(YahooSource::parse(fixture("nokia.json"), entry("NOKIA.HE"), *store, error));
  auto s = store->snapshot();
  EdgeKey k = edgeOf(*s, kNokia, "EUR", "XHEL", "yahoo");
  CandleSeries c = s->candles(k, Resolution::Day);
  CHECK(c.resolution == Resolution::Day && c.candles.size() > 400);
  // Keyed by trading date: the exchange's date of the first bar, not an instant.
  CHECK(c.candles[0].time == 739210 && c.candles[c.candles.size() - 1].time == 739940);
  bool ordered = true, sane = true;
  for (size_t i = 0; i < c.candles.size(); i++) {
    if (i && c.candles[i].time <= c.candles[i - 1].time) ordered = false;
    const Candle & b = c.candles[i];
    if (!(b.low <= b.high && b.low <= b.close && b.close <= b.high && b.volume >= 0)) sane = false;
  }
  CHECK(ordered && sane);
  CHECK(s->kind(k.base) == InstrumentKind::Company && s->name(k.base) == "Nokia");
  CHECK(s->edges(k.market).size() == 1);
  // Its own edge has what Yahoo calls it and its own type; the instrument is the one the other source made.
  CHECK(s->symbol(k) == "NOKIA.HE");
}

TEST(a_row_goes_to_the_market_of_its_code) {
  auto store = makeMemoryStore();
  MarketId helsinki = store->addMarket(
      "XHEL",
      "Helsinki (other name)"); // another provider made it, and named it
  ListEntry e = entry("NOKIA.HE");
  e.market = "Nasdaq Helsinki";
  e.mic = "XHEL";
  std::string error;
  CHECK(YahooSource::parse(fixture("nokia.json"), e, *store, error));
  auto s = store->snapshot();
  CHECK(*s->findMarket("XHEL") ==
        helsinki); // the same market, no second Helsinki (the last name said
                   // stands)
  CHECK(marketCode(helsinki) == "XHEL");
  auto edges = s->edges(helsinki);
  CHECK(edges.size() == 1);
}

TEST(yahoo_dividends_become_corporate_actions_in_the_quote_currency) {
  auto store = makeMemoryStore();
    std::string error;
  CHECK(YahooSource::parse(fixture("nokia.json"), entry("NOKIA.HE"), *store, error));
  auto s = store->snapshot();
  EventKey k{*s->findInstrumentByIsin(kNokia), *s->findDataset("yahoo")};
  auto actions = s->actions(k);
  CHECK(actions.size() >= 2);
  for (const CorporateAction & a : actions) {
    CHECK(a.kind == ActionKind::Dividend && a.value > 0 && a.currency == *s->findInstrument("EUR"));
  }
  CHECK(actions[0].time < actions[1].time);
  CHECK(s->events(k.instrument).size() == 1);
}

TEST(a_yahoo_split_is_new_shares_per_old) {
  const char * body = R"({"chart":{"result":[{"timestamp":[1700000000,1700086400],"events":{"splits":{"1700086400":
      {"date":1700006400,"numerator":2,"denominator":1,"splitRatio":"2:1"}}},"indicators":{"quote":[{
      "open":[1,2],"high":[1,2],"low":[1,2],"close":[1,2],"volume":[5,6]}]}}],"error":null}})";
  auto store = makeMemoryStore();
    std::string error;
  CHECK(YahooSource::parse(body, entry("NOKIA.HE"), *store, error));
  auto s = store->snapshot();
  auto actions = s->actions({*s->findInstrumentByIsin(kNokia), *s->findDataset("yahoo")});
  CHECK(actions.size() == 1 && actions[0].kind == ActionKind::Split && actions[0].value == 2.0 && actions[0].note == "2:1");
  CHECK(actions[0].time == dayNumber(2023, 11, 15));  // an ex-date is a date, not an instant
}

TEST(days_yahoo_has_no_price_for_are_skipped) {
  auto store = makeMemoryStore();
    std::string error;
  CHECK(YahooSource::parse(fixture("gold.json"), entry("GC=F"), *store, error));
  auto s = store->snapshot();
  CandleSeries c =
      s->candles(edgeOf(*s, kGold, "USD", "XXXX", "yahoo"), Resolution::Day);
  CHECK(c.candles.size() == 504);  // 506 days, two of them null
  CHECK(s->kind(*s->findInstrumentByIsin(kGold)) == InstrumentKind::Commodity);
}

TEST(a_currency_pair_is_an_edge_between_two_currencies) {
  auto store = makeMemoryStore();
  std::string error;
  CHECK(YahooSource::parse(fixture("eurusd.json"), entry("EURUSD=X"), *store, error));
  auto s = store->snapshot();
  EdgeKey k = edgeOf(*s, "EUR", "USD", "XXXX", "yahoo");
  CHECK(s->lastCandles(k, Resolution::Day, 1).candles.size() == 1);
  CHECK(s->kind(k.base) == InstrumentKind::Currency && s->name(k.quote) == "US dollar");
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
  YahooSource yahoo(testList(), "1y", std::chrono::milliseconds(0));
  yahoo.load(*store, http, listener);
  CHECK(listener.changes == 3);
  CHECK(listener.messages.size() == testList().size() - 3);
  bool fortum = false, notFound = false;
  for (const std::string & m : listener.messages) {
    fortum = fortum || m == "yahoo: FORTUM.HE: not a chart response";
    notFound = notFound || m == "yahoo: SAMPO.HE: HTTP 404";
  }
  CHECK(fortum && notFound);
  CHECK(http.urls.front().find("https://query1.finance.yahoo.com/v8/finance/chart/") == 0);
  CHECK(http.urls.front().find("range=1y&interval=1d&events=div%7Csplit") != std::string::npos);
  CHECK(
      store->snapshot()->edges(*store->snapshot()->findMarket("XHEL")).size() ==
      1);
}

TEST(loading_tries_the_second_host_when_the_first_does_not_answer) {
  auto store = makeMemoryStore();
    FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("query1") != std::string::npos) return failed(0);
    return url.find("/NOKIA.HE?") != std::string::npos ? ok(fixture("nokia.json")) : failed(404);
  };
  Listener listener;
  YahooSource(testList(), "1y", std::chrono::milliseconds(0)).load(*store, http, listener);
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
  YahooSource(testList(), "1y", std::chrono::milliseconds(0)).load(*store, http, listener);
  CHECK(http.urls.size() == 1 && listener.messages.size() == 1 && listener.changes == 0);
}

TEST(a_cancelled_load_stops_between_requests) {
  auto store = makeMemoryStore();
    FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };
  struct Cancelling : Listener {
    bool cancelled() const override { return !messages.empty(); }
  } listener;
  YahooSource(testList(), "1y", std::chrono::milliseconds(0)).load(*store, http, listener);
  CHECK(http.urls.size() == 2 && listener.messages.size() == 1);  // both hosts for the first instrument, then stop
}

TEST(the_runner_runs_the_sources_on_its_own_thread_and_reports_changes) {
  std::shared_ptr<Store> store = makeMemoryStore();
  std::atomic<int> changes{0};
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::make_unique<SyntheticSource>(testList()));
  SourceRunner runner(store, std::move(sources), [&] { changes++; }, [](std::string) {});
  runner.wait();
  CHECK(changes == 1);
  CHECK(
      store->snapshot()->edges(*store->snapshot()->findMarket("XXXX")).size() ==
      3); // the gold, the silver and the euro
}

TEST(the_sources_say_so_when_their_lists_are_missing_and_do_nothing) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };
  Listener a, b;
  YahooSource(ListSpec{"/no/such/directory", {}}).load(*store, http, a);
  SyntheticSource(ListSpec{"/no/such/directory", {}}).load(*store, http, b);
  CHECK(http.urls.empty() && store->snapshot()->version() == 0);
  CHECK(a.messages.size() == 1 && a.messages[0].find("yahoo: ") == 0);
  CHECK(b.messages.size() == 1 && b.messages[0].find("synthetic: ") == 0);
}

TEST(the_sources_read_the_lists_on_disk_when_they_load) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };
  Listener listener;
  YahooSource(ListSpec{"", {"XXXX"}}, "1y", std::chrono::milliseconds(0))
      .load(*store, http, listener);
  CHECK(http.urls.size() >= 80 && http.urls[0].find("=X") != std::string::npos);  // the currency pairs, one request each
  auto store2 = makeMemoryStore();
  Listener l2;
  SyntheticSource(ListSpec{"", {"XXXX"}}).load(*store2, http, l2);
  CHECK(l2.changes == 1 &&
        store2->snapshot()
                ->edges(*store2->snapshot()->findMarket("XXXX"))
                .size() >= 4);
  CHECK(!store2->snapshot()->findMarket(
      "XHEL")); // the other markets of the lists are not loaded
}

namespace {
// A source that counts its loads and asks to be loaded again.
class CountingSource : public Source {
 public:
  CountingSource(std::atomic<int> & loads, std::chrono::milliseconds interval) : loads_(loads), interval_(interval) {}
  std::string name() const override { return "counting"; }
  std::string description() const override { return ""; }
  void load(Store &, Http &, SourceListener & listener) override {
    loads_++;
    listener.changed();
  }
  std::chrono::milliseconds refreshInterval() const override { return interval_; }

 private:
  std::atomic<int> & loads_;
  std::chrono::milliseconds interval_;
};

bool waitFor(const std::function<bool()> & done) {
  for (int i = 0; i < 400 && !done(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  return done();
}
}  // namespace

TEST(a_source_that_asks_to_be_refreshed_is_loaded_again_and_again_until_the_runner_stops) {
  std::shared_ptr<Store> store = makeMemoryStore();
  std::atomic<int> loads{0}, once{0}, changes{0};
  int after = 0;
  {
    std::vector<std::unique_ptr<Source>> sources;
    sources.push_back(std::make_unique<CountingSource>(loads, std::chrono::milliseconds(10)));
    sources.push_back(std::make_unique<CountingSource>(once, std::chrono::milliseconds(0)));  // not again
    SourceRunner runner(store, std::move(sources), [&] { changes++; }, [](std::string) {});
    CHECK(waitFor([&] { return loads >= 4; }));
    CHECK(once == 1);  // loaded in the first pass only
    CHECK(changes >= 5);
    after = loads;
  }  // the destructor stops the thread, even though a source still wants to be loaded
  int stopped = loads;
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  CHECK(loads == stopped && stopped >= after);
}

TEST(a_runner_with_no_refreshing_source_finishes_by_itself) {
  std::shared_ptr<Store> store = makeMemoryStore();
  std::atomic<int> a{0}, b{0};
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::make_unique<CountingSource>(a, std::chrono::milliseconds(0)));
  sources.push_back(std::make_unique<CountingSource>(b, std::chrono::milliseconds(0)));
  SourceRunner runner(store, std::move(sources), [] {}, [](std::string) {});
  runner.wait();  // returns: nothing refreshes
  CHECK(a == 1 && b == 1);
}

namespace {
// A source that does not finish until another is loading too: it can only, if the runner loads them at once.
class MeetingSource : public Source {
 public:
  MeetingSource(std::atomic<int> & arrived, std::atomic<bool> & met) : arrived_(arrived), met_(met) {}
  std::string name() const override { return "meeting"; }
  std::string description() const override { return ""; }
  void load(Store &, Http &, SourceListener &) override {
    arrived_++;
    for (int i = 0; i < 400 && arrived_ < 2; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (arrived_ >= 2) met_ = true;
  }

 private:
  std::atomic<int> & arrived_;
  std::atomic<bool> & met_;
};
}  // namespace

TEST(the_runner_loads_its_sources_at_once) {
  std::shared_ptr<Store> store = makeMemoryStore();
  std::atomic<int> arrived{0};
  std::atomic<bool> first{false}, second{false};
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::make_unique<MeetingSource>(arrived, first));
  sources.push_back(std::make_unique<MeetingSource>(arrived, second));
  SourceRunner runner(store, std::move(sources), [] {}, [](std::string) {});
  runner.wait();
  CHECK(first && second);  // one after the other, the first would have waited for the second in vain
}

TEST(yahoo_makes_the_instrument_from_the_isin_in_its_list_and_the_same_node_as_another_source) {
  auto store = makeMemoryStore();
  // Nordnet made it first, by its ISIN, with its own edge.
  InstrumentId made = store->addInstrument("", "Nokia Oyj", InstrumentKind::Company, kNokia, "ESH");
  std::string error;
  CHECK(YahooSource::parse(fixture("nokia.json"), entry("NOKIA.HE"), *store, error));
  auto s = store->snapshot();
  EdgeKey k = edgeOf(*s, kNokia, "EUR", "XHEL", "yahoo");
  CHECK(k.base == made && s->nodeCount(NodeType::Instrument) == 2);  // the one instrument, and the euro
  CHECK(s->symbol(k) == "NOKIA.HE");                          // what Yahoo calls it
  // Where Yahoo's history begins is said of the instrument.
  auto first = s->attribute(k.base, predicate::firstTradeDay);
  CHECK(first && *asInteger(*first) >= dayNumber(2000, 1, 1) && *asInteger(*first) < dayNumber(2000, 1, 10));

  // Run first, Yahoo makes the instrument itself: the ISIN is in the data.
  auto alone = makeMemoryStore();
  CHECK(YahooSource::parse(fixture("nokia.json"), entry("NOKIA.HE"), *alone, error));
  CHECK(alone->snapshot()->name(*alone->snapshot()->findInstrumentByIsin(kNokia)) == "Nokia");
}

TEST(yahoo_keeps_its_own_type_on_the_edge) {
  auto store = makeMemoryStore();
    std::string error;
  CHECK(YahooSource::parse(fixture("nokia.json"), entry("NOKIA.HE"), *store, error));
  CHECK(YahooSource::parse(fixture("gold.json"), entry("GC=F"), *store, error));
  auto s = store->snapshot();
  auto typeOf = [&](const EdgeKey & k) { return s->listing(k)->type; };
  CHECK(typeOf(edgeOf(*s, kNokia, "EUR", "XHEL", "yahoo")) == "EQUITY");
  CHECK(typeOf(edgeOf(*s, kGold, "USD", "XXXX", "yahoo")) ==
        "FUTURE"); // the contract, not the metal
  CHECK(s->kind(*s->findInstrumentByIsin(kGold)) == InstrumentKind::Commodity);
}

#include "source/AutoSource.h"
#include "source/NordnetSource.h"

namespace {

// A source that says what it did: it changes the store `changes` times, or none.
class Scripted : public Source {
 public:
  Scripted(std::string n, int changes, std::chrono::milliseconds every = {}) : name_(std::move(n)), changes_(changes), every_(every) {}
  std::string name() const override { return name_; }
  std::string description() const override { return ""; }
  void load(Store &, Http &, SourceListener & listener) override {
    loads++;
    for (int i = 0; i < changes_; i++) listener.changed();
  }
  std::chrono::milliseconds refreshInterval() const override { return every_; }
  int loads = 0;

 private:
  std::string name_;
  int changes_;
  std::chrono::milliseconds every_;
};

}  // namespace

TEST(auto_stays_with_the_primary_when_it_gives_something) {
  auto store = makeMemoryStore();
  FakeHttp http;
  auto primary = std::make_unique<Scripted>("nordnet", 2, std::chrono::minutes(5));
  auto fallback = std::make_unique<Scripted>("yahoo", 1);
  Scripted * p = primary.get(), * f = fallback.get();
  AutoSource source(std::move(primary), std::move(fallback));
  Listener listener;
  source.load(*store, http, listener);
  source.load(*store, http, listener);
  CHECK(p->loads == 2 && f->loads == 0);
  CHECK(listener.changes == 4 && listener.messages.empty());
  CHECK(source.refreshInterval() == std::chrono::minutes(5));  // the primary's
}

TEST(auto_jumps_to_the_fallback_when_the_primary_gives_nothing_and_stays_there) {
  auto store = makeMemoryStore();
  FakeHttp http;
  auto primary = std::make_unique<Scripted>("nordnet", 0, std::chrono::minutes(5));
  auto fallback = std::make_unique<Scripted>("yahoo", 3);
  Scripted * p = primary.get(), * f = fallback.get();
  AutoSource source(std::move(primary), std::move(fallback));
  Listener listener;
  source.load(*store, http, listener);
  CHECK(p->loads == 1 && f->loads == 1);
  CHECK(listener.changes == 3);
  CHECK(listener.messages.size() == 1 && listener.messages[0] == "nordnet gave nothing: using yahoo");
  source.load(*store, http, listener);  // the next load goes to the fallback alone
  CHECK(p->loads == 1 && f->loads == 2);
  CHECK(source.refreshInterval() == std::chrono::milliseconds(0));  // the fallback's: once
}

TEST(auto_with_nordnet_that_cannot_be_reached_loads_yahoo) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("nordnet") != std::string::npos || url.find("nntech") != std::string::npos) return failed(0);  // no answer
    return failed(404);
  };
  Listener listener;
  AutoSource source(std::make_unique<NordnetSource>(NordnetSource::defaultLists(), std::chrono::milliseconds(0)),
                    std::make_unique<YahooSource>(testList(), "1y", std::chrono::milliseconds(0)));
  source.load(*store, http, listener);
  bool jumped = false, asked = false;
  for (const std::string & m : listener.messages) jumped = jumped || m == "nordnet gave nothing: using yahoo";
  for (const std::string & u : http.urls) asked = asked || u.find("finance.yahoo.com") != std::string::npos;
  CHECK(jumped && asked);
}

TEST(a_stopped_primary_has_not_failed) {
  auto store = makeMemoryStore();
  FakeHttp http;
  auto primary = std::make_unique<Scripted>("nordnet", 0);
  auto fallback = std::make_unique<Scripted>("yahoo", 1);
  Scripted * f = fallback.get();
  AutoSource source(std::move(primary), std::move(fallback));
  struct Stopped : Listener {
    bool cancelled() const override { return true; }
  } listener;
  source.load(*store, http, listener);
  CHECK(f->loads == 0 && listener.messages.empty());  // cancelled: nothing is decided
}

TEST(two_sources_do_not_write_the_same_dataset) {
  std::vector<std::unique_ptr<Source>> sources;
  sources.push_back(std::make_unique<NordnetSource>());
  sources.push_back(std::make_unique<YahooSource>(InstrumentList{}));
  sources.push_back(std::make_unique<SyntheticSource>(InstrumentList{}));
  CHECK(datasetCodeClash(sources).empty());
  // Nordnet twice, or auto (which is Nordnet and Yahoo) beside either, would put one's data under the other's.
  sources.push_back(std::make_unique<AutoSource>(std::make_unique<NordnetSource>(), std::make_unique<YahooSource>(InstrumentList{})));
  const std::string clash = datasetCodeClash(sources);
  CHECK(clash.find("nordnet") != std::string::npos && clash.find("auto") != std::string::npos);
  std::vector<std::unique_ptr<Source>> twice;
  twice.push_back(std::make_unique<NordnetSource>());
  twice.push_back(std::make_unique<NordnetSource>());
  CHECK(!datasetCodeClash(twice).empty());
}

TEST(the_datasets_of_the_shipped_fund_schemas_are_codes_that_no_other_source_has) {
  std::set<std::string> codes = {kNordnetDataset, kYahooDataset, kSyntheticDataset};
  size_t schemas = 0;
  for (const auto & entry : std::filesystem::directory_iterator(std::string(STOCKS_DATA_DIR) + "/funds")) {
    if (entry.path().extension() != ".json") continue;
    FundSchema schema;
    std::string error;
    CHECK(FundSchema::load(entry.path().string(), schema, error));  // a schema is refused with a dataset that is no code
    CHECK(codes.insert(schema.dataset).second);                     // and no two have the same
    schemas++;
  }
  CHECK(schemas >= 2);
}

namespace {
// A source that waits a minute between two requests, and says whether the wait was cut short.
class PausingSource : public Source {
 public:
  PausingSource(std::atomic<bool> & waiting, std::atomic<bool> & cut) : waiting_(waiting), cut_(cut) {}
  std::string name() const override { return "pausing"; }
  std::string description() const override { return ""; }
  void load(Store &, Http &, SourceListener & listener) override {
    waiting_ = true;
    cut_ = !listener.pause(std::chrono::minutes(1));
  }

 private:
  std::atomic<bool> & waiting_;
  std::atomic<bool> & cut_;
};
}  // namespace

TEST(the_runner_stopping_cuts_a_sources_pause_short) {
  std::shared_ptr<Store> store = makeMemoryStore();
  std::atomic<bool> waiting{false}, cut{false};
  const auto start = std::chrono::steady_clock::now();
  {
    std::vector<std::unique_ptr<Source>> sources;
    sources.push_back(std::make_unique<PausingSource>(waiting, cut));
    SourceRunner runner(store, std::move(sources), [] {}, [](std::string) {});
    CHECK(waitFor([&] { return waiting.load(); }));
  }  // the destructor wakes the pause and joins
  CHECK(cut);
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}

TEST(a_pause_outside_a_runner_sleeps_and_one_after_a_cancel_does_not) {
  Listener listener;
  CHECK(listener.pause(std::chrono::milliseconds(1)));
  struct Cancelled : Listener {
    bool cancelled() const override { return true; }
  } cancelled;
  CHECK(!cancelled.pause(std::chrono::hours(1)));
}

TEST(a_source_returns_when_its_pause_is_cut_short) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(404); };
  struct Stopping : Listener {
    int pauses = 0;
    bool pause(std::chrono::milliseconds) override {
      pauses++;
      return false;
    }
  } listener;
  YahooSource(testList(), "1y", std::chrono::hours(1)).load(*store, http, listener);
  CHECK(listener.pauses == 1 && http.urls.size() == 2);  // the first instrument (both hosts), then the pause said stop
}

namespace {
// A source whose load throws every time, and whose details throw something that is not an exception, and that counts both.
class ThrowingSource : public Source {
 public:
  ThrowingSource(std::atomic<int> & loads, std::atomic<int> & details) : loads_(loads), details_(details) {}
  std::string name() const override { return "throwing"; }
  std::string description() const override { return "A source that throws"; }
  void load(Store &, Http &, SourceListener &) override {
    loads_++;
    throw std::runtime_error("broken answer");
  }
  std::chrono::milliseconds refreshInterval() const override { return std::chrono::milliseconds(5); }
  bool loadDetails(Store &, Http &, SourceListener &) override {
    details_++;
    throw 42;
  }

 private:
  std::atomic<int> & loads_;
  std::atomic<int> & details_;
};
}  // namespace

TEST(a_source_that_throws_is_said_and_the_runner_goes_on_with_its_schedule) {
  std::shared_ptr<Store> store = makeMemoryStore();
  std::atomic<int> loads{0}, details{0}, other{0};
  std::mutex mutex;
  std::vector<std::string> messages;
  {
    std::vector<std::unique_ptr<Source>> sources;
    sources.push_back(std::make_unique<ThrowingSource>(loads, details));
    sources.push_back(std::make_unique<CountingSource>(other, std::chrono::milliseconds(0)));
    SourceRunner runner(store, std::move(sources), [] {}, [&](std::string m) {
      std::lock_guard lock(mutex);
      messages.push_back(std::move(m));
    });
    CHECK(waitFor([&] { return loads >= 3 && details >= 3; }));  // loaded again and again, its details asked for after each
  }
  CHECK(other == 1);  // the other source is not disturbed
  std::lock_guard lock(mutex);
  CHECK(std::count(messages.begin(), messages.end(), "throwing: broken answer") >= 3);
  CHECK(std::count(messages.begin(), messages.end(), "throwing: failed") >= 3);
}

TEST(a_yahoo_answer_that_lacks_a_column_parses_without_crashing) {
  // No volume: the bars have none.
  {
    auto store = makeMemoryStore();
    std::string error;
    CHECK(YahooSource::parse(
        R"({"chart":{"result":[{"timestamp":[1767254400],"indicators":{"quote":[{"open":[1],"high":[2],"low":[0.5],"close":[1.5]}]}}]}})",
        entry("NOKIA.HE"), *store, error));
    auto s = store->snapshot();
    CandleSeries c = s->candles(edgeOf(*s, kNokia, "EUR", "XHEL", "yahoo"), Resolution::Day);
    CHECK(c.candles.size() == 1 && c.candles[0].close == 1.5 && !isDefined(c.candles[0].volume));
  }
  // No open, a quote that is not an object, events of the wrong shape: no candles, or none of the bad events, and no throw.
  const char * bad[] = {
      R"({"chart":{"result":[{"timestamp":[1767254400],"indicators":{"quote":[{"high":[2],"low":[0.5],"close":[1.5],"volume":[3]}]}}]}})",
      R"({"chart":{"result":[{"timestamp":[1767254400],"indicators":{"quote":[[1,2,3]]}}]}})",
      R"({"chart":{"result":[{"timestamp":[1767254400],"indicators":{"quote":{"open":[1]}}}]}})",
      R"({"chart":{"result":[{"timestamp":[1767254400],"indicators":[]}]}})",
      R"({"chart":{"result":[7]}})",
      R"({"chart":[1]})",
      R"({"chart":{"error":{"description":7}}})",
      R"([1, 2])"};
  for (const char * body : bad) {
    auto store = makeMemoryStore();
    std::string error;
    bool parsed = true;
    try {
      parsed = YahooSource::parse(body, entry("NOKIA.HE"), *store, error);
    } catch (...) {
      CHECK(false);
    }
    CHECK(!parsed && !error.empty());
  }
  auto store = makeMemoryStore();
  std::string error;
  CHECK(YahooSource::parse(
      R"({"chart":{"result":[{"timestamp":[1767254400],"indicators":{"quote":[{"open":[1],"high":[2],"low":[0.5],"close":[1.5]}]},)"
      R"("events":{"dividends":{"a":7,"b":{"date":"x"}},"splits":{"a":[1],"b":{"date":1767254400,"numerator":"2","denominator":1}}}}]}})",
      entry("NOKIA.HE"), *store, error));
}

#ifndef _WIN32
TEST(a_cancelled_http_ends_the_request_in_progress_and_makes_no_more) {
  // A server on this machine that takes the connection and never answers: without a cancel the request would wait for
  // the read timeout (20 s).
  const int server = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  CHECK(bind(server, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0 && listen(server, 4) == 0);
  socklen_t length = sizeof address;
  getsockname(server, reinterpret_cast<sockaddr *>(&address), &length);
  const std::string url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/";

  std::unique_ptr<Http> http = makeHttp();
  std::atomic<bool> returned{false};
  HttpResult result;
  std::thread request([&] {
    result = http->get(url);
    returned = true;
  });
  const int connection = accept(server, nullptr, nullptr);  // the request is in progress
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(!returned);
  const auto cancelled = std::chrono::steady_clock::now();
  http->cancel();
  request.join();
  CHECK(std::chrono::steady_clock::now() - cancelled < std::chrono::seconds(5));
  CHECK(result.status == 0 && result.error == "cancelled");
  // A later request is not made.
  const auto again = std::chrono::steady_clock::now();
  CHECK(http->get(url).status == 0 && std::chrono::steady_clock::now() - again < std::chrono::milliseconds(500));
  if (connection >= 0) close(connection);
  close(server);
}
#endif

namespace {
// A source whose load waits like a request that gets no answer, until its Http is cancelled (for a minute at most).
class WaitingSource : public Source {
 public:
  explicit WaitingSource(std::atomic<bool> & waiting) : waiting_(waiting) {}
  std::string name() const override { return "waiting"; }
  std::string description() const override { return ""; }
  void load(Store &, Http & http, SourceListener &) override {
    waiting_ = true;
    for (int i = 0; i < 6000 && !http.cancelled(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

 private:
  std::atomic<bool> & waiting_;
};
}  // namespace

TEST(the_runner_stopping_cancels_its_sources_http) {
  std::shared_ptr<Store> store = makeMemoryStore();
  std::atomic<bool> waiting{false};
  const auto start = std::chrono::steady_clock::now();
  {
    std::vector<std::unique_ptr<Source>> sources;
    sources.push_back(std::make_unique<WaitingSource>(waiting));
    SourceRunner runner(store, std::move(sources), [] {}, [](std::string) {});
    CHECK(waitFor([&] { return waiting.load(); }));
  }
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
}
