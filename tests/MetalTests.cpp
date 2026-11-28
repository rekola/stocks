#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "source/KnownIsins.h"
#include "source/MetalSource.h"
#include "source/SchemaFile.h"

#include <optional>
#include <set>

namespace {

// A schema as a dealer's: one request for each metal and currency, daily bars that run from 15:00 to 15:00 UTC, so a bar is of the day it ends on.
const char * kSchema = R"({
 "name": "dealer", "dataset": "DEALER", "title": "A dealer", "market": "XXXX", "pauseMs": 10,
 "request": {"url": "https://example.test/chart?symbol={symbol}&currency={currency}&from={from}&to={to}&interval={interval}", "interval": "P1D", "years": 10},
 "dayKey": "end", "foldWeekends": true,
 "items": [{"symbol": "XAU", "name": "Gold", "isin": "XC0009655157", "currencies": ["USD"]}, {"symbol": "XAG", "name": "Silver", "isin": "XC0009653103"}]
})";

MetalSchema schema(const char * text = kSchema) {
  MetalSchema s;
  std::string error;
  const bool ok = MetalSchema::parse(text, s, error);
  CHECK(ok);
  if (!ok) std::printf("  %s\n", error.c_str());
  return s;
}

Timestamp at(int y, unsigned m, unsigned d, int hour) { return (dayNumber(y, m, d) - kUnixEpochDayNumber) * 86400 + hour * 3600; }

std::string iso(Timestamp t) {
  const int64_t days = t / 86400;
  const Date d = civilFromUnixDays(days);
  char text[40];
  std::snprintf(text, sizeof text, "%04d-%02u-%02uT%02d:00:00+00:00", d.year, d.month, d.day, static_cast<int>(t % 86400 / 3600));
  return text;
}

std::string bar(Timestamp start, Timestamp end, double open, double high, double low, double close) {
  char text[400];
  std::snprintf(text, sizeof text, R"({"open":%g,"close":%g,"high":%g,"low":%g,"startDateTime":"%s","endDateTime":"%s"})", open, close, high, low, iso(start).c_str(), iso(end).c_str());
  return text;
}

// The days of a week and the next, as such an answer has them: the bar from Friday's 15:00 to Saturday's is a few hours of the evening, and the one from Sunday's to Monday's is
// Monday's.
std::string week() {
  std::string bars;
  const auto add = [&](Timestamp s, Timestamp e, double o, double h, double l, double c) { bars += (bars.empty() ? "" : ",") + bar(s, e, o, h, l, c); };
  add(at(2026, 11, 24, 15), at(2026, 11, 25, 15), 3460, 3480, 3450, 3470);  // Wednesday's
  add(at(2026, 11, 25, 15), at(2026, 11, 26, 15), 3470, 3590, 3465, 3580);  // Thursday's
  add(at(2026, 11, 26, 15), at(2026, 11, 27, 15), 3580, 3650, 3575, 3637);  // Friday's
  add(at(2026, 11, 27, 15), at(2026, 11, 28, 15), 3637, 3660, 3630, 3654);  // Friday's evening: Saturday by its end
  add(at(2026, 11, 29, 15), at(2026, 11, 30, 15), 3654, 3700, 3640, 3690);  // Monday's, from Sunday evening
  add(at(2026, 11, 30, 15), at(2026, 12, 1, 15), 3690, 3720, 3680, 3710);   // Tuesday's, still going on
  return "{\"statusCode\":200,\"data\":[" + bars + "]}";
}

const Timestamp kNow = 1796126400;  // 2026-12-01 12:00 UTC

struct Said : SourceListener {
  std::vector<std::string> messages;
  int changes = 0, pauses = 0;
  void changed() override { changes++; }
  void message(const std::string & m) override { messages.push_back(m); }
  bool pause(std::chrono::milliseconds) override { pauses++; return true; }  // no waiting in a test
};

}  // namespace

