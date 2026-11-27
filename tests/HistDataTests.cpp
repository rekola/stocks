#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Bars.h"
#include "market/offline/HistData.h"
#include "source/KnownIsins.h"

#include <algorithm>
#include <set>

using namespace histdata;

namespace {
constexpr const char * kSampleRow = "20120201 000100;1.306570;1.306570;1.306470;1.306560;0";
constexpr Timestamp kSampleUtc = 1328072460;  // 2012-02-01 05:01:00 UTC
}  // namespace

TEST(histdata_sample_row_is_utc_0501_with_hour_368909_and_day_734534) {
  const auto row = parseRow(kSampleRow);
  CHECK(row.has_value());
  if (!row) return;
  CHECK(row->time == kSampleUtc);
  CHECK(row->open == 1.30657 && row->high == 1.30657 && row->low == 1.30647 && row->close == 1.30656);
  CHECK(market::barKey(Resolution::Hour1, row->time) == 368909);
  CHECK(market::barKey(Resolution::Day, row->time) == 734534);
}

TEST(histdata_est_is_utc_minus_five_all_year_round) {
  CHECK(utcOfEst(2012, 2, 1, 0, 1, 0) == kSampleUtc);
  // summer: no daylight saving, still 5 hours (New York would be 4)
  CHECK(utcOfEst(2012, 7, 1, 12, 0, 0) % 86400 == 17 * 3600);
  CHECK(utcOfEst(2012, 11, 4, 0, 0, 0) % 86400 == 5 * 3600);  // the day the US clocks went back
  CHECK(utcOfEst(2012, 3, 11, 0, 0, 0) % 86400 == 5 * 3600);  // and forward
  // the evening of New Year's Eve is the next year in UTC
  CHECK(utcOfEst(2011, 12, 31, 20, 0, 0) == 1325379600);  // 2012-01-01 01:00 UTC
  CHECK(market::barKey(Resolution::Day, utcOfEst(2011, 12, 31, 20, 0, 0)) == market::barKey(Resolution::Day, utcOfEst(2012, 1, 1, 0, 0, 0)));
  // a leap day
  CHECK(utcOfEst(2012, 2, 29, 19, 0, 0) == 1330560000);  // 2012-03-01 00:00 UTC
}

TEST(histdata_parses_rows_and_leaves_out_what_is_not_a_row) {
  const std::string csv = std::string(kSampleRow) + "\r\n\r\n20120201 000200;1.306560;1.306600;1.306500;1.306590;0\n" +
                          "not a row\n20120201 0003;1;1;1;1;0\n20121301 000000;1;1;1;1;0\n20120201 000400;1.3;x;1.3;1.3;0\n" +
                          "20120201 000500;1.3;1.4;1.2;1.35";  // no volume, no final newline
  const CsvResult parsed = parseCsv(csv);
  CHECK(parsed.minutes.size() == 3);
  CHECK(parsed.badLines == 4);
  CHECK(parsed.minutes.size() == 3 && parsed.minutes[2].time == kSampleUtc + 4 * 60 && parsed.minutes[2].high == 1.4);
  CHECK(parseCsv("").minutes.empty() && parseCsv("").badLines == 0);
}

TEST(histdata_names_files_pages_and_forms) {
  CHECK(fileName("eurusd", {2026, 11}) == "DAT_ASCII_EURUSD_M1_202611.zip");
  CHECK(fileName("EURUSD", {2025, 0}) == "DAT_ASCII_EURUSD_M1_2025.zip");
  CHECK(periodText({2026, 3}) == "202603");
  const std::string base = "https://www.histdata.com/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/eurusd";
  CHECK(pageUrl("eurusd", {2025, 0}) == base + "/2025");
  CHECK(pageUrl("EURUSD", {2026, 3}) == base + "/2026/3");  // not zero-padded
  CHECK(formBody("abc", "eurusd", {2026, 3}) == "tk=abc&date=2026&datemonth=202603&platform=ASCII&timeframe=M1&fxpair=EURUSD");
  CHECK(formBody("abc", "eurusd", {2025, 0}) == "tk=abc&date=2025&datemonth=2025&platform=ASCII&timeframe=M1&fxpair=EURUSD");
}

