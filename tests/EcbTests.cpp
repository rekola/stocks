#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "source/EcbSource.h"

#include <fstream>
#include <sstream>

namespace {

std::string fixture() {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/ecb-rates.csv");
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

EcbSource::Options at(int64_t now, int days = 400) {
  EcbSource::Options o;
  o.days = days;
  o.now = [now] { return now; };
  return o;
}

constexpr int64_t kNow = 1795300000;  // 2026-11-21 UTC

}  // namespace

TEST(an_ecb_answer_is_rates_by_currency_and_date_in_euros_per_unit) {
  std::vector<EcbRate> rates;
  std::string error;
  CHECK(EcbSource::parse(fixture(), rates, error));
  CHECK(rates.size() == 15);  // three currencies, five working days
  bool usd = false;
  for (const EcbRate & r : rates)
    if (r.currency == "USD" && r.day == dayNumber(2026, 11, 20)) usd = r.rate > 0.5 && r.rate < 2;  // dollars to the euro
  CHECK(usd);
  for (const EcbRate & r : rates) CHECK(r.rate > 0 && r.day >= dayNumber(2026, 11, 16));
}

TEST(what_is_not_an_ecb_answer_is_refused_and_a_day_with_no_rate_is_left_out) {
  std::vector<EcbRate> rates;
  std::string error;
  CHECK(!EcbSource::parse("", rates, error) && error == "not an ECB answer");
  CHECK(!EcbSource::parse("<html>nope</html>", rates, error));
  CHECK(!EcbSource::parse("a,b,c\n1,2,3\n", rates, error));
  const std::string head = "KEY,FREQ,CURRENCY,CURRENCY_DENOM,EXR_TYPE,EXR_SUFFIX,TIME_PERIOD,OBS_VALUE\n";
  rates.clear();
  CHECK(EcbSource::parse(head + "K,D,USD,EUR,SP00,A,2026-11-02,1.1216\nK,D,USD,EUR,SP00,A,2026-11-03,\nK,D,USD,EUR,SP00,A,not-a-date,1.1\n" +
                             "K,D,GBP,USD,SP00,A,2026-11-02,0.8\nK,D,USD,EUR,SP00,A,2026-11-04,0\n",
                         rates, error));
  CHECK(rates.size() == 1 && rates[0].currency == "USD" && rates[0].rate == 1.1216);  // not the empty, the dated badly, the other base, the zero
}

TEST(the_request_asks_for_every_currency_daily_from_the_date_it_is_told_and_from_1999_for_all) {
  CHECK(EcbSource::url(10, kNow) == "https://data-api.ecb.europa.eu/service/data/EXR/D..EUR.SP00.A?format=csvdata&detail=dataonly&startPeriod=2026-11-11");
  CHECK(EcbSource::url(0, kNow).find("startPeriod=1999-01-01") != std::string::npos);
}

TEST(the_rates_become_edges_from_the_euro_to_each_currency_with_a_close_only_and_the_ecb_cited) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(fixture()); };
  Listener listener;
  EcbSource source(at(kNow));
  source.load(*store, http, listener);
  CHECK(http.urls.size() == 1 && http.urls[0].find("/service/data/EXR/D..EUR.SP00.A?") != std::string::npos);
  CHECK(listener.changes == 1 && listener.messages.empty());

  auto s = store->snapshot();
  const DatasetId ecb = s->findDataset("ecb")->id;
  const EdgeKey eurusd{s->findInstrument("EUR")->id, s->findInstrument("USD")->id, s->findMarket("Currencies")->id, ecb};
  CandleSeries bars = s->candles(eurusd, Resolution::Day);
  CHECK(bars.candles.size() == 5 && bars.candles[0].time == dayNumber(2026, 11, 16));
  CHECK(bars.candles[0].close > 0 && !isDefined(bars.candles[0].open) && !isDefined(bars.candles[0].high) &&
        !isDefined(bars.candles[0].low) && !isDefined(bars.candles[0].volume));
  CHECK(s->listing(eurusd)->name == "EUR/USD" && s->symbol(eurusd) == "EURUSD");
  // The three currencies are edges from the euro: a star, with the euro at the middle.
  CHECK(s->edges(s->findMarket("Currencies")->id).size() == 3);
  // The ECB is cited by the dataset.
  CHECK(asText(*s->attribute(NodeRef(DatasetId{ecb}), "source")) == "European Central Bank");
  CHECK(asText(*s->attribute(NodeRef(DatasetId{ecb}), "attribution")).value_or("").find("ECB") != std::string::npos);

  // Loading again changes no bars (the same days replace).
  FakeHttp again;
  again.answer = http.answer;
  source.load(*store, again, listener);
  CHECK(store->snapshot()->candles(eurusd, Resolution::Day).candles.size() == 5);
}

TEST(a_failed_request_says_so_and_loads_nothing) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return failed(503); };
  Listener l;
  EcbSource(at(kNow)).load(*store, http, l);
  CHECK(l.messages.size() == 1 && l.messages[0] == "ecb: HTTP 503" && l.changes == 0 && store->snapshot()->version() == 0);
  FakeHttp none;
  none.answer = [](const std::string &) { return failed(0); };
  Listener l2;
  EcbSource(at(kNow)).load(*store, none, l2);
  CHECK(l2.messages.size() == 1 && l2.messages[0] == "ecb: no answer");
  FakeHttp junk;
  junk.answer = [](const std::string &) { return ok("<html>"); };
  Listener l3;
  EcbSource(at(kNow)).load(*store, junk, l3);
  CHECK(l3.messages.size() == 1 && l3.messages[0] == "ecb: not an ECB answer");
}

TEST(the_rates_are_reloaded_every_six_hours) {
  CHECK(EcbSource().refreshInterval() == std::chrono::hours(6));
}