TEST(metal_schema_is_read_and_what_is_wrong_with_one_is_said) {
  const MetalSchema s = schema();
  CHECK(s.name == "dealer" && s.dataset == "DEALER" && s.title == "A dealer" && s.market == "XXXX" && s.pause == std::chrono::milliseconds(10));
  CHECK(s.items.size() == 2 && s.items[0].currencies == std::vector<std::string>{"USD"} && s.items[1].currencies == std::vector<std::string>{"USD"});  // dollars unless it says
  CHECK(s.items[0].type == "METAL" && s.items[0].unit == "troy oz" && s.dayKey == "end" && s.foldWeekends && s.years == 10 && s.interval == "P1D" && s.time == "iso");
  const auto bad = [&](const std::string & from, const std::string & to, const std::string & part) {
    std::string text = kSchema;
    const size_t at = text.find(from);
    CHECK(at != std::string::npos);
    text.replace(at, from.size(), to);
    MetalSchema out;
    std::string error;
    CHECK(!MetalSchema::parse(text, out, error) && error.find(part) != std::string::npos);
    if (error.find(part) == std::string::npos) std::printf("  wanted '%s', got '%s'\n", part.c_str(), error.c_str());
  };
  bad("\"DEALER\"", "\"dealer lower\"", "dataset is the code");
  bad("\"XXXX\"", "\"X\"", "MIC");
  bad("\"dayKey\": \"end\"", "\"dayKey\": \"middle\"", "dayKey");
  bad("XC0009655157", "XC0009655158", "ISIN");
  bad("symbol={symbol}&", "", "no {symbol}");
  bad("\"years\": 10", "\"years\": 0", "years");
  bad("\"currencies\": [\"USD\"]", "\"currencies\": []", "currencies");
  MetalSchema out;
  std::string error;
  CHECK(!MetalSchema::parse("[]", out, error) && !MetalSchema::parse("{}", out, error) && !MetalSchema::parse("not json", out, error));
}

TEST(metal_schemas_that_come_with_the_program_are_all_valid_and_have_datasets_of_their_own) {
  const std::vector<std::string> names = schemaNames("metals");
  CHECK(!names.empty());
  std::set<std::string> datasets;
  for (const std::string & name : names) {
    MetalSchema s;
    std::string error;
    CHECK(MetalSchema::load(name, s, error));
    if (!error.empty()) std::printf("  %s\n", error.c_str());
    CHECK(datasets.insert(s.dataset).second && s.name == name);
  }
  MetalSchema missing;
  std::string error;
  CHECK(!MetalSchema::load("nowhere", missing, error) && error.find("there is no schema") != std::string::npos);
}

TEST(metal_request_asks_for_ten_years_to_now_with_the_times_written_as_the_schema_says) {
  MetalSchema s = schema();
  const Timestamp now = 1795865760;  // 2026-11-28 11:36 UTC
  CHECK(MetalSource::url(s, s.items[0], "USD", now) ==
        "https://example.test/chart?symbol=XAU&currency=USD&from=2016-11-30T11%3A36%3A00Z&to=2026-11-28T11%3A36%3A00Z&interval=P1D");
  s.time = "date";
  s.interval = "P7D";
  CHECK(MetalSource::url(s, s.items[1], "EUR", now) == "https://example.test/chart?symbol=XAG&currency=EUR&from=2016-11-30&to=2026-11-28&interval=P7D");
}

TEST(metal_a_bar_is_of_the_day_it_ends_and_the_few_hours_of_friday_evening_are_fridays) {
  const MetalSchema s = schema();
  const series::Result r = series::read(week());
  CHECK(r.ok && r.series.size() == 1 && r.series[0].barSeconds == 86400);
  const std::vector<Candle> days = MetalSource::days(s, r.series[0]);
  CHECK(days.size() == 5);
  if (days.size() != 5) return;
  CHECK(days[0].time == dayNumber(2026, 11, 25) && days[1].time == dayNumber(2026, 11, 26) && days[2].time == dayNumber(2026, 11, 27));
  CHECK(days[3].time == dayNumber(2026, 11, 30) && days[4].time == dayNumber(2026, 12, 1));  // Sunday evening's bar is Monday's
  // Friday's own bar and its evening are one: opened with Friday, closed with the evening, the extremes of both.
  CHECK(days[2].open == 3580 && days[2].close == 3654 && days[2].high == 3660 && days[2].low == 3575);
  // By the start, as another provider's days are, a bar is of the day it starts: the evening is a Friday's own and the Sunday's is Sunday's, which the weeks do not want.
  MetalSchema byStart = s;
  byStart.dayKey = "start";
  byStart.foldWeekends = false;
  const std::vector<Candle> starts = MetalSource::days(byStart, r.series[0]);
  CHECK(starts.size() == 6 && starts[3].time == dayNumber(2026, 11, 27) + 0 && starts[4].time == dayNumber(2026, 11, 29));
}