TEST(histdata_reads_the_token_of_a_page) {
  CHECK(extractToken("<form><input type=\"hidden\" name=\"tk\" id=\"tk\" value=\"e2685b2229743f641332d9ff175b89ad\" /></form>") ==
        "e2685b2229743f641332d9ff175b89ad");
  CHECK(extractToken("<input value=\"abc123\" id=\"tk\" type=\"hidden\">") == "abc123");
  CHECK(extractToken("<html>no form here</html>").empty());
  CHECK(extractToken("<input id=\"tk\" value=\"a&b=c\">").empty());  // not a token: it would go into the form unescaped
  CHECK(extractToken("<input id=\"tk\">").empty());
}

TEST(histdata_files_that_cover_a_window) {
  const Timestamp nov13 = utcOfEst(2026, 11, 13, 16, 58, 0);
  // inside the current month: one monthly file
  CHECK((periodsCovering(nov13 - 86400, nov13, 2026, 200005) == std::vector<Period>{{2026, 11}}));
  // across a month boundary: two
  const Timestamp dec1 = utcOfEst(2026, 12, 1, 2, 0, 0);
  CHECK((periodsCovering(dec1 - 86400, dec1, 2026, 200005) == std::vector<Period>{{2026, 11}, {2026, 12}}));
  // the month is the EST month: 2026-12-01 03:00 UTC is still 30 November in EST
  CHECK((periodsCovering(utcOfEst(2026, 11, 30, 22, 0, 0), utcOfEst(2026, 11, 30, 23, 0, 0), 2026, 0) == std::vector<Period>{{2026, 11}}));
  // a stretch of 31 days from 1 May reaches back three months
  const Timestamp may1 = utcOfEst(2026, 5, 1, 9, 0, 0);
  CHECK(periodsCovering(utcOfEst(2026, 4, 1, 0, 0, 0) - 86400, may1, 2026, 0).size() == 3);
  // ten years: ten yearly files and the months of this year
  const auto ten = periodsCovering(market::windowStartTime(Resolution::Week, nov13), nov13, 2026, 200005);
  CHECK(ten.size() == 10 + 11);
  CHECK(ten.front() == (Period{2016, 0}) && ten[9] == (Period{2025, 0}) && ten[10] == (Period{2026, 1}) && ten.back() == (Period{2026, 11}));
  // a year that is past is one file, even when the window starts in its last days (no months of past years)
  CHECK((periodsCovering(utcOfEst(2025, 12, 30, 0, 0, 0), utcOfEst(2026, 1, 2, 0, 0, 0), 2026, 0) == std::vector<Period>{{2025, 0}, {2026, 1}}));
  // nothing before the instrument's first month
  CHECK((periodsCovering(utcOfEst(2000, 1, 1, 0, 0, 0), utcOfEst(2000, 12, 1, 0, 0, 0), 2026, 200005) == std::vector<Period>{{2000, 0}}));
  CHECK(periodsCovering(utcOfEst(1999, 1, 1, 0, 0, 0), utcOfEst(1999, 12, 1, 0, 0, 0), 2026, 200005).empty());
}

