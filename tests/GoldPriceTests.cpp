#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "source/GoldApiSource.h"
#include "source/GoldPriceHistory.h"
#include "source/GoldPriceSource.h"
#include "model/Period.h"
#include "source/PhysicalSource.h"

#include <fstream>
#include <sstream>

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/goldprice/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

struct Said : SourceListener {
  std::vector<std::string> messages;
  int changes = 0;
  void changed() override { changes++; }
  void message(const std::string & m) override { messages.push_back(m); }
};

std::string row(const std::string & symbol, const std::string & currency, const std::string & unit, const std::string & price, bool stale = false,
                const std::string & at = "2026-11-25T14:22:01.418143+00:00", const std::string & contract = "spot") {
  return "{\"symbol\":\"" + symbol + "\",\"quote_currency\":\"" + currency + "\",\"unit\":\"" + unit + "\",\"contract_type\":\"" + contract + "\",\"price\":\"" + price +
         "\",\"is_stale\":" + (stale ? "true" : "false") + ",\"computed_at\":\"" + at + "\"}";
}

}  // namespace

TEST(the_prices_answer_is_read_with_its_strings_and_what_is_not_a_spot_price_is_left_out) {
  std::vector<GoldPriceSource::Price> prices;
  std::string error;
  CHECK(GoldPriceSource::parsePrices(fixture("prices.json"), prices, error) && prices.size() == 5);
  CHECK(prices[0].symbol == "HG" && prices[0].unit == "pound" && std::abs(prices[0].price - 5.2595) < 1e-12 && prices[0].day == dayNumber(2026, 11, 25) && !prices[0].stale);
  const std::string body = "{\"symbols\":[" + row("XAU", "USD", "troy_ounce", "10") + "," + row("XAU", "EUR", "troy_ounce", "0") + "," + row("XAU", "GBP", "troy_ounce", "x") + "," +
                           row("XAU", "SEK", "", "5") + "," + row("XAU", "NOK", "troy_ounce", "5", false, "yesterday") + "," + row("GC", "USD", "troy_ounce", "9", false, "2026-11-25T00:00:00+00:00", "future") +
                           "," + row("XAG", "USD", "troy_ounce", "30", true) + ",5]}";
  CHECK(GoldPriceSource::parsePrices(body, prices, error) && prices.size() == 2 && prices[1].symbol == "XAG" && prices[1].stale);
  for (const char * bad : {"", "[]", "{}", "{\"symbols\":[]}", "{\"symbols\":[5]}"}) CHECK(!GoldPriceSource::parsePrices(bad, prices, error) && !error.empty());
}

TEST(the_prices_are_one_request_and_the_metals_are_stored_in_the_unit_of_their_instrument) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("prices.json")); };
  Said said;
  GoldPriceSource source;
  source.load(*store, http, said);
  CHECK(http.urls == std::vector<std::string>{"https://api.goldprice.dev/v1/prices"} && said.messages.empty() && said.changes == 3);  // gold, silver and copper
  const auto s = store->snapshot();
  const MarketId none = *s->findMarket(kNoMarket);
  const DatasetId data = *s->findDataset(kGoldPriceDataset);
  CHECK(s->name(NodeRef(data)) == "goldprice" && s->edges(none).size() == 4);  // XAU in EUR and USD (not SEK: it was not asked for), XAG and HG in USD
  const InstrumentId gold = *s->findInstrument("XAU"), eur = *s->findInstrument("EUR"), usd = *s->findInstrument("USD");
  CHECK(!s->findInstrument("SEK"));
  const auto last = [&](InstrumentId base, InstrumentId quote) { return s->lastCandles({base, quote, none, data}, Resolution::Day, 1).candles.back(); };
  CHECK(std::abs(last(gold, eur).close - 3179.04) < 1e-9 && std::abs(last(gold, usd).close - 3479.11) < 1e-9);
  CHECK(last(gold, eur).time == dayNumber(2026, 11, 25) && !isDefined(last(gold, eur).open));
  // Copper: the answer says "pound", and the instrument (the ISIN Nordnet's copper has) is per tonne.
  CHECK(std::abs(last(*s->findInstrument("HG"), usd).close - 5.2595 * 1000 / 0.45359237) < 1e-6);
  CHECK(s->listing({*s->findInstrument("HG"), usd, none, data})->name == "Copper, per tonne" && s->listing({gold, eur, none, data})->name == "Gold, per troy oz");
  CHECK(s->isin(gold) == "XC0009655157" && s->isin(*s->findInstrument("HG")) == "XC0005705501" && s->kind(gold) == InstrumentKind::Commodity);
  // A second load replaces the day's bar, not adds one.
  source.load(*store, http, said);
  CHECK(store->snapshot()->candles({gold, eur, none, data}, Resolution::Day).candles.size() == 1);
}