TEST(metal_writes_the_daily_and_weekly_bars_and_the_price_of_each_metal_in_each_currency) {
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(week()); };
  Said said;
  MetalSource source(schema(), [] { return kNow; });
  CHECK(source.name() == "metals:dealer" && source.datasetCodes() == std::vector<std::string>{"DEALER"});
  source.load(*store, http, said);
  CHECK(http.urls.size() == 2 && said.messages.empty() && said.changes == 2 && said.pauses == 1);  // a pause between the requests, not before the first
  CHECK(http.urls[0].find("symbol=XAU&currency=USD") != std::string::npos && http.urls[1].find("symbol=XAG&currency=USD") != std::string::npos);
  const auto s = store->snapshot();
  const auto gold = s->findInstrumentByIsin(known_isin::kGold);
  const auto usd = s->findInstrument("USD");
  const auto market = s->findMarket("XXXX");
  const auto dataset = s->findDataset("DEALER");
  CHECK(gold && usd && market && dataset);
  if (!gold || !usd || !market || !dataset) return;
  const EdgeKey edge{*gold, *usd, *market, *dataset};
  const auto days = s->queue(edge, Resolution::Day);
  CHECK(days.size() == 5 && days.back().time == dayNumber(2026, 12, 1) && days.back().close == 3710 && days.back().high == 3720);
  const auto weeks = s->queue(edge, Resolution::Week);
  CHECK(weeks.size() == 1 && weeks[0].time == dayNumber(2026, 11, 30));  // the first week has no Monday, and is left out
  const auto quote = s->quote(edge);
  CHECK(quote && quote->last == 3710 && quote->time == kNow);  // the bar that is still going on has its price now
  const auto listing = s->listing(edge);
  CHECK(listing && listing->symbol == "XAU" && listing->type == "METAL" && listing->name == "Gold, per troy oz");
  CHECK(s->quoteRowsSince(0).size() == 2);
}

TEST(metal_says_what_went_wrong_and_goes_on_to_the_next_metal) {
  auto store = makeMemoryStore();
  FakeHttp http;
  int n = 0;
  http.answer = [&](const std::string &) {
    n++;
    return n == 1 ? ok(R"({"statusCode":400,"error":{"type":"BAD_REQUEST","description":"Could not resolve argument `symbol`."}})") : ok(week());
  };
  Said said;
  MetalSource(schema(), [] { return kNow; }).load(*store, http, said);
  CHECK(said.messages.size() == 1 && said.messages[0] == "dealer: XAU/USD: Could not resolve argument `symbol`." && said.changes == 1 && http.urls.size() == 2);

  const auto one = [&](HttpResult answer) {
    auto s = makeMemoryStore();
    FakeHttp h;
    h.answer = [&](const std::string &) { return answer; };
    Said sa;
    MetalSchema single = schema();
    single.items.resize(1);
    MetalSource(single, [] { return kNow; }).load(*s, h, sa);
    return std::make_pair(sa.messages, sa.changes);
  };
  const auto http503 = one(failed(503));
  CHECK(http503.first == std::vector<std::string>{"dealer: XAU/USD: HTTP 503"} && http503.second == 0);
  const auto html = one(ok("<html>maintenance</html>"));
  CHECK(html.first == std::vector<std::string>{"dealer: XAU/USD: not JSON"});
  const auto empty = one(ok(R"({"statusCode":200,"data":[]})"));
  CHECK(empty.first == std::vector<std::string>{"dealer: XAU/USD: no bars in the answer"});
  // The answer gives longer bars than a day (an endpoint may, whatever it was asked for): not written, and said.
  const auto weekly = one(ok(R"({"data":[{"open":1,"close":2,"high":3,"low":1,"startDateTime":"2026-11-02T00:00:00+00:00","endDateTime":"2026-11-09T00:00:00+00:00"},
                                          {"open":2,"close":3,"high":3,"low":1,"startDateTime":"2026-11-09T00:00:00+00:00","endDateTime":"2026-11-16T00:00:00+00:00"}]})"));
  CHECK(weekly.first.size() == 1 && weekly.first[0].find("604800 seconds long, longer than a day; not written") != std::string::npos && weekly.second == 0);
  // Several series in an answer that is for one metal.
  const auto several = one(ok(R"({"a":[[1700000000,1],[1700086400,2]],"b":[[1700000000,3],[1700086400,4]]})"));
  CHECK(several.first.size() == 1 && several.first[0].find("2 series, not the one asked for") != std::string::npos);
}

