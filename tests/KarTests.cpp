#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "model/Period.h"
#include "source/KarSource.h"
#include "source/KnownIsins.h"

namespace {

// The price page's answer, made up: gold on 2026-11-25 and 2026-11-26 in dollars and in euros (the dollar's price converted: not carried), rhodium (a
// metal that is not carried) and an unknown code. The numbers are the real ones of those days.
const char * kAnswer = R"([
 {"Code":"AU","RateType":"P","RIC":"XAUFIXPM=","Prices":[
  {"Currency":"EUR","TradeTime":"2026-11-25T15:00:00","Bid":1.0937,"Ask":1.0939,"BidGram":101.879200330146,"AskGram":101.897830521301},
  {"Currency":"USD","TradeTime":"2026-11-25T15:14:00","Bid":3466.35,"Ask":3466.35,"BidGram":111.445657241146,"AskGram":111.445657241146},
  {"Currency":"EUR","TradeTime":"2026-11-26T15:00:00","Bid":1.0978,"Ask":1.098,"BidGram":104.794372557713,"AskGram":104.813464263407},
  {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","Bid":3578.9,"Ask":3578.9,"BidGram":115.064221068369,"AskGram":115.064221068369}]},
 {"Code":"AG","RateType":"A","RIC":"LDNXAG=","Prices":[
  {"Currency":"USD","TradeTime":"2026-11-26T12:00:00","Bid":41.54,"Ask":41.54,"BidGram":1.3355410162843,"AskGram":1.3355410162843}]},
 {"Code":"RH","RateType":"S","RIC":"RH","Prices":[
  {"Currency":"USD","TradeTime":"2026-11-25T09:31:00","Bid":8900,"Ask":8900,"BidGram":286.141431028662,"AskGram":286.141431028662}]},
 {"Code":"XX","RateType":"S","RIC":"XX","Prices":[{"Currency":"USD","TradeTime":"2026-11-25T09:31:00","Bid":1,"Ask":1,"BidGram":1,"AskGram":1}]}
])";

const Timestamp kNov26 = 1795705380;  // 2026-11-26 15:03:00 UTC
const Timestamp kNow = 1795737600;     // 2026-11-27 00:00:00 UTC

struct Said : SourceListener {
  std::vector<std::string> messages;
  int changes = 0;
  void changed() override { changes++; }
  void message(const std::string & m) override { messages.push_back(m); }
};

// A store as the application has it, with the answer loaded.
struct Loaded {
  std::unique_ptr<Store> store = makeMemoryStore();
  FakeHttp http;
  Said said;
  explicit Loaded(const std::string & body = kAnswer) {
    http.answer = [body](const std::string &) { return ok(body); };
    KarSource source(std::chrono::hours(3), [] { return kNow; });
    source.load(*store, http, said);
  }
  // The dollar edge of a metal (by the ISIN the other sources share) on a market (no market by default), in the dataset.
  std::optional<EdgeKey> edge(const char * isin, const char * mic = "XLBM") const {
    const auto snapshot = store->snapshot();
    const auto base = snapshot->findInstrumentByIsin(isin), quote = snapshot->findInstrument("USD");
    const auto market = snapshot->findMarket(mic);
    const auto dataset = snapshot->findDataset(kKarDataset);
    if (!base || !quote || !market || !dataset) return std::nullopt;
    return EdgeKey{*base, *quote, *market, *dataset};
  }
};

}  // namespace

TEST(kar_reads_the_dollar_prices_per_ounce_and_leaves_the_converted_currencies_out) {
  KarSource::Parsed parsed;
  std::string error;
  CHECK(KarSource::parse(kAnswer, parsed, error));
  // the euro rows, rhodium and the unknown code are not carried, and are not rows that failed
  CHECK(parsed.rows.size() == 3 && parsed.skipped == 0);
  CHECK(parsed.rows[1].metal == "AU" && parsed.rows[1].time == kNov26);
  CHECK_NEAR(parsed.rows[1].price, 3578.9, 1e-9);
  CHECK(parsed.rows[2].metal == "AG" && parsed.rows[2].price == 41.54);
}

TEST(kar_leaves_out_rows_that_are_not_prices_and_refuses_what_is_not_an_answer) {
  KarSource::Parsed parsed;
  std::string error;
  const std::string body = R"([{"Code":"AU","Prices":[
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","Bid":0,"Ask":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","Bid":2,"Ask":1},
    {"Currency":"USD","TradeTime":"not a time","Bid":1,"Ask":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","Bid":"1","Ask":1},
    {"TradeTime":"2026-11-26T15:03:00","Bid":1,"Ask":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","Bid":1,"Ask":1}]}])";
  CHECK(KarSource::parse(body, parsed, error));
  CHECK(parsed.rows.size() == 1 && parsed.skipped == 4);  // a row with no currency is not the dollar's: left out, not counted
  CHECK(!KarSource::parse("", parsed, error) && !error.empty());
  CHECK(!KarSource::parse("{\"error\":true}", parsed, error));
  CHECK(!KarSource::parse("[]", parsed, error));
  CHECK(KarSource::parse(R"([{"Code":"AU","Prices":[]}])", parsed, error) && parsed.rows.empty());
}

TEST(kar_asks_for_the_whole_history_as_a_browser_does_with_the_pages_referer) {
  CHECK(KarSource::url(kNow) == "https://karasmussen.fi/wp-json/price-history/v1/prices?from=2019-11-01&to=2026-11-27");
  Loaded l;
  CHECK(l.http.urls.size() == 1 && l.http.urls[0] == KarSource::url(kNow));
  bool agent = false, referer = false;
  for (const auto & [name, value] : l.http.headers[0]) {
    if (name == "User-Agent" && value.find("Windows NT 10.0") != std::string::npos) agent = true;
    if (name == "Referer" && value == "https://karasmussen.fi/paivan-hinnat/") referer = true;
  }
  CHECK(agent && referer);
  KarSource source;
  CHECK(source.name() == "kar" && source.datasetCodes() == std::vector<std::string>{kKarDataset} && std::string(kKarDataset) == "KAR");
}

TEST(kar_writes_the_daily_bars_and_the_last_price_of_each_metal_in_dollars_on_the_bullion_market) {
  Loaded l;
  CHECK(l.said.changes == 1 && l.said.messages.empty());
  const auto gold = l.edge(known_isin::kGold), silver = l.edge(known_isin::kSilver);
  CHECK(gold.has_value() && silver.has_value());
  if (!gold || !silver) return;
  const auto snapshot = l.store->snapshot();
  CHECK(snapshot->quoteRowsSince(0).size() == 2);  // gold and silver in dollars: no euros, no rhodium
  const auto listing = snapshot->listing(*gold);
  CHECK(listing && listing->symbol == "XAU" && listing->type == "METAL");
  const auto bars = snapshot->queue(*gold, Resolution::Day);
  CHECK(bars.size() == 2);
  if (bars.size() != 2) return;
  CHECK(bars[1].time == dayNumberOfInstant(kNov26) && std::abs(bars[1].close - 3578.9) < 1e-9);
  // the answer's first week has no Monday (the 25th is a Wednesday): it is not a whole week, and is left out
  CHECK(snapshot->queue(*gold, Resolution::Week).empty());
  // one price: a last, and no bid or ask to say it has two sides
  const auto quote = snapshot->quote(*gold);
  CHECK(quote && quote->time == kNov26 && std::abs(quote->last - 3578.9) < 1e-9 && !isDefined(quote->bid) && !isDefined(quote->ask));
  const auto silverQuote = snapshot->quote(*silver);
  CHECK(silverQuote && silverQuote->last == 41.54);
}

TEST(kar_says_what_went_wrong_and_writes_nothing_of_an_answer_that_is_not_one) {
  Loaded refused(R"(<html>maintenance</html>)");
  CHECK(refused.said.messages.size() == 1 && refused.said.messages[0].rfind("kar: ", 0) == 0 && refused.said.changes == 0);
  CHECK(refused.store->snapshot()->quoteRowsSince(0).empty());

  Loaded empty(R"([{"Code":"AU","Prices":[]}])");
  CHECK(empty.said.messages.size() == 1 && empty.said.messages[0] == "kar: no prices in the answer");

  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(503); };
  Said said;
  KarSource(std::chrono::hours(3), [] { return kNow; }).load(*store, http, said);
  CHECK(said.messages.size() == 1 && said.messages[0] == "kar: HTTP 503" && said.changes == 0);
}

TEST(kar_loads_again_without_changing_what_the_store_has) {
  Loaded l;
  const auto edge = *l.edge(known_isin::kGold);
  const auto before = l.store->snapshot()->queue(edge, Resolution::Day).size();
  KarSource source(std::chrono::hours(3), [] { return kNow; });
  source.load(*l.store, l.http, l.said);
  CHECK(l.store->snapshot()->queue(edge, Resolution::Day).size() == before && before == 2);
}

TEST(kar_makes_the_weekly_bars_the_3_and_10_years_read_of_the_daily_ones) {
  // Gold in dollars on every weekday from Monday 2026-11-16 to Thursday 2026-11-26, at 1000 plus the day of the month.
  std::string rows;
  for (int day : {16, 17, 18, 19, 20, 23, 24, 25, 26}) {
    char at[32];
    std::snprintf(at, sizeof at, "2026-11-%02dT15:00:00", day);
    const std::string price = std::to_string(1000 + day);
    rows += std::string(rows.empty() ? "" : ",") + R"({"Currency":"USD","TradeTime":")" + at + R"(","Bid":)" + price + R"(,"Ask":)" + price + "}";
  }
  Loaded l(R"([{"Code":"AU","Prices":[)" + rows + "]}]");
  const auto gold = l.edge(known_isin::kGold);
  CHECK(gold.has_value());
  if (!gold) return;
  const auto weeks = l.store->snapshot()->queue(*gold, Resolution::Week);
  CHECK(weeks.size() == 2);  // the Mondays of the 16th and the 23rd
  if (weeks.size() != 2) return;
  CHECK(weeks[0].time == dayNumber(2026, 11, 16) && weeks[1].time == dayNumber(2026, 11, 23));
  CHECK_NEAR(weeks[0].close, 1020, 1e-9);  // one value a week, as one a day: the last day's
  CHECK_NEAR(weeks[1].close, 1026, 1e-9);  // the week that is still going on is the bar of the days there are
}
