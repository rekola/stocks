#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Ingest.h"
#include "market/Store.h"

#include <algorithm>
#include <set>

using namespace histdata;
using market::Level;

namespace {

const market::Log kQuiet = [](const std::string &) {};
const Timestamp kLast = 1794603600;  // 2026-11-13 21:00 UTC: 16:00 EST, a row of the made-up site (the real file ends 21:58)

market::InstrumentRecord only(market::MarketStore & store) {
  const auto all = store.instruments();
  return all.empty() ? market::InstrumentRecord{} : all[0].instrument;
}

// What the made-up site's rows say a level should hold, counted the slow way: the distinct keys of the rows in the window.
std::vector<int64_t> expectedKeys(const FakeSite & site, Level level) {
  std::set<int64_t> keys;
  std::vector<market::MinuteBar> all;
  for (int y = 2015; y <= 2026; y++) {
    if (y == 2026) {
      for (int m = 1; m <= 12; m++) {
        char p[8];
        std::snprintf(p, sizeof p, "%04d%02d", y, m);
        for (auto & r : site.rows(p)) all.push_back(r);
      }
    } else {
      for (auto & r : site.rows(std::to_string(y))) all.push_back(r);
    }
  }
  const Resolution bar = market::barOf(level);
  const int64_t last = market::barKey(bar, site.lastBar);
  const int64_t first = market::windowFirstKey(level, site.lastBar);
  for (const auto & r : all) {
    const int64_t k = market::barKey(bar, r.time);
    if (k >= first && k <= last) keys.insert(k);
  }
  return {keys.begin(), keys.end()};
}

struct Fixture {
  FakeSite site;
  TempDir dir;
  std::unique_ptr<market::MarketStore> store = market::makeMemoryStore();
  Fixture() {
    site.lastBar = kLast;
    site.step = 120;
    site.firstYear = 2000;
  }
  market::IngestReport run(std::vector<Level> levels, bool refresh = false, std::vector<std::string> codes = {"eurusd"}) {
    HistData adapter(site, market::RawCache(dir.path), fastSettings());
    market::IngestOptions options;
    options.codes = codes;
    options.levels = levels;
    options.refresh = refresh;
    return market::runIngest(adapter, *store, options, kQuiet);
  }
};

}  // namespace

TEST(ingest_level_5m_downloads_one_file_when_the_day_is_in_one_month) {
  Fixture f;
  const auto report = f.run({Level::Day});
  CHECK(f.site.downloads() == 1);
  CHECK(report.complete());
  CHECK(report.instruments.size() == 1 && report.instruments[0].levels.size() == 1);
  const auto bars = f.store->bars(only(*f.store).edge, Level::Day, 0, INT64_MAX);
  std::vector<int64_t> keys;
  for (const auto & b : bars) keys.push_back(b.key);
  CHECK(keys == expectedKeys(f.site, Level::Day));
  CHECK(!bars.empty() && bars.back().key == market::barKey(Resolution::Min5, kLast));
  // nothing of the other levels
  CHECK(f.store->bars(only(*f.store).edge, Level::Year, 0, INT64_MAX).empty());
}

TEST(ingest_level_5m_downloads_two_files_when_the_day_spans_a_month_boundary) {
  Fixture f;
  f.site.lastBar = utcOfEst(2026, 11, 1, 3, 0, 0);  // 3 am EST on the 1st: the day before is in October
  f.site.step = 30;
  const auto report = f.run({Level::Day});
  CHECK(f.site.downloads() == 2);
  CHECK(report.complete());
  const auto bars = f.store->bars(only(*f.store).edge, Level::Day, 0, INT64_MAX);
  CHECK(!bars.empty() && bars.front().key < market::barKey(Resolution::Min5, utcOfEst(2026, 11, 1, 0, 0, 0)));
}

TEST(ingest_all_levels_hold_the_windows_of_the_rows_and_each_file_is_asked_for_once) {
  Fixture f;
  f.site.step = 360;  // a few rows a day: the ten years are a lot of files, not of rows
  const auto report = f.run({Level::Day, Level::Week, Level::Month, Level::Year, Level::Years3, Level::Years10});
  CHECK(report.complete());
  const market::Edge edge = only(*f.store).edge;
  for (Level level : market::kLevels) {
    std::vector<int64_t> keys;
    for (const auto & b : f.store->bars(edge, level, 0, INT64_MAX)) keys.push_back(b.key);
    CHECK(keys == expectedKeys(f.site, level));
    CHECK(!keys.empty());
  }
  // ten yearly files (2016 to 2025) and the eleven months of this year, one request pair each
  CHECK(f.site.downloads() == 21);
  std::set<std::string> posts;
  for (const auto & r : f.site.requests)
    if (r.rfind("POST", 0) == 0) posts.insert(r);
  CHECK(posts.size() == 21);
  int yearly = 0, monthly = 0;
  for (const auto & r : posts) (r.size() - r.rfind(' ') - 1 == 4 ? yearly : monthly)++;
  CHECK(yearly == 10 && monthly == 11);  // never the months of a past year, nor the whole of this one
}

TEST(ingest_bars_are_first_open_max_high_min_low_last_close) {
  Fixture f;
  f.run({Level::Week});
  const market::Edge edge = only(*f.store).edge;
  const auto bars = f.store->bars(edge, Level::Week, 0, INT64_MAX);
  // recompute the last hour from the site's own rows
  std::vector<market::MinuteBar> mine;
  for (const auto & r : f.site.rows("202611"))
    if (market::barKey(Resolution::Hour1, r.time) == bars.back().key) mine.push_back(r);
  CHECK(!mine.empty());
  double high = 0, low = 1e9;
  for (const auto & m : mine) high = std::max(high, m.high), low = std::min(low, m.low);
  CHECK_NEAR(bars.back().open, mine.front().open, 1e-6);
  CHECK_NEAR(bars.back().close, mine.back().close, 1e-6);
  CHECK_NEAR(bars.back().high, high, 1e-6);
  CHECK_NEAR(bars.back().low, low, 1e-6);
}

