#include "TestFramework.h"
#include "MarketTestSupport.h"
#include "market/Bars.h"
#include "market/Ingest.h"
#include "market/offline/Kar.h"
#include "model/Store.h"
#include "source/KnownIsins.h"

#include <map>

using namespace kar;

namespace {

const market::Log kQuiet = [](const std::string &) {};

// The price page's answer, made up: gold on 2026-11-25 and 2026-11-26 (USD at one price; EUR with the rate's two sides) and rhodium on the 25th
// in dollars only (a metal the dataset does not carry). The numbers are the real ones of those days.
const char * kAnswer = R"([
 {"Code":"AU","RateType":"P","RIC":"XAUFIXPM=","Prices":[
  {"Currency":"EUR","TradeTime":"2026-11-25T15:00:00","Bid":1.0937,"Ask":1.0939,"BidGram":101.879200330146,"AskGram":101.897830521301},
  {"Currency":"USD","TradeTime":"2026-11-25T15:14:00","Bid":3466.35,"Ask":3466.35,"BidGram":111.445657241146,"AskGram":111.445657241146},
  {"Currency":"EUR","TradeTime":"2026-11-26T15:00:00","Bid":1.0978,"Ask":1.098,"BidGram":104.794372557713,"AskGram":104.813464263407},
  {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","Bid":3578.9,"Ask":3578.9,"BidGram":115.064221068369,"AskGram":115.064221068369},
  {"Currency":"GBP","TradeTime":"2026-11-26T15:00:00","Bid":1.3,"Ask":1.3,"BidGram":90,"AskGram":90}]},
 {"Code":"RH","RateType":"S","RIC":"RH","Prices":[
  {"Currency":"USD","TradeTime":"2026-11-25T09:31:00","Bid":8900,"Ask":8900,"BidGram":286.141431028662,"AskGram":286.141431028662}]},
 {"Code":"XX","RateType":"S","RIC":"XX","Prices":[{"Currency":"USD","TradeTime":"2026-11-25T09:31:00","Bid":1,"Ask":1,"BidGram":1,"AskGram":1}]}
])";

// An empty year, as the site answers for one before its data.
const char * kEmpty = R"([{"Code":"AU","RateType":"P","RIC":"XAUFIXPM=","Prices":[]},{"Code":"RH","RateType":"S","RIC":"RH","Prices":[]}])";

class KarSite : public Http {
 public:
  std::map<int, std::string> years;  // the body for a year; a year not here is answered with 500
  int failures = 0;                  // answers to give with a 500 first
  std::vector<std::string> urls;
  std::vector<HttpHeaders> headers;
  HttpResult get(const std::string & url, const HttpHeaders & h) override {
    urls.push_back(url);
    headers.push_back(h);
    HttpResult r;
    if (failures > 0) {
      failures--;
      r.status = 500;
      return r;
    }
    const auto it = years.find(std::stoi(url.substr(url.find("from=") + 5, 4)));
    r.status = it == years.end() ? 500 : 200;
    if (it != years.end()) r.body = it->second;
    return r;
  }
};

kar::Settings fast(Timestamp now = kTestNow) {
  kar::Settings s;
  s.pause = std::chrono::milliseconds(0);
  s.now = [now] { return now; };
  s.sleep = [](std::chrono::milliseconds) {};
  return s;
}

void madeUp(KarSite & site) {
  for (int y = 2019; y <= 2026; y++) site.years[y] = y == 2026 ? kAnswer : kEmpty;
}

market::InstrumentSpec spec(const std::string & code) {
  for (const auto & s : instruments())
    if (s.code == code) return s;
  return {};
}

const Timestamp kNov26 = 1795705380;  // 2026-11-26 15:03:00 UTC
}  // namespace

