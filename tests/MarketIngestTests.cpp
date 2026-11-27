#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Api.h"
#include "market/Ingest.h"
#include "model/Period.h"
#include "model/SqliteBarStore.h"
#include "model/Store.h"
#include "source/KnownIsins.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

using namespace histdata;

namespace {

const market::Log kQuiet = [](const std::string &) {};
const Timestamp kLast = 1794603600;  // 2026-11-13 21:00 UTC: 16:00 EST, a row of the made-up site (the real file ends 21:58)

std::unique_ptr<Store> plainStore() {
  StoreOptions options;
  options.invent_isins = false;  // as the programs have it: an instrument with no ISIN is refused
  return makeMemoryStore(std::move(options));
}

EdgeKey onlyEdge(const Store & store) {
  const auto edges = store.snapshot()->quoteRowsSince(0);
  return edges.empty() ? EdgeKey{} : edges[0];
}

std::vector<int64_t> keysOf(const Seq<Candle> & queue) {
  std::vector<int64_t> keys;
  for (const Candle & c : queue) keys.push_back(c.time);
  return keys;
}

// What the made-up site's rows say a resolution's window should hold, counted the slow way: the distinct keys of the rows in the window that the
// model's rule gives (the bars whose instant is within the keep of the last bar's).
std::vector<int64_t> expectedKeys(const FakeSite & site, Resolution r) {
  std::set<int64_t> keys;
  std::vector<market::MinuteBar> all;
  for (int y = 2015; y <= 2026; y++) {
    if (y == 2026) {
      for (int m = 1; m <= 12; m++) {
        char p[8];
        std::snprintf(p, sizeof p, "%04d%02d", y, m);
        for (auto & row : site.rows(p)) all.push_back(row);
      }
    } else {
      for (auto & row : site.rows(std::to_string(y))) all.push_back(row);
    }
  }
  Timestamp lastRow = 0;  // the window ends at the last row the site has, which is not always its cap
  for (const auto & row : all) lastRow = std::max(lastRow, row.time);
  const int64_t last = market::barKey(r, lastRow);
  const int64_t from = market::barStart(r, last) - market::keepSeconds(r);
  for (const auto & row : all) {
    const int64_t k = market::barKey(r, row.time);
    if (k <= last && market::barStart(r, k) >= from) keys.insert(k);
  }
  return {keys.begin(), keys.end()};
}

struct Fixture {
  FakeSite site;
  TempDir dir;
  std::unique_ptr<Store> store = plainStore();
  Fixture() {
    site.lastBar = kLast;
    site.step = 120;
  }
  market::IngestReport run(std::vector<Resolution> resolutions, bool refresh = false, std::vector<std::string> codes = {"eurusd"}, Timestamp now = kTestNow) {
    HistData adapter(site, market::RawCache(dir.path), fastSettings(now));
    market::IngestOptions options;
    options.codes = codes;
    options.resolutions = resolutions;
    options.refresh = refresh;
    return market::runIngest(adapter, *store, options, kQuiet);
  }
};

const std::vector<Resolution> kAll = {Resolution::Min5, Resolution::Hour1, Resolution::Day, Resolution::Week};

}  // namespace

TEST(ingest_5m_downloads_one_file_when_the_day_is_in_one_month) {
  Fixture f;
  const auto report = f.run({Resolution::Min5});
  CHECK(f.site.downloads() == 1);
  CHECK(report.complete());
  const EdgeKey edge = onlyEdge(*f.store);
  const auto snapshot = f.store->snapshot();
  CHECK(keysOf(snapshot->queue(edge, Resolution::Min5)) == expectedKeys(f.site, Resolution::Min5));
  CHECK(!snapshot->queue(edge, Resolution::Min5).empty() && snapshot->queue(edge, Resolution::Min5).back().time == market::barKey(Resolution::Min5, kLast));
  CHECK(snapshot->queue(edge, Resolution::Day).empty());  // nothing of the other resolutions
}