TEST(histdata_reads_the_pair_list_page) {
  const std::string page =
      "<table><tr><td><a href=\"/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/EURUSD\" title=\"x\"><strong>EUR/USD</strong></a>"
      "<br/>(2000/May)</td><td><a href=\"/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/USDCZK\" title=\"y\"><strong>USD/CZK</strong></a>"
      "<br/>(2010/August)</td></tr><tr><td><a href=\"/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/NEWPAR\"><strong>NEW/PAR</strong></a>"
      "<br/>(2024/January)</td><td><a href=\"/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/EURUSD/2020\">2020</a></td></tr></table>";
  const auto pairs = parseDiscovery(page);
  CHECK(pairs.size() == 3);
  if (pairs.size() != 3) return;
  CHECK(pairs[0].code == "eurusd" && pairs[0].name == "EUR/USD" && pairs[0].baseSymbol == "EUR" && pairs[0].quote == "USD");
  CHECK(pairs[0].firstPeriod == 200005 && pairs[0].baseKind == InstrumentKind::Currency);
  CHECK(pairs[1].code == "usdczk" && pairs[1].firstPeriod == 201008);
  CHECK(pairs[2].code == "newpar" && pairs[2].baseKind == InstrumentKind::Currency);
  CHECK(parseDiscovery("<html>nothing</html>").empty());
}

TEST(histdata_fallback_list_is_the_one_in_the_task) {
  const auto & list = fallbackInstruments();
  CHECK(list.size() == 66);
  std::set<std::string> codes;
  for (const auto & s : list) codes.insert(s.code);
  CHECK(codes.size() == 66);
  const auto find = [&](const std::string & code) { return *std::find_if(list.begin(), list.end(), [&](const auto & s) { return s.code == code; }); };
  CHECK(find("eurusd").firstPeriod == 200005 && find("eurusd").name == "EUR/USD" && find("eurusd").symbol == "EURUSD");
  CHECK(find("usdczk").firstPeriod == 201008 && find("gbpjpy").firstPeriod == 200205 && find("auxaud").firstPeriod == 201011);
  // an FX pair is an edge between two currencies: no ISIN
  CHECK(find("eurusd").baseKind == InstrumentKind::Currency && find("eurusd").baseSymbol == "EUR" && find("eurusd").quote == "USD" && find("eurusd").baseIsin.empty());
  // gold and silver are the commodity by the ISIN the sources share, oil and the indices are instruments by the ISINs we were given
  CHECK(find("xauusd").baseKind == InstrumentKind::Commodity && find("xauusd").baseIsin == known_isin::kGold && find("xauusd").quote == "USD");
  CHECK(find("xagusd").baseIsin == known_isin::kSilver && find("xaueur").baseIsin == known_isin::kGold && find("xaueur").quote == "EUR");
  CHECK(find("wtiusd").baseIsin == known_isin::kWti && find("bcousd").baseIsin == known_isin::kBrent && find("wtiusd").name == "WTI/USD");
  CHECK(find("spxusd").baseKind == InstrumentKind::Index && find("spxusd").baseIsin == known_isin::kSp500 && find("spxusd").quote == kPointsUnit);
  for (const char * code : {"nsxusd", "udxusd", "grxeur", "frxeur", "etxeur", "ukxgbp", "jpxjpy", "hkxhkd", "auxaud"})
    CHECK(find(code).baseKind == InstrumentKind::Index && !find(code).baseIsin.empty() && find(code).quote == kPointsUnit);
}

TEST(histdata_discovery_falls_back_when_the_page_cannot_be_read) {
  FakeSite site;
  site.pairsOnPage = false;
  TempDir dir;
  HistData adapter(site, market::RawCache(dir.path), fastSettings());
  std::vector<std::string> log;
  const auto found = adapter.discover([&](const std::string & line) { log.push_back(line); }, false);
  CHECK(found.size() == fallbackInstruments().size());
  CHECK(log.size() == 1 && log[0].find("built-in list") != std::string::npos);
}

// ---- the download flow, against a made-up site ----

namespace {

histdata::HistData makeAdapter(FakeSite & site, const TempDir & dir) { return HistData(site, market::RawCache(dir.path), fastSettings()); }

market::InstrumentSpec eurusd() {
  for (const auto & spec : fallbackInstruments())
    if (spec.code == "eurusd") return spec;
  return {};
}

const market::Log kQuiet = [](const std::string &) {};

}  // namespace