TEST(a_stale_price_is_not_stored_and_a_unit_it_does_not_know_is_said) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) {
    return ok("{\"symbols\":[" + row("XAU", "USD", "troy_ounce", "3500") + "," + row("XAU", "EUR", "troy_ounce", "3000", true) + "," + row("XAG", "USD", "gram", "1") + "," +
              row("XPT", "USD", "troy_ounce", "1000") + "]}");
  };
  Said said;
  GoldPriceSource source;
  source.load(*store, http, said);
  const auto s = store->snapshot();
  const MarketId none = *s->findMarket(kNoMarket);
  CHECK(s->edges(none).size() == 1 && !s->findInstrument("XPT"));  // the stale euro price, silver in grams and a symbol with no ISIN are not stored
  CHECK(said.messages.size() == 2 && said.messages[0].find("XAG/USD") != std::string::npos && said.messages[1].find("1 stale price") != std::string::npos);
}

TEST(a_failed_prices_request_is_said_and_writes_nothing) {
  for (const HttpResult & answer : {failed(0), failed(429), ok("not json"), ok("{\"symbols\":[]}")}) {
    auto store = makeMemoryStore();
    FakeHttp http;
    http.answer = [&](const std::string &) { return answer; };
    Said said;
    GoldPriceSource source;
    source.load(*store, http, said);
    CHECK(said.messages.size() == 1 && said.messages[0].rfind("goldprice: ", 0) == 0 && said.changes == 0 && !store->snapshot()->findDataset(kGoldPriceDataset));
  }
  GoldPriceSource source(std::chrono::minutes(7));
  CHECK(source.refreshInterval() == std::chrono::minutes(7) && source.name() == "goldprice" && source.datasetCodes() == std::vector<std::string>{kGoldPriceDataset});
  CHECK(source.historyProvider() && source.historyProvider()->dataset() == kGoldPriceDataset);
}

TEST(the_bars_answer_is_read_oldest_first_and_a_row_without_a_close_is_left_out) {
  GoldPriceHistory::Bars bars;
  std::string error;
  CHECK(GoldPriceHistory::parse(fixture("bars-XAU-USD.json"), bars, error) && bars.bars.size() == 3 && bars.next_cursor.empty());
  CHECK(bars.bars[0].time == dayNumber(2026, 11, 20) && bars.bars[2].time == dayNumber(2026, 11, 24));
  CHECK(std::abs(bars.bars[2].open - 3630.79) < 1e-9 && std::abs(bars.bars[2].high - 3678.332) < 1e-9 && std::abs(bars.bars[2].low - 3546.8679) < 1e-9 &&
        std::abs(bars.bars[2].close - 3551.34) < 1e-9 && !isDefined(bars.bars[2].volume));
  const std::string body = R"({"bars":[{"bar_start":"2026-11-24T00:00:00Z","close":"5"},{"bar_start":"2026-11-23T00:00:00Z"},{"close":"4"},{"bar_start":"x","close":"4"}],"next_cursor":"abc"})";
  CHECK(GoldPriceHistory::parse(body, bars, error) && bars.bars.size() == 1 && !isDefined(bars.bars[0].open) && bars.next_cursor == "abc");
  for (const char * bad : {"", "[]", "{}"}) CHECK(!GoldPriceHistory::parse(bad, bars, error) && !error.empty());
}