TEST(ingest_5m_downloads_two_files_when_the_day_spans_a_month_boundary) {
  Fixture f;
  f.site.lastBar = utcOfEst(2026, 11, 1, 3, 0, 0);  // 3 am EST on the 1st: the day before is in October
  f.site.step = 30;
  const auto report = f.run({Resolution::Min5});
  CHECK(f.site.downloads() == 2);
  CHECK(report.complete());
  const auto queue = f.store->snapshot()->queue(onlyEdge(*f.store), Resolution::Min5);
  CHECK(!queue.empty() && queue[0].time < market::barKey(Resolution::Min5, utcOfEst(2026, 11, 1, 0, 0, 0)));
}

TEST(ingest_every_resolution_holds_the_window_of_the_rows_and_each_file_is_asked_for_once) {
  Fixture f;
  f.site.step = 360;  // a few rows a day: the ten years are a lot of files, not of rows
  const auto report = f.run(kAll);
  CHECK(report.complete());
  const EdgeKey edge = onlyEdge(*f.store);
  const auto snapshot = f.store->snapshot();
  for (Resolution r : kAll) {
    CHECK(keysOf(snapshot->queue(edge, r)) == expectedKeys(f.site, r));
    CHECK(!snapshot->queue(edge, r).empty());
  }
  CHECK(f.site.downloads() == 21);  // ten yearly files (2016 to 2025) and the eleven months of this year, one request pair each
  std::set<std::string> posts;
  for (const auto & r : f.site.requests)
    if (r.rfind("POST", 0) == 0) posts.insert(r);
  int yearly = 0, monthly = 0;
  for (const auto & r : posts) (r.size() - r.rfind(' ') - 1 == 4 ? yearly : monthly)++;
  CHECK(posts.size() == 21 && yearly == 10 && monthly == 11);  // never the months of a past year, nor the whole of this one
}

TEST(ingest_bars_are_first_open_max_high_min_low_last_close) {
  Fixture f;
  f.run({Resolution::Hour1});
  const auto queue = f.store->snapshot()->queue(onlyEdge(*f.store), Resolution::Hour1);
  CHECK(!queue.empty());
  if (queue.empty()) return;
  std::vector<market::MinuteBar> mine;
  for (const auto & r : f.site.rows("202611"))
    if (market::barKey(Resolution::Hour1, r.time) == queue.back().time) mine.push_back(r);
  CHECK(!mine.empty());
  double high = 0, low = 1e9;
  for (const auto & m : mine) high = std::max(high, m.high), low = std::min(low, m.low);
  CHECK_NEAR(queue.back().open, mine.front().open, 1e-3);  // a queue keeps a price in the form of its record (a float of the difference from the open)
  CHECK_NEAR(queue.back().close, mine.back().close, 1e-3);
  CHECK_NEAR(queue.back().high, high, 1e-3);
  CHECK_NEAR(queue.back().low, low, 1e-3);
}

TEST(ingest_a_second_run_with_a_warm_cache_makes_no_requests) {
  Fixture f;
  f.site.step = 360;
  f.run(kAll);
  const size_t before = f.site.requests.size();
  CHECK(before > 0);
  f.store = plainStore();  // a new program
  const auto again = f.run(kAll);
  CHECK(f.site.requests.size() == before);
  CHECK(again.complete());
  CHECK(again.instruments[0].downloads == 0 && again.instruments[0].cached == 21);
}

TEST(ingest_says_the_same_bars_again_and_nothing_changes) {
  Fixture f;
  f.run({Resolution::Min5});
  const uint64_t version = f.store->snapshot()->version();
  f.run({Resolution::Min5});
  CHECK(f.store->snapshot()->version() == version);
}

TEST(ingest_refresh_asks_again_for_this_years_months_only) {
  Fixture f;
  f.site.step = 360;
  f.run(kAll);
  f.site.requests.clear();
  f.site.lastBar += 3 * 86400;  // the site has grown (a month's file does)
  f.run(kAll, /*refresh=*/true);
  int years = 0, months = 0;
  for (const auto & r : f.site.requests)
    if (r.rfind("POST", 0) == 0) (r.size() - r.rfind(' ') == 5 ? years : months)++;
  CHECK(years == 0);
  CHECK(months == 11);
  CHECK(f.store->snapshot()->queue(onlyEdge(*f.store), Resolution::Min5).back().time == expectedKeys(f.site, Resolution::Min5).back());
}

