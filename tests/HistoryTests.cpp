#include "TestFramework.h"

#include "FakeHttp.h"
#include "history/HistoryLoader.h"
#include "model/Dates.h"

#include <atomic>
#include <fstream>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/nordnet/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

// A loader owns its Http; a test keeps its FakeHttp to look at what it was asked.
struct Borrowed : Http {
  FakeHttp * inner = nullptr;
  HttpResult get(const std::string & url, const HttpHeaders & h) override { return inner->get(url, h); }
};
std::unique_ptr<Http> borrow(FakeHttp & http) {
  auto b = std::make_unique<Borrowed>();
  b->inner = &http;
  return b;
}

HistoryLoader::Options quickly() {
  HistoryLoader::Options o;
  o.pause = std::chrono::milliseconds(0);
  o.offline_pause = std::chrono::milliseconds(20);
  return o;
}

// An edge as Nordnet's discovery leaves it: today's bar and a listing with the order book id.
struct World {
  std::shared_ptr<Store> store = makeMemoryStore();
  Instrument usd = store->addInstrument({{}, "USD", "US dollar", InstrumentKind::Currency});
  Market market = store->addMarket({{}, "Nasdaq Stockholm"});
  Dataset nordnet = store->addDataset({{}, "nordnet"});
  int count = 0;
  EdgeKey edge(InstrumentKind kind, const std::string & book, bool nordnetData = true) {
    Instrument i = store->addInstrument({{}, "", "Instrument", kind, "XX" + std::to_string(100000 + count++)});
    EdgeKey k{i.id, usd.id, market.id, nordnetData ? nordnet.id : store->addDataset({{}, "other"}).id};
    store->appendCandles(k, Resolution::Day, {{dayNumber(2026, 12, 1), kUndefined, 1, 1, 1, kUndefined}});
    store->setListing({k, "SYM", "", book, "ESH", "Instrument"});
    return k;
  }
  CandleSeries days(const EdgeKey & k) { return store->snapshot()->candles(k, Resolution::Day); }
};

}  // namespace

TEST(a_stock_answer_is_daily_bars_keyed_by_their_date_with_the_volume) {
  HistoryLoader::Series s;
  std::string error;
  CHECK(HistoryLoader::parse(fixture("history-stock-day.json"), false, s, error));
  CHECK(s.supported && s.resolution == Resolution::Day && s.bars.size() == 7);
  CHECK(s.bars[0].time == dayNumberOfInstant(1595376000) && s.bars[0].open == 10.8698947007 && s.bars[0].close == 11.0115219932);
  CHECK(s.bars[0].high == 11.5426243402 && s.bars[0].low == 10.5512332926 && s.bars[0].volume == 322161);
  CHECK(s.bars[1].time == s.bars[0].time + 1);
  CHECK(std::find(s.periods.begin(), s.periods.end(), "YEAR_5") != s.periods.end());
}

TEST(a_coarser_answer_is_stored_under_the_resolution_it_returned_with_no_volume_where_there_is_none) {
  HistoryLoader::Series s;
  std::string error;
  // Asked for days, answered in weeks: the answer says so, and that is what counts.
  CHECK(HistoryLoader::parse(fixture("history-gold-week.json"), false, s, error));
  CHECK(s.supported && s.resolution == Resolution::Week && s.bars.size() == 7);
  CHECK(s.bars[0].time == dayNumberOfInstant(865814400));  // a Monday: the week's first day
  CHECK(weekday(s.bars[0].time) == 0 && s.bars[1].time == s.bars[0].time + 7);
  CHECK(s.bars[0].close == 340.9 && !isDefined(s.bars[0].volume));  // 0 is no volume
  CHECK(std::find(s.periods.begin(), s.periods.end(), "YEAR_10") != s.periods.end());
}

TEST(a_funds_answer_is_a_nav_a_bar_with_only_a_close) {
  HistoryLoader::Series s;
  std::string error;
  CHECK(HistoryLoader::parse(fixture("history-fund-nav.json"), true, s, error));
  CHECK(s.bars.size() == 7 && s.bars[0].close == 3.6226839455928);
  CHECK(!isDefined(s.bars[0].open) && !isDefined(s.bars[0].high) && !isDefined(s.bars[0].low) && !isDefined(s.bars[0].volume));
}

TEST(what_is_not_a_series_is_refused_and_a_resolution_not_stored_is_left_unsupported) {
  HistoryLoader::Series s;
  std::string error;
  CHECK(!HistoryLoader::parse("", false, s, error) && error == "not a price time series");
  CHECK(!HistoryLoader::parse("{}", false, s, error));
  CHECK(!HistoryLoader::parse("<html>", false, s, error));
  // Minute bars: parsed as an answer, but not ours to store yet.
  CHECK(HistoryLoader::parse(R"({"pricePoints":[{"timeStamp":1,"last":1}],"returnedResolution":"MIN_5","availablePeriods":["DAY_1"]})", false, s, error));
  CHECK(!s.supported && s.bars.empty());
}

