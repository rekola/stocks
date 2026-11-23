#include "HistoryWorld.h"

#include "source/NordnetHistory.h"

#include <atomic>
#include <thread>

// Nordnet's price history: the adapter that knows its endpoint, headers and answers, driven by a history loader as the program does.
using namespace history_test;

namespace {

NordnetHistory::Options full() {
  NordnetHistory::Options o;
  o.depth = NordnetHistory::Depth::Full;
  return o;
}

// An intraday answer: 5-minute bars of 1 December 2026 from 07:00 UTC, in the resolution `returned`.
std::string intraday(const std::string & returned, int count, int stepSeconds, double previousClose) {
  std::string points;
  const int64_t start = (dayNumber(2026, 12, 1) - kUnixEpochDayNumber) * 86400 + 7 * 3600;
  for (int i = 0; i < count; i++)
    points += std::string(i ? "," : "") + R"({"timeStamp":)" + std::to_string((start + i * stepSeconds) * 1000) + R"(,"open":1,"high":2,"low":1,"last":)" +
              std::to_string(10 + i) + R"(,"volume":5})";
  return R"({"pricePoints":[)" + points + R"(],"previousClose":)" + std::to_string(previousClose) + R"(,"returnedResolution":")" + returned + R"(","availablePeriods":["DAY_1"]})";
}
}  // namespace

TEST(a_stock_answer_is_daily_bars_keyed_by_their_date_with_the_volume) {
  NordnetHistory::Series s;
  std::string error;
  CHECK(NordnetHistory::parse(fixture("history-stock-day.json"), false, s, error));
  CHECK(s.supported && s.resolution == Resolution::Day && s.bars.size() == 7);
  CHECK(s.bars[0].time == dayNumberOfInstant(1595376000) && s.bars[0].open == 10.8698947007 && s.bars[0].close == 11.0115219932);
  CHECK(s.bars[0].high == 11.5426243402 && s.bars[0].low == 10.5512332926 && s.bars[0].volume == 322161);
  CHECK(s.bars[1].time == s.bars[0].time + 1);
  CHECK(std::find(s.periods.begin(), s.periods.end(), "YEAR_5") != s.periods.end());
}

TEST(a_coarser_answer_is_stored_under_the_resolution_it_returned_with_no_volume_where_there_is_none) {
  NordnetHistory::Series s;
  std::string error;
  // Asked for days, answered in weeks: the answer says so, and that is what counts.
  CHECK(NordnetHistory::parse(fixture("history-gold-week.json"), false, s, error));
  CHECK(s.supported && s.resolution == Resolution::Week && s.bars.size() == 7);
  CHECK(s.bars[0].time == dayNumberOfInstant(865814400));  // a Monday: the week's first day
  CHECK(weekday(s.bars[0].time) == 0 && s.bars[1].time == s.bars[0].time + 7);
  CHECK(s.bars[0].close == 340.9 && !isDefined(s.bars[0].volume));  // 0 is no volume
  CHECK(std::find(s.periods.begin(), s.periods.end(), "YEAR_10") != s.periods.end());
}

TEST(a_funds_answer_is_a_nav_a_bar_with_only_a_close) {
  NordnetHistory::Series s;
  std::string error;
  CHECK(NordnetHistory::parse(fixture("history-fund-nav.json"), true, s, error));
  CHECK(s.bars.size() == 7 && s.bars[0].close == 3.6226839455928);
  CHECK(!isDefined(s.bars[0].open) && !isDefined(s.bars[0].high) && !isDefined(s.bars[0].low) && !isDefined(s.bars[0].volume));
}

TEST(what_is_not_a_series_is_refused_and_a_resolution_not_stored_is_left_unsupported) {
  NordnetHistory::Series s;
  std::string error;
  CHECK(!NordnetHistory::parse("", false, s, error) && error == "not a price time series");
  CHECK(!NordnetHistory::parse("{}", false, s, error));
  CHECK(!NordnetHistory::parse("<html>", false, s, error));
  // A resolution we have no name for: parsed as an answer, but not ours to store.
  CHECK(NordnetHistory::parse(R"({"pricePoints":[{"timeStamp":1,"last":1}],"returnedResolution":"SECOND_1","availablePeriods":["DAY_1"]})", false, s, error));
  CHECK(!s.supported && s.bars.empty());
}