TEST(ingest_a_file_that_fails_leaves_the_resolution_as_it_was) {
  Fixture f;
  f.site.step = 360;
  f.run({Resolution::Day});
  const EdgeKey edge = onlyEdge(*f.store);
  const auto before = keysOf(f.store->snapshot()->queue(edge, Resolution::Day));
  CHECK(!before.empty());
  // a fresh cache and a site that breaks after the latest file
  TempDir other;
  struct Breaking : Http {
    FakeSite & site;
    int posts = 0;
    explicit Breaking(FakeSite & s) : site(s) {}
    HttpResult get(const std::string & u, const HttpHeaders & h) override { return site.get(u, h); }
    HttpResult post(const std::string & u, const std::string & b, const HttpHeaders & h) override {
      if (++posts > 1) {
        HttpResult r;
        r.status = 500;
        return r;
      }
      return site.post(u, b, h);
    }
  } breaking(f.site);
  HistData broken(breaking, market::RawCache(other.path), fastSettings());
  market::IngestOptions options;
  options.codes = {"eurusd"};
  options.resolutions = {Resolution::Day};
  const auto report = market::runIngest(broken, *f.store, options, kQuiet);
  CHECK(!report.complete());
  CHECK(!report.instruments[0].failed.empty() && !report.instruments[0].skipped.empty());
  CHECK(keysOf(f.store->snapshot()->queue(edge, Resolution::Day)) == before);
}

TEST(ingest_makes_its_instruments_as_the_sources_do) {
  Fixture f;
  f.site.step = 360;
  f.run({Resolution::Day}, false, {"eurusd", "xauusd", "spxusd"});
  const auto snapshot = f.store->snapshot();
  const auto edges = snapshot->quoteRowsSince(0);
  CHECK(edges.size() == 3);
  for (const EdgeKey & e : edges) {
    const Listing l = *snapshot->listing(e);
    CHECK(text(NodeKey{e.dataset.value}) == "HISTDATA" && snapshot->name(NodeRef(e.dataset)) == "HistData.com");
    CHECK(text(NodeKey{e.market.value}) == "XXXX");
    if (l.symbol == "EURUSD") {
      // a pair of two currencies, found by their codes
      CHECK(l.name == "EUR/USD" && snapshot->kind(e.base) == InstrumentKind::Currency && text(NodeKey{e.base.value}) == "EUR" && text(NodeKey{e.quote.value}) == "USD");
    } else if (l.symbol == "XAUUSD") {
      // gold, the one the other sources have, by its ISIN
      CHECK(l.name == "XAU/USD" && snapshot->kind(e.base) == InstrumentKind::Commodity && snapshot->isin(e.base) == known_isin::kGold);
      CHECK(text(NodeKey{e.quote.value}) == "USD");
    } else {
      CHECK(l.symbol == "SPXUSD" && l.name == "SPX/USD");
      CHECK(snapshot->kind(e.base) == InstrumentKind::Index && snapshot->isin(e.base) == known_isin::kSp500 && snapshot->symbol(e.base) == "SPX");
      CHECK(snapshot->symbol(e.quote) == kPointsUnit && snapshot->kind(e.quote) == InstrumentKind::Unit);
    }
  }
}

TEST(ingest_an_unknown_code_is_reported_and_a_pair_with_no_data_this_year_is_left_out) {
  Fixture f;
  f.site.lastBar = utcOfEst(2025, 6, 13, 16, 0, 0);  // the data stopped last year; the clock says November 2026
  const auto report = f.run({Resolution::Min5}, false, {"eurusd", "nonesuch"});
  CHECK(report.unknownCodes == std::vector<std::string>{"nonesuch"});
  CHECK(report.inactive == 1 && report.instruments.size() == 1 && report.instruments[0].inactive && report.instruments[0].series.empty());
  CHECK(!report.complete());
  CHECK(f.store->snapshot()->quoteRowCount() == 0);  // nothing is carried for it
  CHECK(market::formatReport(report).find("left out") != std::string::npos);
  CHECK(f.site.downloads() == 11);  // the months of this year, and nothing of the years before
}

TEST(ingest_formats_times) {
  CHECK(market::formatTime(1794607080) == "2026-11-13T21:58:00Z");
  CHECK(market::formatTime(0) == "1970-01-01T00:00:00Z");
}