TEST(kar_reads_the_answer_as_prices_per_ounce) {
  Parsed parsed;
  std::string error;
  CHECK(parse(kAnswer, parsed, error));
  // the GBP row and the rhodium and XX metals are not carried, and are not rows that failed
  CHECK(parsed.rows.size() == 4 && parsed.skipped == 0);
  const Row & usd = parsed.rows[3];
  CHECK(usd.metal == "AU" && usd.currency == "USD" && usd.time == kNov26);
  CHECK_NEAR(usd.bid, 3578.9, 0.01);  // the gram's price times 31.1035: the ounce of the fix
  CHECK(usd.bid == usd.ask);
  const Row & eur = parsed.rows[2];
  CHECK(eur.currency == "EUR" && eur.ask > eur.bid && eur.mid() > eur.bid && eur.mid() < eur.ask);
  CHECK_NEAR(eur.bid, 104.794372557713 * kGramsPerTroyOunce, 1e-9);
}

TEST(kar_leaves_out_rows_that_are_not_prices_and_refuses_what_is_not_an_answer) {
  Parsed parsed;
  std::string error;
  const std::string body = R"([{"Code":"AU","Prices":[
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","BidGram":0,"AskGram":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","BidGram":2,"AskGram":1},
    {"Currency":"USD","TradeTime":"not a time","BidGram":1,"AskGram":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","BidGram":"1","AskGram":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","BidGram":1,"AskGram":1}]}])";
  CHECK(parse(body, parsed, error));
  CHECK(parsed.rows.size() == 1 && parsed.skipped == 4);
  CHECK(!parse("", parsed, error) && !error.empty());
  CHECK(!parse("{\"error\":true}", parsed, error));
  CHECK(!parse("[]", parsed, error));
  CHECK(parse(kEmpty, parsed, error) && parsed.rows.empty());  // a year without prices is an answer
}

TEST(kar_asks_by_the_year_with_a_browsers_headers_and_the_pages_referer) {
  CHECK(url(2026) == "https://karasmussen.fi/wp-json/price-history/v1/prices?from=2026-01-01&to=2026-12-31");
  CHECK(fileName(2026) == "kar_2026.json");
  KarSite site;
  madeUp(site);
  TempDir dir;
  Kar adapter(site, market::RawCache(dir.path), fast());
  adapter.load(spec("xaueur"), {{Resolution::Day}, false}, kQuiet);
  CHECK(site.urls.size() == 8 && site.urls.front() == url(2019) && site.urls.back() == url(2026));
  bool agent = false, referer = false;
  for (const auto & [name, value] : site.headers[0]) {
    if (name == "User-Agent" && value.find("Windows NT 10.0") != std::string::npos) agent = true;
    if (name == "Referer" && value == "https://karasmussen.fi/paivan-hinnat/") referer = true;
  }
  CHECK(agent && referer);
}

TEST(kar_has_each_metal_in_five_currencies_by_the_isins_the_sources_share) {
  const auto & list = instruments();
  CHECK(list.size() == 20);
  CHECK(spec("xaueur").baseIsin == known_isin::kGold && spec("xaueur").quote == "EUR" && spec("xaueur").baseKind == InstrumentKind::Commodity);
  CHECK(spec("xagusd").baseIsin == known_isin::kSilver && spec("xptsek").baseIsin == known_isin::kPlatinum && spec("xpddkk").baseIsin == known_isin::kPalladium);
  CHECK(spec("xaueur").market == "XLBM" && spec("xagusd").market == "XLBM" && spec("xptsek").market == "XLBM" && spec("xpddkk").market == "XLBM");
  CHECK(spec("xaueur").firstPeriod == 201911 && spec("xrhusd").code.empty());  // rhodium is not carried
  KarSite site;
  Kar adapter(site, market::RawCache("unused"), fast());
  CHECK(adapter.code() == "KAR");
  CHECK(adapter.discover(kQuiet, false).size() == 20);
}