TEST(histdata_flow_gets_the_page_then_posts_with_referer_and_origin) {
  FakeSite site;
  site.lastBar = utcOfEst(2026, 11, 13, 16, 0, 0);
  site.step = 30;
  TempDir dir;
  HistData adapter = makeAdapter(site, dir);
  const auto result = adapter.load(eurusd(), {{Resolution::Min5}, false}, kQuiet);
  CHECK(site.requests.size() == 2);
  CHECK(site.requests.size() == 2 && site.requests[0] == "GET https://www.histdata.com/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/eurusd/2026/11");
  CHECK(site.requests.size() == 2 && site.requests[1] == "POST https://www.histdata.com/get.php 202611");
  CHECK(site.postBodies.size() == 1 && site.postBodies[0] ==
        "tk=0123456789abcdef0123456789abcdef&date=2026&datemonth=202611&platform=ASCII&timeframe=M1&fxpair=EURUSD");
  CHECK(site.postHeaders.size() == 1 && site.postHeaders[0] ==
        (HttpHeaders{{"Referer", "https://www.histdata.com/download-free-forex-historical-data/?/ascii/1-minute-bar-quotes/eurusd/2026/11"},
                     {"Origin", "https://www.histdata.com"}}));
  CHECK(result.downloads == 1 && result.cached == 0 && result.failed.empty());
  CHECK(result.lastBarTime == site.lastBar);
  // saved exactly as it came, under the task's name
  const auto saved = market::RawCache(dir.path).read("DAT_ASCII_EURUSD_M1_202611.zip");
  CHECK(saved.has_value() && saved->compare(0, 2, "PK") == 0);
}

TEST(histdata_walks_back_past_months_with_a_token_and_no_data) {
  FakeSite site;
  site.lastBar = utcOfEst(2026, 9, 18, 16, 0, 0);  // the site's data ends in September; the clock says November
  site.step = 60;
  TempDir dir;
  HistData adapter = makeAdapter(site, dir);
  const auto result = adapter.load(eurusd(), {{Resolution::Min5}, false}, kQuiet);
  CHECK(result.lastBarTime == site.lastBar);
  // November and October answered with a token and an empty body; September was the first with a file
  CHECK(site.downloads() == 3);
  CHECK(result.series.size() == 1 && !result.series[0].bars.empty() && result.series[0].bars.back().time == market::barKey(Resolution::Min5, site.lastBar));
  CHECK(!market::RawCache(dir.path).has("DAT_ASCII_EURUSD_M1_202611.zip"));  // nothing cached for what is not there
}

TEST(histdata_a_failing_download_is_retried_twice_then_given_up) {
  FakeSite site;
  site.lastBar = utcOfEst(2026, 11, 13, 16, 0, 0);
  site.failDownloads = 100;
  TempDir dir;
  HistData adapter = makeAdapter(site, dir);
  std::vector<std::string> log;
  const auto result = adapter.load(eurusd(), {{Resolution::Min5, Resolution::Hour1}, false}, [&](const std::string & l) { log.push_back(l); });
  CHECK(site.downloads() == 3);  // the first try and two more
  CHECK(result.series.empty() && result.failed.size() == 1 && result.skipped.size() == 2);
  CHECK(!result.inactive);
  CHECK(std::count_if(log.begin(), log.end(), [](const std::string & l) { return l.find("trying again") != std::string::npos; }) == 2);
}

TEST(histdata_a_failure_that_goes_away_on_the_second_try_is_fine) {
  FakeSite site;
  site.lastBar = utcOfEst(2026, 11, 13, 16, 0, 0);
  site.failDownloads = 1;
  TempDir dir;
  HistData adapter = makeAdapter(site, dir);
  const auto result = adapter.load(eurusd(), {{Resolution::Min5}, false}, kQuiet);
  CHECK(result.failed.empty() && result.series.size() == 1 && site.downloads() == 2);
}

