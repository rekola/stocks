#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "model/Period.h"
#include "source/KarSource.h"
#include "source/KnownIsins.h"

namespace {

// The price page's answer, made up: gold on 2026-11-25 and 2026-11-26 (USD at one price; EUR with the rate's two sides), rhodium in dollars (a metal that is not
// carried) and an unknown code. The numbers are the real ones of those days.
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
  // The edge of gold (by the ISIN the other sources share) in a currency, on the London Bullion Market, in the dataset.
  std::optional<EdgeKey> edge(const std::string & currency) const {
    const auto snapshot = store->snapshot();
    const auto base = snapshot->findInstrumentByIsin(known_isin::kGold), quote = snapshot->findInstrument(currency);
    const auto market = snapshot->findMarket("XLBM");
    const auto dataset = snapshot->findDataset(kKarDataset);
    if (!base || !quote || !market || !dataset) return std::nullopt;
    return EdgeKey{*base, *quote, *market, *dataset};
  }
};

}  // namespace

TEST(kar_reads_the_answer_as_prices_per_ounce) {
  KarSource::Parsed parsed;
  std::string error;
  CHECK(KarSource::parse(kAnswer, parsed, error));
  // the GBP row and the rhodium and XX metals are not carried, and are not rows that failed
  CHECK(parsed.rows.size() == 4 && parsed.skipped == 0);
  const auto & usd = parsed.rows[3];
  CHECK(usd.metal == "AU" && usd.currency == "USD" && usd.time == kNov26);
  CHECK_NEAR(usd.bid, 3578.9, 0.01);  // the gram's price times 31.1035: the ounce of the fix
  CHECK(usd.bid == usd.ask);
  const auto & eur = parsed.rows[2];
  CHECK(eur.currency == "EUR" && eur.ask > eur.bid && eur.mid() > eur.bid && eur.mid() < eur.ask);
  CHECK_NEAR(eur.bid, 104.794372557713 * KarSource::kGramsPerTroyOunce, 1e-9);
}

TEST(kar_leaves_out_rows_that_are_not_prices_and_refuses_what_is_not_an_answer) {
  KarSource::Parsed parsed;
  std::string error;
  const std::string body = R"([{"Code":"AU","Prices":[
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","BidGram":0,"AskGram":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","BidGram":2,"AskGram":1},
    {"Currency":"USD","TradeTime":"not a time","BidGram":1,"AskGram":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","BidGram":"1","AskGram":1},
    {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","BidGram":1,"AskGram":1}]}])";
  CHECK(KarSource::parse(body, parsed, error));
  CHECK(parsed.rows.size() == 1 && parsed.skipped == 4);
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

TEST(kar_writes_the_daily_bars_and_the_quote_of_each_metal_in_each_currency_on_the_london_bullion_market) {
  Loaded l;
  CHECK(l.said.changes == 1 && l.said.messages.empty());
  const auto eur = l.edge("EUR");
  const auto usd = l.edge("USD");
  CHECK(eur.has_value() && usd.has_value());
  if (!eur || !usd) return;
  const auto snapshot = l.store->snapshot();
  CHECK(snapshot->quoteRowsSince(0).size() == 2);  // gold in two currencies: nothing of the metals that are not carried
  const auto listing = snapshot->listing(*eur);
  CHECK(listing && listing->symbol == "XAU" && listing->type == "METAL");
  // two days of one value each, at the mean of the sides
  const auto bars = snapshot->queue(*eur, Resolution::Day);
  CHECK(bars.size() == 2);
  if (bars.size() != 2) return;
  CHECK(bars[1].time == dayNumberOfInstant(kNov26));
  CHECK_NEAR(bars[1].close, (104.794372557713 + 104.813464263407) / 2 * KarSource::kGramsPerTroyOunce, 1e-9);
  // euros are converted at the sides of a rate: a bid and an ask beside the last
  const auto quote = snapshot->quote(*eur);
  CHECK(quote.has_value());
  if (!quote) return;
  CHECK(quote->time == kNov26 - 180);  // 15:00, the euro's row (the dollar's is 15:03)
  CHECK(isDefined(quote->bid) && isDefined(quote->ask) && quote->bid < quote->last && quote->last < quote->ask);
  // dollars are one price: a last, and no bid or ask to say it has two sides
  const auto fix = snapshot->quote(*usd);
  CHECK(fix && fix->time == kNov26 && !isDefined(fix->bid) && !isDefined(fix->ask) && std::abs(fix->last - 3578.9) < 0.01);
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
  const auto edge = *l.edge("EUR");
  const auto before = l.store->snapshot()->queue(edge, Resolution::Day).size();
  KarSource source(std::chrono::hours(3), [] { return kNow; });
  source.load(*l.store, l.http, l.said);
  CHECK(l.store->snapshot()->queue(edge, Resolution::Day).size() == before && before == 2);
}