TEST(metal_stops_at_a_refusal_and_does_not_ask_the_others) {
  for (int status : {429, 403}) {
    auto store = makeMemoryStore();
    FakeHttp http;
    http.answer = [&](const std::string &) { return failed(status); };
    Said said;
    MetalSource(schema(), [] { return kNow; }).load(*store, http, said);
    CHECK(http.urls.size() == 1 && said.messages.size() == 1 && said.messages[0].find("stopping") != std::string::npos);
  }
}

namespace {

// A provider with every metal and currency in one answer, as `kar` is: gold in dollars and euros (the euros are the dollar's converted: not carried), silver, rhodium and
// a code that is not asked for. The numbers are the real ones of those days.
const char * kEvery = R"([
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

MetalSchema kar() {
  MetalSchema s;
  std::string error;
  const bool ok = MetalSchema::load("kar", s, error);
  CHECK(ok);
  if (!ok) std::printf("  %s\n", error.c_str());
  return s;
}

// The edge of a metal (by its ISIN) in dollars on a market, in a dataset.
std::optional<EdgeKey> edgeOf(const Snapshot & s, const char * isin, const char * mic, const char * dataset) {
  const auto base = s.findInstrumentByIsin(isin), quote = s.findInstrument("USD");
  const auto market = s.findMarket(mic);
  const auto set = s.findDataset(dataset);
  if (!base || !quote || !market || !set) return std::nullopt;
  return EdgeKey{*base, *quote, *market, *set};
}

// A bar for each weekday from `first` to `last` (day numbers), as a dealer's: from 15:00 the day before to 15:00, so the Monday's starts on the Sunday.
std::string daysAnswer(int64_t first, int64_t last, double base = 3000) {
  std::string bars;
  for (int64_t d = first; d <= last; d++) {
    if (weekday(d) >= 5) continue;
    const Timestamp end = (d - kUnixEpochDayNumber) * 86400 + 15 * 3600;
    const double close = base + static_cast<double>(d % 200);
    bars += (bars.empty() ? "" : ",") + bar(end - 86400, end, close - 1, close + 5, close - 5, close);
  }
  return "{\"statusCode\":200,\"data\":[" + bars + "]}";
}

}  // namespace

TEST(metal_an_answer_of_every_metal_is_one_request_and_the_series_are_found_by_their_labels) {
  const MetalSchema s = kar();
  CHECK(!s.perItem() && s.items.size() == 5 && s.items[4].market == "XXXX" && s.items[4].labels.at("Code") == "RH" && s.market == "XLBM");
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(kEvery); };
  Said said;
  MetalSource(s, [] { return kNow; }).load(*store, http, said);
  CHECK(http.urls.size() == 1 && http.urls[0] == "https://karasmussen.fi/wp-json/price-history/v1/prices?from=2016-12-03&to=2026-12-01");
  // The Referer is the page's, over the browser's headers.
  bool referer = false;
  for (const auto & [name, value] : http.headers[0])
    if (name == "Referer" && value == "https://karasmussen.fi/paivan-hinnat/") referer = true;
  CHECK(referer);
  // Platinum and palladium are not in the made-up answer: said, and the others written.
  CHECK(said.messages == std::vector<std::string>({"kar: XPT/USD: not in the answer", "kar: XPD/USD: not in the answer"}) && said.changes == 3);
  const auto snapshot = store->snapshot();
  const auto gold = edgeOf(*snapshot, known_isin::kGold, "XLBM", "KAR");
  const auto silver = edgeOf(*snapshot, known_isin::kSilver, "XLBM", "KAR");
  const auto rhodium = edgeOf(*snapshot, known_isin::kRhodium, "XXXX", "KAR");
  CHECK(gold && silver && rhodium);
  if (!gold || !silver || !rhodium) return;
  CHECK(snapshot->quoteRowsSince(0).size() == 3);  // gold, silver and rhodium in dollars: no euros
  // A price a day: a value queue, at the price; a bid and an ask equal are one price, so none is written.
  const auto days = snapshot->queue(*gold, Resolution::Day);
  CHECK(days.size() == 2 && days[1].time == dayNumber(2026, 11, 26) && days[1].close == 3578.9);
  CHECK(snapshot->coverage(*gold, Resolution::Day)->kind == QueueKind::Scalar);
  const auto quote = snapshot->quote(*gold);
  CHECK(quote && quote->last == 3578.9 && quote->time == 1795705380 && !isDefined(quote->bid) && !isDefined(quote->ask));  // 2026-11-26 15:03 UTC, the fix's time
  CHECK(snapshot->quote(*silver)->last == 41.54 && snapshot->quote(*rhodium)->last == 8900);
  // Rhodium has no fix: it is on no market, and not on the other.
  CHECK(!snapshot->quote(EdgeKey{rhodium->base, rhodium->quote, gold->market, rhodium->dataset}) && !snapshot->quote(EdgeKey{gold->base, gold->quote, rhodium->market, gold->dataset}));
  // A series that is not found by its labels is not another's.
  MetalSchema other = kar();
  other.items.resize(1);
  other.items[0].labels["Code"] = "ZZ";
  auto empty = makeMemoryStore();
  Said none;
  MetalSource(other, [] { return kNow; }).load(*empty, http, none);
  CHECK(none.messages == std::vector<std::string>({"kar: XAU/USD: not in the answer"}) && none.changes == 0);
}