TEST(ingest_writes_every_bar_it_builds_to_the_bar_store) {
  Fixture f;
  f.site.step = 360;
  TempDir bars;
  StoreOptions options;
  options.invent_isins = false;
  options.bars = openSqliteBarStore(bars.path / "bars.sqlite");
  const auto barStore = options.bars;
  f.store = makeMemoryStore(std::move(options));
  f.run(kAll);
  const EdgeKey edge = onlyEdge(*f.store);
  for (Resolution r : kAll) {
    std::vector<int64_t> stored;
    barStore->read(edge, r, 0, INT64_MAX, [&](const Candle & c) { stored.push_back(c.time); });
    CHECK(stored == expectedKeys(f.site, r));
  }
}

TEST(ingest_to_a_database_and_the_server_reads_it_back_whatever_the_database) {
  forEachDatabase([](const market::DbConfig & db, const char * backend) {
    std::printf("  %s\n", backend);
    Fixture f;
    f.site.step = 360;
    {
      f.store = makeMemoryStore(market::storeOptionsOf(db, {}));
      const auto report = f.run(kAll, false, {"eurusd", "xauusd"});
      CHECK(report.complete());
    }  // the store is gone: what is left is the database's
    auto server = std::shared_ptr<Store>(makeMemoryStore(market::storeOptionsOf(db, {})));
    market::loadQueues(*server);
    const auto instruments = nlohmann::json::parse(market::handleApi(*server, "/instruments", {}).body);
    CHECK(instruments["instruments"].size() == 2);
    const auto summary = nlohmann::json::parse(market::handleApi(*server, "/summary", {{"edge", "EUR/USD/XXXX/HISTDATA"}}).body)["instruments"][0];
    CHECK(summary["edge"]["dataset"] == "HISTDATA" && summary["listing"]["symbol"] == "EURUSD");
    CHECK(summary["last_bar_time"].get<int64_t>() > kLast - 86400 && summary["last_bar_time"].get<int64_t>() <= kLast);
    CHECK(summary["queues"].contains("5m") && summary["queues"].contains("1h") && summary["queues"].contains("1d") && summary["queues"].contains("1w"));
    CHECK(summary["queues"]["5m"]["count"].get<int>() >= 2 && summary["queues"]["5m"]["volume"][0].is_null());
    // the bars of the bar store are the full doubles, the queues the model's records
    const EdgeKey edge = onlyEdge(*server);
    CHECK(!server->snapshot()->queue(edge, Resolution::Day).empty());
  });
}

// ---- a month that was cached before it was over, a site that does not answer, a store that cannot write ----

namespace {

int postsFor(const FakeSite & site, const std::string & period) {
  int n = 0;
  for (const auto & r : site.requests)
    if (r.rfind("POST", 0) == 0 && r.size() > period.size() && r.compare(r.size() - period.size(), period.size(), period) == 0) n++;
  return n;
}

}  // namespace

TEST(ingest_asks_again_once_for_a_month_that_was_cached_before_it_was_over_and_fills_the_gap) {
  Fixture f;
  f.site.step = 360;
  // November, with only its first days on the site, and the clock in the middle of it
  f.site.lastBar = utcOfEst(2026, 11, 9, 12, 0, 0);
  f.run({Resolution::Day}, false, {"eurusd"}, utcOfEst(2026, 11, 10, 12, 0, 0));
  const EdgeKey edge = onlyEdge(*f.store);
  const auto lastDay = [&] { return f.store->snapshot()->queue(edge, Resolution::Day).back().time; };
  CHECK(lastDay() == market::barKey(Resolution::Day, utcOfEst(2026, 11, 9, 12, 0, 0)));
  const int64_t day20 = market::barKey(Resolution::Day, utcOfEst(2026, 11, 20, 12, 0, 0));
  CHECK(keysOf(f.store->snapshot()->queue(edge, Resolution::Day)).back() < day20);  // the gap

  // The month is over and the site has all of it. The clock is in December, where there is no file yet.
  f.site.lastBar = utcOfEst(2026, 11, 30, 12, 0, 0);
  f.site.requests.clear();
  const Timestamp december = utcOfEst(2026, 12, 2, 12, 0, 0);
  const auto report = f.run({Resolution::Day}, false, {"eurusd"}, december);
  CHECK(report.complete());
  CHECK(postsFor(f.site, "202611") == 1);  // asked for again, though it is in the cache
  CHECK(postsFor(f.site, "202610") == 0 && postsFor(f.site, "202601") == 0);  // the months that were over when they were fetched are final
  CHECK(lastDay() == market::barKey(Resolution::Day, utcOfEst(2026, 11, 30, 12, 0, 0)));
  const auto keys = keysOf(f.store->snapshot()->queue(edge, Resolution::Day));
  CHECK(std::find(keys.begin(), keys.end(), day20) != keys.end());  // the gap is filled

  // And once: it was fetched after the month was over, so it is final.
  f.site.requests.clear();
  f.run({Resolution::Day}, false, {"eurusd"}, december);
  CHECK(postsFor(f.site, "202611") == 0);
}