TEST(wanting_an_edge_loads_its_whole_history_daily_and_writes_it_to_the_store) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Company, "book-1");
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("history-stock-day.json")); };
  std::atomic<int> changes{0};
  HistoryLoader loader(w.store, borrow(http), quickly(), [&] { changes++; });
  loader.want({k});
  loader.waitIdle();
  CHECK(http.urls.size() == 1);
  CHECK(http.urls[0] == "https://api.prod.nntech.io/market-data/v3/price-time-series/period/ALL/identifier/book-1?resolution=DAY");
  bool locale = false, cookie = false;
  for (const auto & [n, v] : http.headers[0]) {
    locale = locale || (n == "x-locale" && v == "fi-FI");
    cookie = cookie || n == "Cookie";
  }
  CHECK(locale && !cookie);
  CHECK(w.days(k).candles.size() == 7 + 1);  // the history, and today's bar from the discovery (another date)
  CHECK(changes >= 1);

  // Done: wanted again, it is not asked for again.
  loader.want({k});
  loader.waitIdle();
  CHECK(http.urls.size() == 1);
}

TEST(a_fund_is_asked_for_its_nav) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Fund, "book-f");
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("history-fund-nav.json")); };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.want({k});
  loader.waitIdle();
  CHECK(!http.urls.empty() && http.urls[0].find("identifier/book-f?resolution=DAY&fundType=FUND_NAV") != std::string::npos);
  // The answer came weekly: stored as weeks, and the longest period that gives days is tried next.
  CHECK(w.store->snapshot()->candles(k, Resolution::Week).candles.size() == 7);
}

TEST(where_the_whole_period_comes_back_weekly_the_longest_period_that_gives_days_is_asked_for_too) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Commodity, "book-g");
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("/period/YEAR_5/") != std::string::npos) return ok(fixture("history-gold-year5-day.json"));
    return ok(fixture("history-gold-week.json"));  // ALL and YEAR_10: weekly
  };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.want({k});
  loader.waitIdle();
  CHECK(http.urls.size() == 3);
  CHECK(http.urls[0].find("/period/ALL/") != std::string::npos && http.urls[1].find("/period/YEAR_10/") != std::string::npos &&
        http.urls[2].find("/period/YEAR_5/") != std::string::npos);
  auto s = w.store->snapshot();
  CHECK(s->candles(k, Resolution::Week).candles.size() == 7);   // back to 1997
  CHECK(s->candles(k, Resolution::Day).candles.size() == 7 + 1);  // the last five years
  CHECK((s->resolutions(k) == std::vector<Resolution>{Resolution::Day, Resolution::Week}));
}

TEST(edges_that_cannot_or_need_not_be_loaded_are_left_out) {
  World w;
  EdgeKey notNordnet = w.edge(InstrumentKind::Company, "book-a", false);
  EdgeKey noBook = w.edge(InstrumentKind::Company, "");
  EdgeKey has = w.edge(InstrumentKind::Company, "book-c");
  std::vector<Candle> past;
  for (int d = 0; d < 6; d++) past.push_back({dayNumber(2026, 1, 1) + d, 1, 1, 1, 1, 1});
  w.store->appendCandles(has, Resolution::Day, past);  // it has its history already
  EdgeKey noListing{w.store->addInstrument({{}, "", "X", InstrumentKind::Company, "XX999999"}).id, w.usd.id, w.market.id, w.nordnet.id};
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("history-stock-day.json")); };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.want({notNordnet, noBook, has, noListing});
  loader.waitIdle();
  CHECK(http.urls.empty());
}

TEST(a_missing_series_is_remembered_and_a_refusal_stops_everything) {
  {
    World w;
    EdgeKey k = w.edge(InstrumentKind::Company, "book-1");
    FakeHttp http;
    http.answer = [](const std::string &) { return failed(404); };
    HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
    loader.want({k});
    loader.waitIdle();
    loader.want({k});
    loader.waitIdle();
    CHECK(http.urls.size() == 1);  // not asked for again
  }
  for (int status : {403, 429}) {
    World w;
    EdgeKey a = w.edge(InstrumentKind::Company, "book-a"), b = w.edge(InstrumentKind::Company, "book-b");
    FakeHttp http;
    http.answer = [status](const std::string &) { return failed(status); };
    std::vector<std::string> said;
    HistoryLoader loader(w.store, borrow(http), quickly(), [] {}, [&](std::string m) { said.push_back(m); });
    loader.want({a, b});
    loader.waitIdle();
    CHECK(loader.refused() && http.urls.size() == 1);  // the second edge is not tried
    CHECK(said.size() == 1);
    loader.want({b});
    loader.waitIdle();
    CHECK(http.urls.size() == 1);
  }
}

TEST(no_answer_pauses_and_the_edge_is_asked_for_again) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Company, "book-1");
  FakeHttp http;
  std::atomic<int> calls{0};
  http.answer = [&](const std::string &) { return calls++ == 0 ? failed(0) : ok(fixture("history-stock-day.json")); };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.want({k});
  for (int i = 0; i < 400 && w.days(k).candles.size() < 8; i++) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  CHECK(w.days(k).candles.size() == 8);
  CHECK(http.urls.size() == 2);  // the first got no answer, the second was after the pause
}

TEST(edges_are_loaded_in_the_order_they_are_wanted) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Company, "book-a"), b = w.edge(InstrumentKind::Company, "book-b"),
          c = w.edge(InstrumentKind::Company, "book-c");
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("history-stock-day.json")); };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.want({c, a, b});
  loader.waitIdle();
  CHECK(http.urls.size() == 3);
  CHECK(http.urls[0].find("book-c") != std::string::npos && http.urls[1].find("book-a") != std::string::npos &&
        http.urls[2].find("book-b") != std::string::npos);
}