TEST(metal_a_load_after_the_first_asks_from_two_weeks_before_the_last_day_it_has) {
  MetalSchema s = schema();
  s.items.resize(1);
  auto store = makeMemoryStore();
  FakeHttp http;
  const int64_t first = dayNumber(2025, 10, 1), last = dayNumber(2026, 11, 30);  // a Wednesday and a Monday: some fifty-eight weeks
  http.answer = [&](const std::string &) { return ok(daysAnswer(first, last)); };
  Said said;
  MetalSource source(s, [] { return kNow; });
  source.load(*store, http, said);
  CHECK(said.messages.empty() && http.urls.size() == 1 && http.urls[0] == MetalSource::url(s, s.items[0], "USD", kNow));  // the first asks for ten years

  // Now the store has them: the next asks from the last day less 14, and what it gets goes on the end.
  http.answer = [&](const std::string &) { return ok(daysAnswer(last - 14, last + 1)); };
  source.load(*store, http, said);
  const Timestamp since = (last - 14 - kUnixEpochDayNumber) * 86400;
  CHECK(said.messages.empty() && http.urls.size() == 2 && http.urls[1] == MetalSource::url(s, s.items[0], "USD", kNow, since));
  CHECK(http.urls[1].find("from=2026-11-16T00%3A00%3A00Z") != std::string::npos);  // two weeks before Monday the 30th
  const auto snapshot = store->snapshot();
  const auto gold = edgeOf(*snapshot, known_isin::kGold, "XXXX", "DEALER");
  CHECK(gold.has_value());
  if (!gold) return;
  const auto days = snapshot->queue(*gold, Resolution::Day);
  CHECK(days.back().time == last + 1 && days[days.size() - 2].time == last);  // Tuesday the 1st of December is on the end, and nothing before it was lost
  const auto weeks = snapshot->queue(*gold, Resolution::Week);
  CHECK(weeks.size() >= 55 && weeks.back().time == dayNumber(2026, 11, 30));
  // What came with the overlap did not change what was there.
  CHECK(days.size() > 250);

  // A store that does not have the edge, or has only a few weeks of it, asks for the whole.
  auto fresh = makeMemoryStore();
  FakeHttp again;
  again.answer = [](const std::string &) { return ok(week()); };
  Said sa;
  MetalSource second(s, [] { return kNow; });
  second.load(*fresh, again, sa);
  second.load(*fresh, again, sa);
  CHECK(again.urls.size() == 2 && again.urls[0] == again.urls[1] && again.urls[0] == MetalSource::url(s, s.items[0], "USD", kNow));
}