TEST(kar_builds_the_daily_and_weekly_bars_and_the_quote_of_the_last_day) {
  KarSite site;
  madeUp(site);
  TempDir dir;
  Kar adapter(site, market::RawCache(dir.path), fast());
  const auto eur = adapter.load(spec("xaueur"), {{Resolution::Min5, Resolution::Hour1, Resolution::Day, Resolution::Week}, false}, kQuiet);
  CHECK(eur.failed.empty() && eur.skipped.empty() && !eur.inactive);
  CHECK(eur.series.size() == 2);  // one price a day has no 5 minute or hourly bars
  if (eur.series.size() != 2) return;
  CHECK(eur.series[0].resolution == Resolution::Day && eur.series[0].bars.size() == 2);
  const Candle & day = eur.series[0].bars[1];
  CHECK(day.time == market::barKey(Resolution::Day, kNov26));
  CHECK(day.open == day.close && day.high == day.close && day.low == day.close && !isDefined(day.volume));
  CHECK_NEAR(day.close, (104.794372557713 + 104.813464263407) / 2 * kGramsPerTroyOunce, 1e-9);
  // the 25th and the 26th are two days of one week (a Wednesday and a Thursday)
  CHECK(eur.series[1].resolution == Resolution::Week && eur.series[1].bars.size() == 1);
  CHECK(eur.series[1].bars[0].open < eur.series[1].bars[0].close);
  CHECK(eur.quote.has_value());
  if (!eur.quote) return;
  CHECK(eur.quote->time == kNov26 - 180);  // 15:00, the euro's row (the dollar's is 15:03)
  CHECK(isDefined(eur.quote->bid) && isDefined(eur.quote->ask) && eur.quote->bid < eur.quote->ask);
  CHECK(eur.quote->bid < eur.quote->last && eur.quote->last < eur.quote->ask);
  // in dollars the fix is one price: a last, and no bid or ask to say it has two sides
  const auto usd = adapter.load(spec("xauusd"), {{Resolution::Day}, false}, kQuiet);
  CHECK(usd.quote && usd.quote->time == kNov26 && !isDefined(usd.quote->bid) && !isDefined(usd.quote->ask));
  CHECK(usd.quote && std::abs(usd.quote->last - 3578.9) < 0.01);
  CHECK(usd.lastBarTime == kNov26);
}

TEST(kar_asks_for_each_year_once_and_keeps_the_answers_in_the_cache) {
  KarSite site;
  madeUp(site);
  site.years[2027] = kEmpty;
  TempDir dir;
  {
    Kar adapter(site, market::RawCache(dir.path), fast());
    const auto first = adapter.load(spec("xaueur"), {{Resolution::Day}, false}, kQuiet);
    adapter.load(spec("xagusd"), {{Resolution::Day}, false}, kQuiet);
    adapter.load(spec("xauusd"), {{Resolution::Day}, true}, kQuiet);
    CHECK(site.urls.size() == 8 && first.downloads == 8);  // every instrument of a run reads the years already in hand, a refresh included
  }
  CHECK(market::RawCache(dir.path).has("kar_2026.json") && market::RawCache(dir.path).has("kar_2019.json"));
  {
    Kar adapter(site, market::RawCache(dir.path), fast());
    const auto again = adapter.load(spec("xaueur"), {{Resolution::Day}, false}, kQuiet);
    CHECK(site.urls.size() == 8 && again.downloads == 0 && again.cached == 8);
  }
  {
    Kar adapter(site, market::RawCache(dir.path), fast());
    adapter.load(spec("xaueur"), {{Resolution::Day}, true}, kQuiet);
    CHECK(site.urls.size() == 9 && site.urls.back() == url(2026));  // --refresh asks again for the current year only
  }
  {
    // the next year: 2026 was fetched before it was over, so it is asked for again, once; the years before it are not, and the new one is new
    Kar adapter(site, market::RawCache(dir.path), fast(kTestNow + 400 * 86400));
    adapter.load(spec("xaueur"), {{Resolution::Day}, false}, kQuiet);
    CHECK(site.urls.size() == 11 && site.urls[9] == url(2026) && site.urls[10] == url(2027));
  }
}