TEST(wanting_an_edge_loads_the_last_three_months_daily_in_one_request_and_writes_them_to_the_store) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Company, "book-1");
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("history-stock-day.json")); };
  std::atomic<int> changes{0};
  HistoryLoader loader(w.store, borrow(http), quickly(), [&] { changes++; });
  loader.addProvider(std::make_shared<NordnetHistory>());
  loader.want({k});
  loader.waitIdle();
  CHECK(http.urls.size() == 1);
  CHECK(http.urls[0] == "https://api.prod.nntech.io/market-data/v3/price-time-series/period/MONTH_3/identifier/book-1?resolution=DAY");
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
  loader.addProvider(std::make_shared<NordnetHistory>());
  loader.want({k});
  loader.waitIdle();
  CHECK(http.urls.size() == 1);  // one request: the snapshot
  CHECK(http.urls[0].find("MONTH_3/identifier/book-f?resolution=DAY&fundType=FUND_NAV") != std::string::npos);
  // Whatever resolution the answer says it has is the one it is stored under.
  CHECK(w.store->snapshot()->candles(k, Resolution::Week).candles.size() == 7);
}

TEST(a_full_load_asks_for_the_whole_period_and_where_it_comes_back_weekly_the_longest_period_that_gives_days) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Commodity, "book-g");
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("/period/YEAR_5/") != std::string::npos) return ok(fixture("history-gold-year5-day.json"));
    return ok(fixture("history-gold-week.json"));  // ALL and YEAR_10: weekly
  };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.addProvider(std::make_shared<NordnetHistory>(full()));
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
  for (int d = 0; d < 70; d++) past.push_back({dayNumber(2026, 12, 1) - 70 + d, 1, 1, 1, 1, 1});  // up to the discovery's bar of 1 December
  w.store->appendCandles(has, Resolution::Day, past);  // it has its history already
  EdgeKey noListing{w.store->addInstrument("", "X", InstrumentKind::Company, testIsin(999999)), w.usd, w.market, w.nordnet};
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("history-stock-day.json")); };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.addProvider(std::make_shared<NordnetHistory>());
  loader.want({notNordnet, noBook, has, noListing});
  loader.waitIdle();
  CHECK(http.urls.empty());
}

TEST(intraday_bars_are_stored_under_their_slot_numbers_and_the_resolution_the_answer_returned) {
  NordnetHistory::Series s;
  std::string error;
  CHECK(NordnetHistory::parse(intraday("MIN_5", 3, 300, 9.5), false, s, error));
  const int64_t start = (dayNumber(2026, 12, 1) - kUnixEpochDayNumber) * 86400 + 7 * 3600;
  CHECK(s.supported && s.resolution == Resolution::Min5 && s.bars.size() == 3);
  CHECK(s.bars[0].time == slotOfInstant(Resolution::Min5, start) && s.bars[0].time == start / 300);  // a slot number, not seconds
  CHECK(s.bars[1].time == s.bars[0].time + 1 && s.bars[0].close == 10 && s.bars[0].volume == 5);
  CHECK(s.previousClose == 9.5);
  // Hourly bars are by the hour number; one that is not on the hour is left out.
  CHECK(NordnetHistory::parse(intraday("HOUR_1", 3, 3600, 9), false, s, error));
  CHECK(s.resolution == Resolution::Hour1 && s.bars[0].time == start / 3600 && s.bars.size() == 3);
  CHECK(NordnetHistory::parse(intraday("HOUR_1", 2, 1800, 9), false, s, error));
  CHECK(s.bars.size() == 1);
}