TEST(histdata_discovery_is_kept_in_the_cache_and_read_again_only_on_refresh) {
  FakeSite site;
  TempDir dir;
  HistData adapter = makeAdapter(site, dir);
  CHECK(adapter.discover(kQuiet, false).size() == 4);
  CHECK(site.requests.size() == 1);
  CHECK(adapter.discover(kQuiet, false).size() == 4);
  CHECK(site.requests.size() == 1);  // from the cache
  CHECK(adapter.discover(kQuiet, true).size() == 4);
  CHECK(site.requests.size() == 2);
}

TEST(histdata_a_pair_with_no_data_this_year_is_inactive_and_nothing_earlier_is_asked_for) {
  FakeSite site;
  site.lastBar = utcOfEst(2025, 6, 13, 16, 0, 0);  // the data stopped last year; the clock says November 2026
  site.step = 60;
  TempDir dir;
  HistData adapter = makeAdapter(site, dir);
  const auto result = adapter.load(eurusd(), {{Resolution::Min5}, false}, kQuiet);
  CHECK(result.inactive && result.activeFromYear == 2026 && result.series.empty() && result.failed.empty());
  CHECK(site.downloads() == 11);  // January to November of this year, and not the year before
}

TEST(histdata_early_in_a_year_the_year_before_still_counts) {
  FakeSite site;
  site.lastBar = utcOfEst(2026, 12, 18, 16, 0, 0);
  site.step = 60;
  TempDir dir;
  const Timestamp tenthOfJanuary = utcOfEst(2027, 1, 10, 12, 0, 0);
  HistData adapter(site, market::RawCache(dir.path), fastSettings(tenthOfJanuary));
  const auto result = adapter.load(eurusd(), {{Resolution::Min5}, false}, kQuiet);
  CHECK(!result.inactive && result.lastBarTime == site.lastBar);
  CHECK(!result.series.empty());
}

TEST(histdata_pauses_between_requests) {
  FakeSite site;
  site.lastBar = utcOfEst(2026, 11, 13, 16, 0, 0);
  site.step = 60;
  TempDir dir;
  histdata::Settings settings = fastSettings();
  settings.pause = std::chrono::milliseconds(2000);
  int sleeps = 0;
  std::chrono::milliseconds slept{0};
  settings.sleep = [&](std::chrono::milliseconds d) { sleeps++, slept += d; };
  HistData adapter(site, market::RawCache(dir.path), settings);
  adapter.load(eurusd(), {{Resolution::Min5}, false}, kQuiet);
  CHECK(site.requests.size() == 2 && sleeps == 1 && slept.count() > 1900 && slept.count() <= 2000);  // not before the first
}

TEST(histdata_a_cached_file_that_is_not_what_it_says_is_a_failed_period_and_not_an_exception) {
  FakeSite site;
  site.lastBar = utcOfEst(2026, 11, 13, 16, 0, 0);
  TempDir dir;
  // a zip whose CSV says it is four gigabytes, in the place of the latest month
  std::string zip = market::makeZip({{"DAT_ASCII_EURUSD_M1_202611.csv", "x"}}, false);
  const size_t central = zip.find(std::string("PK\x01\x02", 4));
  for (int i = 0; i < 4; i++) zip[central + 24 + static_cast<size_t>(i)] = static_cast<char>(0xff);
  std::string error;
  CHECK(market::RawCache(dir.path).store("DAT_ASCII_EURUSD_M1_202611.zip", zip, error));
  HistData adapter = makeAdapter(site, dir);
  market::LoadResult result;
  bool threw = false;
  try {
    result = adapter.load(eurusd(), {{Resolution::Min5}, false}, kQuiet);
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK(result.failed.size() == 1 && result.series.empty() && !result.inactive);
  CHECK(site.downloads() == 0);  // it is in the cache, so nothing is asked for
}