TEST(ingest_a_shorter_download_never_replaces_a_longer_cached_month) {
  Fixture f;
  f.site.step = 360;
  f.site.lastBar = utcOfEst(2026, 11, 20, 12, 0, 0);
  f.run({Resolution::Day});
  const EdgeKey edge = onlyEdge(*f.store);
  const int64_t last = f.store->snapshot()->queue(edge, Resolution::Day).back().time;
  f.site.lastBar = utcOfEst(2026, 11, 5, 12, 0, 0);  // the site now gives less
  f.store = plainStore();
  f.run({Resolution::Day}, /*refresh=*/true);
  CHECK(f.store->snapshot()->queue(onlyEdge(*f.store), Resolution::Day).back().time == last);
  const auto cached = market::RawCache(f.dir.path).read("DAT_ASCII_EURUSD_M1_202611.zip");
  CHECK(cached.has_value() && cached->size() > 0);
}

TEST(ingest_a_page_with_no_token_is_a_failure_that_is_tried_again_and_not_a_pair_that_stopped) {
  Fixture f;
  f.site.tokens = false;  // a challenge or error page for every request
  const auto report = f.run({Resolution::Min5});
  CHECK(report.inactive == 0);
  CHECK(!report.complete());
  CHECK(report.instruments.size() == 1 && !report.instruments[0].inactive && !report.instruments[0].failed.empty());
  int gets = 0, posts = 0;
  for (const auto & r : f.site.requests) (r.rfind("GET", 0) == 0 ? gets : posts)++;
  CHECK(gets == 4 && posts == 0);  // the list page, then the file's page three times
  CHECK(f.store->snapshot()->quoteRowCount() == 0);
}

TEST(ingest_a_run_in_which_every_instrument_came_out_inactive_is_not_complete) {
  Fixture f;
  f.site.lastBar = utcOfEst(2025, 6, 13, 16, 0, 0);
  const auto report = f.run({Resolution::Min5});  // no unknown code, nothing failed: one instrument, and it is inactive
  CHECK(report.unknownCodes.empty() && report.instruments.size() == 1 && report.instruments[0].failed.empty());
  CHECK(report.inactive == 1);
  CHECK(!report.complete());
  CHECK(market::formatReport(report).find("may not be answering") != std::string::npos);
  // but one that is left out beside the others that were loaded is only a pair that stopped
  Fixture g;
  g.site.step = 360;
  const auto mixed = g.run({Resolution::Day}, false, {"eurusd"});
  CHECK(mixed.complete());
}

namespace {

struct FullBars : BarStore {
  void apply(const BarBatch &) override { throw std::runtime_error("the medium is full"); }
  void read(const EdgeKey &, Resolution, int64_t, int64_t, const std::function<void(const Candle &)> &) override {}
};

}  // namespace

TEST(ingest_says_so_when_the_store_could_not_write_what_it_was_given) {
  Fixture f;
  StoreOptions options;
  options.invent_isins = false;
  options.bars = std::make_shared<FullBars>();
  options.bar_writer.delay = std::chrono::milliseconds(0);
  f.store = makeMemoryStore(std::move(options));
  const auto report = f.run({Resolution::Min5});
  CHECK(report.failedWrites >= 1);
  CHECK(!report.complete());
  CHECK(market::formatReport(report).find("writes to the database failed") != std::string::npos);
  CHECK(report.instruments[0].failed.empty());  // the loading itself went well
}