TEST(the_day_is_one_request_of_five_minute_bars_stored_with_the_previous_close_and_not_asked_again) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Company, "book-1");
  FakeHttp http;
  // Asked for 5 minutes, answered in 15: what the answer says is what is stored.
  http.answer = [](const std::string &) { return ok(intraday("MIN_15", 4, 900, 9.5)); };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.addProvider(std::make_shared<NordnetHistory>());
  loader.want({k}, Period::Day);
  loader.waitIdle();
  CHECK(http.urls.size() == 1);
  CHECK(http.urls[0] == "https://api.prod.nntech.io/market-data/v3/price-time-series/period/DAY_1/identifier/book-1?resolution=MIN_5");
  auto snapshot = w.store->snapshot();
  CHECK(snapshot->candles(k, Resolution::Min15).candles.size() == 4 && !snapshot->hasCandles(k, Resolution::Min5));
  CHECK(snapshot->previousClose(k, dayNumber(2026, 12, 1)) == 9.5 && !snapshot->previousClose(k, dayNumber(2026, 12, 2)));
  loader.want({k}, Period::Day);
  loader.waitIdle();
  CHECK(http.urls.size() == 1);  // once per edge and period
}

TEST(each_period_asks_once_per_edge_for_the_rows_wanted_and_a_month_shares_the_quarters_request) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Company, "book-a"), b = w.edge(InstrumentKind::Company, "book-b"), c = w.edge(InstrumentKind::Company, "book-c");
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("YEAR_1") != std::string::npos) return ok(fixture("history-gold-week.json"));
    if (url.find("HOUR_1") != std::string::npos) return ok(intraday("HOUR_1", 3, 3600, 9));
    return ok(fixture("history-stock-day.json"));
  };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.addProvider(std::make_shared<NordnetHistory>());
  auto asked = [&](const std::string & what) {
    size_t n = 0;
    for (const std::string & u : http.urls) n += u.find(what) != std::string::npos;
    return n;
  };
  loader.want({a, b}, Period::Week);  // c is not on screen
  loader.waitIdle();
  CHECK(http.urls.size() == 2 && asked("WEEK_1") == 2 && asked("book-c") == 0);
  loader.want({a, b, c}, Period::Week);
  loader.waitIdle();
  CHECK(http.urls.size() == 3 && asked("book-c") == 1);  // only the one not asked before
  loader.want({a}, Period::Year);
  loader.waitIdle();
  CHECK(asked("YEAR_1/identifier/book-a?resolution=WEEK") == 1);
  loader.want({a}, Period::Month);
  loader.waitIdle();
  loader.want({a}, Period::Quarter);
  loader.waitIdle();
  CHECK(asked("MONTH_3/identifier/book-a") == 1);  // the month and the quarter are one request
  loader.want({a, b, c}, Period::Year);
  loader.waitIdle();
  CHECK(http.urls.size() == 3 + 1 + 1 + 2);
}

TEST(the_quarter_never_expires_and_a_failure_is_not_asked_again) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Company, "book-a"), gone = w.edge(InstrumentKind::Company, "book-gone");
  FakeHttp http;
  http.answer = [](const std::string & url) {
    if (url.find("book-gone") != std::string::npos) return failed(404);
    return ok(fixture("history-stock-day.json"));
  };
  NordnetHistory::Options o;
  o.day_lifetime = o.week_lifetime = o.year_lifetime = std::chrono::milliseconds(20);
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.addProvider(std::make_shared<NordnetHistory>(o));
  loader.want({a, gone}, Period::Quarter);
  loader.waitIdle();
  CHECK(http.urls.size() == 2);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  loader.want({a, gone}, Period::Quarter);
  loader.waitIdle();
  CHECK(http.urls.size() == 2);
}

TEST(a_fund_has_no_intraday_so_its_day_and_week_are_the_daily_request_and_what_the_store_has_is_used_first) {
  World w;
  EdgeKey fund = w.edge(InstrumentKind::Fund, "book-f");
  EdgeKey stock = w.edge(InstrumentKind::Company, "book-s");
  std::vector<Candle> past;
  for (int d = 0; d < 70; d++) past.push_back({dayNumber(2026, 12, 1) - 70 + d, 1, 1, 1, 1, 1});
  w.store->appendCandles(stock, Resolution::Day, past);
  w.store->appendCandles(stock, Resolution::Hour1, {{slotOfInstant(Resolution::Hour1, (dayNumber(2026, 12, 1) - kUnixEpochDayNumber) * 86400), 1, 1, 1, 1, 1}});
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("history-fund-nav.json")); };
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.addProvider(std::make_shared<NordnetHistory>());
  loader.want({fund}, Period::Day);
  loader.waitIdle();
  CHECK(http.urls.size() == 1 && http.urls[0].find("MONTH_3") != std::string::npos && http.urls[0].find("FUND_NAV") != std::string::npos);
  // The stock has a quarter of daily bars (a quarter and a month need no request) but no five-minute ones (the day does).
  loader.want({stock}, Period::Quarter);
  loader.waitIdle();
  CHECK(http.urls.size() == 1);
  loader.want({stock}, Period::Month);
  loader.waitIdle();
  CHECK(http.urls.size() == 1);
  // An hourly bar is there, but it does not reach back a week: asked for.
  loader.want({stock}, Period::Week);
  loader.waitIdle();
  CHECK(http.urls.size() == 2);
}