namespace {

struct Edge {
  std::unique_ptr<Store> store = makeMemoryStore();
  EdgeKey key;
  Edge() {
    FakeHttp http;
    http.answer = [](const std::string &) { return ok(fixture("prices.json")); };
    Said said;
    GoldPriceSource().load(*store, http, said);
    const auto s = store->snapshot();
    key = {*s->findInstrument("XAU"), *s->findInstrument("USD"), *s->findMarket(kNoMarket), *s->findDataset(kGoldPriceDataset)};
  }
};

}  // namespace

TEST(the_bars_of_an_edge_are_asked_by_its_symbol_and_currency_for_the_days_the_plan_has) {
  Edge edge;
  GoldPriceHistory history;
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture("bars-XAU-USD.json")); };
  CHECK(history.load(*edge.store, http, edge.key, 0, [] { return true; }) == HistoryProvider::Outcome::Ok);
  CHECK(http.urls.size() == 1 && http.urls[0].rfind("https://api.goldprice.dev/v1/bars?symbol=XAU/USD&interval=1d&from=", 0) == 0);
  CHECK(edge.store->snapshot()->candles(edge.key, Resolution::Day).candles.size() == 4);  // the three days and today's spot bar
  // The day and the week are intraday, which the plan has none of; the others read daily bars.
  const auto s = edge.store->snapshot();
  CHECK(!history.need(*s, edge.key, Period::Day) && !history.need(*s, edge.key, Period::Week));
  CHECK(history.need(*s, edge.key, Period::Month) && history.need(*s, edge.key, Period::Quarter) && history.need(*s, edge.key, Period::Year));
}

TEST(a_page_with_a_cursor_is_followed_and_what_the_plan_refuses_is_missing_not_refused) {
  Edge edge;
  GoldPriceHistory history;
  int n = 0;
  FakeHttp http;
  http.answer = [&](const std::string & url) {
    n++;
    if (n == 1) return ok(R"({"bars":[{"bar_start":"2026-11-24T00:00:00Z","close":"5"}],"next_cursor":"c1"})");
    CHECK(url.find("&cursor=c1") != std::string::npos);
    return ok(R"({"bars":[{"bar_start":"2026-11-23T00:00:00Z","close":"4"}],"next_cursor":null})");
  };
  CHECK(history.load(*edge.store, http, edge.key, 0, [] { return true; }) == HistoryProvider::Outcome::Ok && n == 2);
  CHECK(edge.store->snapshot()->candles(edge.key, Resolution::Day).candles.size() == 3);
  using Outcome = HistoryProvider::Outcome;
  for (auto [answer, outcome] : {std::pair{failed(403), Outcome::Missing}, {failed(404), Outcome::Missing}, {ok(R"({"bars":[],"next_cursor":null})"), Outcome::Missing},
                                 {ok("not json"), Outcome::Missing}, {failed(429), Outcome::Refused}, {failed(0), Outcome::Offline}, {failed(503), Outcome::Offline}}) {
    Edge other;
    FakeHttp refusing;
    refusing.answer = [&, a = answer](const std::string &) { return a; };
    CHECK(history.load(*other.store, refusing, other.key, 0, [] { return true; }) == outcome);
  }
}

