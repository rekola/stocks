#include "TestFramework.h"

#include "FakeHttp.h"
#include "model/Dates.h"
#include "source/KnownIsins.h"
#include "source/MetalSource.h"
#include "source/SchemaFile.h"

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
  // The answer gives finer bars than a day (an endpoint may, whatever it was asked for): not written, and said.
  const auto hourly = one(ok(R"({"data":[{"open":1,"close":2,"high":3,"low":1,"startDateTime":"2026-11-20T00:00:00+00:00","endDateTime":"2026-11-20T01:00:00+00:00"},
                                          {"open":2,"close":3,"high":3,"low":1,"startDateTime":"2026-11-20T01:00:00+00:00","endDateTime":"2026-11-20T02:00:00+00:00"}]})"));
  CHECK(hourly.first.size() == 1 && hourly.first[0].find("asked for daily bars, the answer's are 3600 seconds long") != std::string::npos && hourly.second == 0);
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