TEST(the_status_of_an_answer_says_what_became_of_the_request) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Company, "book-1");
  NordnetHistory history;
  const auto outcomeOf = [&](int status) {
    FakeHttp http;
    http.answer = [status](const std::string &) { return status == 200 ? ok(fixture("history-stock-day.json")) : failed(status); };
    return history.load(*w.store, http, k, NordnetHistory::Quarter, [] { return true; });
  };
  CHECK(outcomeOf(200) == HistoryProvider::Outcome::Ok);
  CHECK(outcomeOf(404) == HistoryProvider::Outcome::Missing && outcomeOf(400) == HistoryProvider::Outcome::Missing);
  CHECK(outcomeOf(403) == HistoryProvider::Outcome::Refused && outcomeOf(429) == HistoryProvider::Outcome::Refused);
  CHECK(outcomeOf(500) == HistoryProvider::Outcome::Offline && outcomeOf(0) == HistoryProvider::Outcome::Offline);
}

TEST(nordnets_history_is_asked_for_by_the_headers_its_endpoint_expects_and_its_dataset_is_nordnets) {
  World w;
  EdgeKey k = w.edge(InstrumentKind::Company, "book-1");
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("history-stock-day.json")); };
  NordnetHistory history;
  CHECK(history.dataset() == "nordnet");
  history.load(*w.store, http, k, NordnetHistory::Quarter, [] { return true; });
  bool origin = false;
  for (const auto & [n, v] : http.headers.at(0)) origin = origin || (n == "Origin" && v == "https://www.nordnet.fi");
  CHECK(origin);
}

TEST(a_wanted_edge_is_requested_again_once_its_request_has_expired_and_not_before) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Company, "book-a"), b = w.edge(InstrumentKind::Company, "book-b");
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(intraday("MIN_5", 3, 300, 9.5)); };
  NordnetHistory::Options o;
  o.day_lifetime = std::chrono::milliseconds(150);
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.addProvider(std::make_shared<NordnetHistory>(o));
  loader.want({a, b}, Period::Day);
  loader.waitIdle();
  CHECK(http.urls.size() == 2);
  loader.want({a, b}, Period::Day);
  loader.waitIdle();
  CHECK(http.urls.size() == 2);  // the store has the bars and the request is young
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  loader.want({a}, Period::Day);  // b is not wanted: it is not asked for, whatever its age
  loader.waitIdle();
  CHECK(http.urls.size() == 3 && http.urls[2].find("book-a") != std::string::npos);
  loader.want({a}, Period::Day);
  loader.waitIdle();
  CHECK(http.urls.size() == 3);  // the time starts again from the new load
  loader.want({b}, Period::Day);
  loader.waitIdle();
  CHECK(http.urls.size() == 4 && http.urls[3].find("book-b") != std::string::npos);
}

TEST(an_expired_request_queued_again_by_want_is_fetched_and_not_dropped_as_done) {
  World w;
  EdgeKey a = w.edge(InstrumentKind::Company, "book-a");
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(intraday("MIN_5", 3, 300, 9.5)); };
  NordnetHistory::Options o;
  o.day_lifetime = std::chrono::milliseconds(30);
  HistoryLoader loader(w.store, borrow(http), quickly(), [] {});
  loader.addProvider(std::make_shared<NordnetHistory>(o));
  for (size_t round = 1; round <= 3; round++) {
    loader.want({a}, Period::Day);  // expired since the last round: queued, popped by run() (which finds it in done_) and loaded
    loader.waitIdle();
    CHECK(http.urls.size() == round);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
  }
}