TEST(ingest_a_second_run_with_a_warm_cache_makes_no_requests) {
  Fixture f;
  f.site.step = 360;
  f.run({Level::Day, Level::Week, Level::Month, Level::Year, Level::Years3, Level::Years10});
  const size_t before = f.site.requests.size();
  CHECK(before > 0);
  const auto again = f.run({Level::Day, Level::Week, Level::Month, Level::Year, Level::Years3, Level::Years10});
  CHECK(f.site.requests.size() == before);
  CHECK(again.complete());
  CHECK(again.instruments[0].downloads == 0 && again.instruments[0].cached == 21);
}

TEST(ingest_replaces_a_level_and_nothing_old_remains) {
  Fixture f;
  f.run({Level::Day});
  const market::Edge edge = only(*f.store).edge;
  const size_t first = f.store->bars(edge, Level::Day, 0, INT64_MAX).size();
  // an old bar that the next run does not produce
  f.store->replaceBars(edge, Level::Day, {{5, 1, 1, 1, 1}}, 1, 99);
  f.run({Level::Day});
  const auto bars = f.store->bars(edge, Level::Day, 0, INT64_MAX);
  CHECK(bars.size() == first && bars.front().key > 5);
}

TEST(ingest_refresh_asks_again_for_this_years_months_only) {
  Fixture f;
  f.site.step = 360;
  f.run({Level::Day, Level::Week, Level::Month, Level::Year, Level::Years3, Level::Years10});
  f.site.requests.clear();
  f.site.lastBar += 3 * 86400;  // the site has grown (a month's file does)
  f.run({Level::Day, Level::Week, Level::Month, Level::Year, Level::Years3, Level::Years10}, /*refresh=*/true);
  int years = 0, months = 0;
  for (const auto & r : f.site.requests)
    if (r.rfind("POST", 0) == 0) (r.size() - r.rfind(' ') == 5 ? years : months)++;
  CHECK(years == 0);
  CHECK(months == 11);
}

TEST(ingest_a_file_that_fails_leaves_the_level_as_it_was) {
  Fixture f;
  f.site.step = 360;
  f.run({Level::Year});
  const market::Edge edge = only(*f.store).edge;
  const auto before = f.store->bars(edge, Level::Year, 0, INT64_MAX);
  // a fresh cache and a site that breaks after the latest file
  TempDir other;
  HistData adapter(f.site, market::RawCache(other.path), fastSettings());
  f.site.failDownloads = 0;
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
  options.levels = {Level::Year};
  const auto report = market::runIngest(broken, *f.store, options, kQuiet);
  CHECK(!report.complete());
  CHECK(!report.instruments[0].failed.empty() && !report.instruments[0].skipped.empty());
  CHECK(f.store->bars(edge, Level::Year, 0, INT64_MAX) == before);
}

TEST(ingest_makes_the_instrument_record_and_names_the_dataset) {
  Fixture f;
  f.run({Level::Day});
  const auto all = f.store->instruments();
  CHECK(all.size() == 1);
  CHECK(all.size() == 1 && all[0].instrument.code == "eurusd" && all[0].instrument.name == "EUR/USD" && all[0].instrument.assetClass == "currency");
  CHECK(all.size() == 1 && all[0].instrument.firstPeriod == 200005);
  CHECK(all.size() == 1 && text(NodeKey{all[0].instrument.edge.dataset}) == "HISTDATA" && text(NodeKey{all[0].instrument.edge.base}) == "EUR");
  CHECK(all.size() == 1 && all[0].levels.size() == 1 && all[0].levels[0].lastBarTime == kLast);
}

TEST(ingest_an_unknown_code_is_reported_and_a_pair_with_no_data_this_year_is_left_out) {
  Fixture f;
  f.site.lastBar = 0;  // the site has nothing
  const auto report = f.run({Level::Day}, false, {"eurusd", "nonesuch"});
  CHECK(report.unknownCodes == std::vector<std::string>{"nonesuch"});
  CHECK(report.inactive == 1);
  CHECK(!report.complete());
  CHECK(report.instruments.size() == 1 && report.instruments[0].inactive && report.instruments[0].levels.empty());
  CHECK(f.store->instruments().empty());  // nothing is carried for a pair with no data this year
  CHECK(market::formatReport(report).find("left out") != std::string::npos);
}

TEST(ingest_formats_times) {
  CHECK(market::formatTime(1794607080) == "2026-11-13T21:58:00Z");
  CHECK(market::formatTime(0) == "1970-01-01T00:00:00Z");
}

TEST(ingest_a_pair_that_stops_being_quoted_is_forgotten) {
  Fixture f;
  f.run({Level::Day});
  CHECK(f.store->instruments().size() == 1);
  f.site.lastBar = utcOfEst(2025, 6, 13, 16, 0, 0);  // the data stops: nothing in 2026 on the next run
  TempDir other;
  HistData adapter(f.site, market::RawCache(other.path), fastSettings());
  market::IngestOptions options;
  options.codes = {"eurusd"};
  options.levels = {Level::Day};
  const auto report = market::runIngest(adapter, *f.store, options, kQuiet);
  CHECK(report.inactive == 1 && f.store->instruments().empty());
  CHECK(f.store->bars(market::Edge{currencyKey("EUR")->value, currencyKey("USD")->value, marketKey("XXXX")->value, datasetKey("HISTDATA")->value},
                      Level::Day, 0, INT64_MAX).empty());
}