TEST(metal_an_answer_of_every_metal_asks_from_the_earliest_day_any_of_them_needs) {
  const MetalSchema s = kar();
  auto store = makeMemoryStore();
  FakeHttp http;
  http.answer = [](const std::string &) { return ok(kEvery); };
  Said said;
  MetalSource source(s, [] { return kNow; });
  source.load(*store, http, said);
  source.load(*store, http, said);
  // Two days of bars are not what a store has of a metal that was loaded whole (half a year of weeks), so the second asks for the whole history again.
  CHECK(http.urls.size() == 2 && http.urls[0] == http.urls[1]);
}

namespace {

// The hourly bars of the weekdays from `first` to `last` (day numbers) as a provider whose day begins at 15:00 UTC has them: round the clock from Sunday 22:00 to Friday 22:00, an
// hour of each at 1000 plus its hour of the week, so that a day's open is its first hour's and its close its last's.
std::string hoursAnswer(Timestamp from, Timestamp to) {
  std::string bars;
  for (Timestamp t = from - from % 3600; t < to; t += 3600) {
    const int64_t d = t / 86400 + kUnixEpochDayNumber;
    const int hour = static_cast<int>(t % 86400 / 3600);
    const int wd = weekday(d);
    const bool closed = (wd == 4 && hour >= 22) || wd == 5 || (wd == 6 && hour < 22);  // from Friday 22:00 to Sunday 22:00 UTC
    if (closed) continue;
    const double v = 1000 + static_cast<double>((t / 3600) % 1000);
    bars += (bars.empty() ? "" : ",") + bar(t, t + 3600, v, v + 2, v - 2, v + 1);
  }
  return "{\"statusCode\":200,\"data\":[" + bars + "]}";
}

MetalSchema hourly() {
  MetalSchema s = schema(R"({
   "name": "dealer", "dataset": "DEALER", "market": "XXXX", "pauseMs": 10,
   "request": {"url": "https://example.test/chart?symbol={symbol}&currency={currency}&from={from}&to={to}&interval={interval}", "interval": "P1D", "years": 10,
               "recent": {"interval": "PT1H", "days": 28}},
   "dayKey": "end", "dayBoundaryHour": 15, "foldWeekends": true,
   "items": [{"symbol": "XAU", "name": "Gold", "isin": "XC0009655157"}]})");
  return s;
}

}  // namespace

TEST(metal_hours_make_the_days_of_a_provider_whose_day_begins_at_a_set_hour) {
  const MetalSchema s = hourly();
  CHECK(s.dayBoundaryHour == 15 && s.recent.interval == "PT1H" && s.recent.days == 28);
  // Wednesday 2026-11-25 15:00 to Friday 2026-11-27 15:00: hourly bars of two days, whose first begins on the Wednesday afternoon.
  const series::Result r = series::read(hoursAnswer(at(2026, 11, 25, 15), at(2026, 11, 27, 15)));
  CHECK(r.ok && r.series[0].barSeconds == 3600 && r.series[0].bars.size() == 48);
  const std::vector<Candle> days = MetalSource::days(s, r.series[0]);
  CHECK(days.size() == 2 && days[0].time == dayNumber(2026, 11, 26) && days[1].time == dayNumber(2026, 11, 27));  // each of the day it ends on
  const double firstHour = 1000 + static_cast<double>((at(2026, 11, 25, 15) / 3600) % 1000);
  CHECK(days[0].open == firstHour && days[0].high == days[0].close + 1 + 0 + (days[0].high - days[0].close - 1) && days[0].low < days[0].open);
  // A request that begins in the middle of a day has only part of its first: that one is left out.
  const series::Result part = series::read(hoursAnswer(at(2026, 11, 25, 20), at(2026, 11, 27, 15)));
  const std::vector<Candle> rest = MetalSource::days(s, part.series[0], /*dropFirstDay=*/true);
  CHECK(rest.size() == 1 && rest[0].time == dayNumber(2026, 11, 27) && rest[0].open == days[1].open && rest[0].close == days[1].close);
  // A week of them, over a weekend: Friday's evening is Friday's, and Sunday evening's is Monday's.
  const series::Result weekOf = series::read(hoursAnswer(at(2026, 11, 22, 0), at(2026, 11, 28, 22)));
  const std::vector<Candle> days2 = MetalSource::days(s, weekOf.series[0]);
  bool noWeekend = true;
  for (const Candle & c : days2) noWeekend = noWeekend && weekday(c.time) < 5;
  CHECK(noWeekend && days2.size() == 5 && days2.front().time == dayNumber(2026, 11, 23));  // Monday the 23rd to Friday the 27th
}