TEST(kar_retries_a_year_and_falls_back_to_the_cached_one) {
  KarSite site;
  madeUp(site);
  TempDir dir;
  {
    Kar adapter(site, market::RawCache(dir.path), fast());
    site.failures = 2;
    const auto loaded = adapter.load(spec("xaueur"), {{Resolution::Day}, false}, kQuiet);
    CHECK(loaded.failed.empty() && !loaded.series.empty());
    CHECK(site.urls.size() == 8 + 2);  // two answers of 500 to the first year, then the answer
  }
  {
    Kar adapter(site, market::RawCache(dir.path), fast());
    site.failures = 100;
    const auto loaded = adapter.load(spec("xaueur"), {{Resolution::Day}, true}, kQuiet);
    CHECK(loaded.failed.empty() && !loaded.series.empty() && loaded.quote);  // asked for again, not had: the copy in the cache is used
  }
  {
    TempDir empty;
    Kar adapter(site, market::RawCache(empty.path), fast());
    const auto loaded = adapter.load(spec("xaueur"), {{Resolution::Day, Resolution::Week}, false}, kQuiet);
    CHECK(!loaded.failed.empty() && loaded.series.empty() && !loaded.quote && !loaded.inactive);  // nothing had: a failure, which leaves the stored bars as they are
  }
}

TEST(kar_a_year_that_is_not_an_answer_is_a_failure_and_a_metal_with_no_recent_price_is_left_out) {
  KarSite site;
  madeUp(site);
  site.years[2023] = "<html>maintenance</html>";
  TempDir dir;
  Kar adapter(site, market::RawCache(dir.path), fast());
  const auto loaded = adapter.load(spec("xaueur"), {{Resolution::Day, Resolution::Week}, false}, kQuiet);
  CHECK(loaded.failed.size() == 1 && loaded.failed[0].rfind("2023:", 0) == 0);
  CHECK(loaded.series.empty() && loaded.skipped.size() == 2);  // a gap in the middle leaves what is stored, rather than a window with a hole
  CHECK(!market::RawCache(dir.path).has("kar_2023.json"));  // what is not an answer is not kept

  KarSite old;
  madeUp(old);
  TempDir dir2;
  Kar later(old, market::RawCache(dir2.path), fast(kTestNow + 120 * 86400));  // a quarter after the last price: not carried
  const auto left = later.load(spec("xaueur"), {{Resolution::Day}, false}, kQuiet);
  CHECK(left.inactive && left.series.empty());
}

TEST(kar_ingest_writes_the_bars_the_quote_and_the_listing_under_the_dataset_kar) {
  KarSite site;
  madeUp(site);
  TempDir dir;
  StoreOptions options;
  options.invent_isins = false;
  auto store = makeMemoryStore(std::move(options));
  Kar adapter(site, market::RawCache(dir.path), fast());
  market::IngestOptions ingest;
  ingest.codes = {"xaueur", "xauusd", "xagusd"};
  ingest.resolutions = {Resolution::Day, Resolution::Week};
  const auto report = market::runIngest(adapter, *store, ingest, kQuiet);
  // silver has no price in the made-up year: left out, which an ingest of several instruments does not call failed
  CHECK(report.instruments.size() == 3 && report.inactive == 1 && report.failedWrites == 0);
  const auto snapshot = store->snapshot();
  const auto edges = snapshot->quoteRowsSince(0);
  CHECK(edges.size() == 2);
  bool bidAsk = false, fixOnly = false;
  for (const EdgeKey & edge : edges) {
    const auto quote = snapshot->quote(edge);
    CHECK(quote.has_value() && !snapshot->queue(edge, Resolution::Day).empty());
    if (!quote) continue;
    (isDefined(quote->bid) ? bidAsk : fixOnly) = true;
  }
  CHECK(bidAsk && fixOnly);
}