TEST(of_two_sources_of_one_spot_the_newest_bar_wins_and_on_the_same_day_the_row_the_file_names_first) {
  auto store = makeMemoryStore();
  FakeHttp http;
  Said said;
  // goldprice.dev: gold 3479.11 USD on the 25th; gold-api.com: its own on the 25th.
  http.answer = [](const std::string &) { return ok(fixture("prices.json")); };
  GoldPriceSource().load(*store, http, said);
  const auto day = [&](const char * dataset) {
    const auto s = store->snapshot();
    return s->lastCandles({*s->findInstrument("XAU"), *s->findInstrument("USD"), *s->findMarket(kNoMarket), *s->findDataset(dataset)}, Resolution::Day, 1).candles.back().time;
  };
  CHECK(day(kGoldPriceDataset) == dayNumber(2026, 11, 25));
  http.answer = [](const std::string & url) {
    if (url.find("/symbols") != std::string::npos) return ok(R"([{"name":"Gold","symbol":"XAU"}])");
    const std::string currency = url.substr(url.rfind('/') + 1);
    return ok("{\"currency\":\"" + currency + "\",\"name\":\"Gold\",\"price\":3400.0,\"symbol\":\"XAU\",\"updatedAt\":\"2026-11-25T14:19:44Z\"}");
  };
  GoldApiSource(std::chrono::minutes(2), std::chrono::milliseconds(0)).load(*store, http, said);
  PhysicalTable table;
  std::string error;
  CHECK(parsePhysicalTable("spot,gold,XAU,GOLDPRICE\nspot,gold,XAU,GOLDAPI\nitem,gold,GOLDCOIN,Coin,1,1,1\n", table, error));
  PhysicalSource estimate(table);
  const auto coin = [&](const char * currency) {
    const auto s = store->snapshot();
    const DatasetId data = *s->findDataset(kPhysicalDataset);
    return s->lastCandles({*s->findInstrument("GOLDCOIN"), *s->findInstrument(currency), *s->findMarket(kOffExchange), data}, Resolution::Day, 1).candles.back().close;
  };
  estimate.load(*store, http, said);
  CHECK(std::abs(coin("USD") - 3479.11) < 1e-9);  // the same day: the first row of the file
  // goldprice.dev stops answering and gold-api.com goes on into the 26th: its newer bar wins whatever the order.
  http.answer = [](const std::string & url) {
    if (url.find("/symbols") != std::string::npos) return ok(R"([{"name":"Gold","symbol":"XAU"}])");
    const std::string currency = url.substr(url.rfind('/') + 1);
    return ok("{\"currency\":\"" + currency + "\",\"name\":\"Gold\",\"price\":3500.0,\"symbol\":\"XAU\",\"updatedAt\":\"2026-11-26T08:00:00Z\"}");
  };
  GoldApiSource(std::chrono::minutes(2), std::chrono::milliseconds(0)).load(*store, http, said);
  estimate.load(*store, http, said);
  CHECK(std::abs(coin("USD") - 3500.0) < 1e-9);
  // A row naming a dataset the store has not is skipped, and a bad row is said.
  CHECK(!parsePhysicalTable("spot,gold,XAU,A,B\nitem,gold,GOLDCOIN,Coin,1,1,1\n", table, error) && error.find("spot,METAL,SYMBOL") != std::string::npos);
}

TEST(the_gold_chart_has_a_trace_for_every_period_the_bars_reach_and_a_change_only_for_the_month) {
  Edge edge;
  GoldPriceHistory history;
  FakeHttp http;
  // The 29 days the free plan gives, ending on the day of the spot bar.
  http.answer = [](const std::string &) {
    std::string bars;
    for (int i = 0; i < 29; i++) {
      const Date d = dateOfDayNumber(dayNumber(2026, 11, 25) - 1 - i);
      char day[32];
      std::snprintf(day, sizeof day, "%04d-%02u-%02uT00:00:00Z", d.year, d.month, d.day);
      bars += std::string(i ? "," : "") + "{\"bar_start\":\"" + day + "\",\"open\":\"3500\",\"high\":\"3600\",\"low\":\"3400\",\"close\":\"" + std::to_string(3500 + i) + "\"}";
    }
    return ok("{\"bars\":[" + bars + "],\"next_cursor\":null}");
  };
  CHECK(history.load(*edge.store, http, edge.key, 0, [] { return true; }) == HistoryProvider::Outcome::Ok);
  const auto s = edge.store->snapshot();
  const PeriodView month = periodView(*s, edge.key, Period::Month, true), quarter = periodView(*s, edge.key, Period::Quarter, true), year = periodView(*s, edge.key, Period::Year, true);
  CHECK(month.closes.size() > 20 && isDefined(month.change()));
  CHECK(quarter.closes.size() > 20 && !isDefined(quarter.change()));
  CHECK(year.closes.size() > 20 && !isDefined(year.change()));
}