TEST(metal_the_first_load_asks_for_the_history_and_the_recent_hours_and_the_later_loads_for_the_hours_alone) {
  const MetalSchema s = hourly();
  auto store = makeMemoryStore();
  FakeHttp http;
  const Timestamp now = at(2026, 12, 1, 12);  // a Tuesday
  http.answer = [&](const std::string & address) {
    if (address.find("interval=PT1H") != std::string::npos) {
      const size_t f = address.find("from=") + 5;
      // the request's from, read back from the text: 2026-11-03T12%3A00%3A00Z
      const int y = std::stoi(address.substr(f, 4)), mo = std::stoi(address.substr(f + 5, 2)), d = std::stoi(address.substr(f + 8, 2)), h = std::stoi(address.substr(f + 11, 2));
      return ok(hoursAnswer(at(y, static_cast<unsigned>(mo), static_cast<unsigned>(d), h), now));
    }
    return ok(daysAnswer(dayNumber(2025, 10, 1), dayNumber(2026, 12, 1), 3000));
  };
  Said said;
  MetalSource source(s, [&] { return now; });
  source.load(*store, http, said);
  CHECK(said.messages.empty() && http.urls.size() == 2 && said.pauses == 1);
  CHECK(http.urls[0] == MetalSource::url(s, s.items[0], "USD", now) && http.urls[0].find("interval=P1D") != std::string::npos);
  CHECK(http.urls[1] == MetalSource::url(s, s.items[0], "USD", now, now - 28 * 86400, "PT1H"));
  const auto snapshot = store->snapshot();
  const auto gold = edgeOf(*snapshot, known_isin::kGold, "XXXX", "DEALER");
  CHECK(gold.has_value());
  if (!gold) return;
  // The days of the last four weeks are the hours', the ones before are the daily bars'.
  const auto days = snapshot->queue(*gold, Resolution::Day);
  const auto hours = snapshot->queue(*gold, Resolution::Hour1);
  CHECK(!hours.empty() && days.size() > 250 && days.back().time == dayNumber(2026, 12, 1));
  const int64_t recent = dayNumber(2026, 11, 5);  // the hours begin on the 3rd of November, and the day they begin in is only part of one
  bool fromHours = true, fromDays = true;
  for (const Candle & c : days) {
    if (c.time >= recent) fromHours = fromHours && c.high - c.low > 10;                                      // a day of hours has the range of its hours, which climb by one an hour
    else if (c.time >= dayNumber(2026, 1, 1) && c.time < dayNumber(2026, 11, 3)) fromDays = fromDays && c.high - c.low == 10;  // a daily bar of the made-up answer: 5 either side
  }
  CHECK(fromHours && fromDays);
  CHECK(snapshot->quote(*gold)->time == now);

  // Now the store has them, and the hours alone are asked for: one request.
  http.urls.clear();
  source.load(*store, http, said);
  CHECK(said.messages.empty() && http.urls.size() == 1 && http.urls[0].find("interval=PT1H") != std::string::npos);
  // From the day before the current week began (the Monday was the 30th: the Sunday the 29th), not from 28 days back: the current week is all that has changed.
  CHECK(http.urls[0] == MetalSource::url(s, s.items[0], "USD", now, at(2026, 11, 29, 0), "PT1H"));
  CHECK(http.urls[0].find("from=2026-11-29T00%3A00%3A00Z") != std::string::npos);

  // A store that missed some loads (ten days) asks from the day before its last day, which is earlier than the current week's: the hours alone still reach.
  http.urls.clear();
  MetalSource missed(s, [&] { return now + 10 * 86400; });
  missed.load(*store, http, said);
  CHECK(http.urls.size() == 1 && http.urls[0] == MetalSource::url(s, s.items[0], "USD", now + 10 * 86400, at(2026, 11, 30, 0), "PT1H"));

  // A store that has not been loaded for longer than the hours reach back asks for the daily history from its last day too.
  http.urls.clear();
  MetalSource later(s, [&] { return now + 40 * 86400; });
  later.load(*store, http, said);
  CHECK(http.urls.size() == 2 && http.urls[0].find("interval=P1D") != std::string::npos && http.urls[1].find("interval=PT1H") != std::string::npos);
}
