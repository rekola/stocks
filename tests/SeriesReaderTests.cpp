#include "TestFramework.h"

#include "source/SeriesReader.h"

#include <fstream>
#include <sstream>

using namespace series;

namespace {

std::string fixture(const std::string & name) {
  std::ifstream in(std::string(STOCKS_FIXTURES_DIR) + "/" + name);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

bool noteSays(const Result & r, const std::string & part) {
  for (const std::string & n : r.notes)
    if (n.find(part) != std::string::npos) return true;
  return false;
}

// An hour of the dealer's bars, as they come: an envelope, structs with a start and an end.
const char * kHourly = R"({"statusCode":200,"data":[
 {"open":3607.14,"close":3596.71,"high":3626.71,"low":3585.68,"startDateTime":"2026-11-20T00:00:00+00:00","endDateTime":"2026-11-20T01:00:00+00:00"},
 {"open":3596.53,"close":3560.18,"high":3609.27,"low":3556.61,"startDateTime":"2026-11-20T01:00:00+00:00","endDateTime":"2026-11-20T02:00:00+00:00"},
 {"open":3559.81,"close":3547.22,"high":3560.84,"low":3526.37,"startDateTime":"2026-11-20T02:00:00+00:00","endDateTime":"2026-11-20T03:00:00+00:00"}]})";

// A provider with every metal and currency in one answer: a struct for each metal that holds its rows, which hold the currency.
const char * kNested = R"([
 {"Code":"AU","RateType":"P","RIC":"XAUFIXPM=","Prices":[
  {"Currency":"EUR","TradeTime":"2026-11-25T15:00:00","Bid":1.0937,"Ask":1.0939,"BidGram":101.87,"AskGram":101.89},
  {"Currency":"USD","TradeTime":"2026-11-25T15:14:00","Bid":3466.35,"Ask":3466.35,"BidGram":111.44,"AskGram":111.44},
  {"Currency":"EUR","TradeTime":"2026-11-26T15:00:00","Bid":1.0978,"Ask":1.098,"BidGram":104.79,"AskGram":104.81},
  {"Currency":"USD","TradeTime":"2026-11-26T15:03:00","Bid":3578.9,"Ask":3578.9,"BidGram":115.06,"AskGram":115.06}]},
 {"Code":"RH","RateType":"S","RIC":"RH","Prices":[
  {"Currency":"USD","TradeTime":"2026-11-25T09:31:00","Bid":8900,"Ask":8900,"BidGram":286.14,"AskGram":286.14}]}])";

}  // namespace

TEST(series_time_is_seconds_or_milliseconds_or_iso_with_or_without_a_zone) {
  Timestamp t = 0;
  CHECK(parseTime("1700000000", t) && t == 1700000000);
  CHECK(parseTime("1700000000000", t) && t == 1700000000);
  CHECK(parseTime("2026-11-26T15:03:00Z", t) && t == 1795705380);
  CHECK(parseTime("2026-11-26T15:03:00", t) && t == 1795705380);  // no zone: UTC
  CHECK(parseTime("2026-11-26T15:03:00+00:00", t) && t == 1795705380);
  CHECK(parseTime("2026-11-26T17:03:00+02:00", t) && t == 1795705380);
  CHECK(parseTime("2026-11-26T15:03:00.250", t) && t == 1795705380);
  CHECK(parseTime("2026-11-26 15:03:00", t) && t == 1795705380);
  CHECK(parseTime("2026-11-26", t) && t == 1795651200);
  CHECK(!parseTime("", t) && !parseTime("yesterday", t) && !parseTime("2026-13-40", t) && !parseTime("-5", t));
}

TEST(series_reads_rows_of_arrays_the_way_a_fund_history_comes) {
  const Result r = read("[[1700000000000, 10.5], [1700086400000, 10.6], [1700172800000, 10.4]]");
  CHECK(r.ok && r.series.size() == 1 && r.shape == Shape::RowsOfArrays);
  if (!r.ok) return;
  const Series & s = r.series[0];
  CHECK(s.bars.size() == 3 && s.bars[0].time == 1700000000 && s.bars[2].close == 10.4 && !isDefined(s.bars[0].open));
  CHECK(s.barSeconds == 86400 && s.labels.empty());
  // Five columns are a time and open, high, low, close; six have a volume.
  const Result bars = read("[[1700000000, 1, 3, 0.5, 2, 100], [1700000060, 2, 4, 1, 3, 200]]");
  CHECK(bars.ok && bars.series[0].bars[1].open == 2 && bars.series[0].bars[1].high == 4 && bars.series[0].bars[1].low == 1 && bars.series[0].bars[1].close == 3);
  CHECK(bars.series[0].bars[1].volume == 200 && bars.series[0].barSeconds == 60);
  const Result ohlc = read("[[1700000000, 1, 3, 0.5, 2], [1700000060, 2, 4, 1, 3]]");
  CHECK(ohlc.ok && ohlc.series[0].bars[0].close == 2 && !isDefined(ohlc.series[0].bars[0].volume));
  // Three columns could be a value and a volume or anything else: not guessed.
  const Result three = read("[[1700000000, 1, 3], [1700000060, 2, 4]]");
  CHECK(!three.ok && three.error.find("arrays of 3") != std::string::npos);
}

TEST(series_reads_structs_in_an_envelope_with_the_bar_length_from_the_rows) {
  const Result r = read(kHourly);
  CHECK(r.ok && r.series.size() == 1 && r.shape == Shape::Structs);
  if (!r.ok) return;
  const Series & s = r.series[0];
  CHECK(s.bars.size() == 3 && s.barSeconds == 3600 && s.bars[0].end - s.bars[0].time == 3600);
  CHECK(s.bars[1].open == 3596.53 && s.bars[1].high == 3609.27 && s.bars[1].low == 3556.61 && s.bars[1].close == 3560.18 && !isDefined(s.bars[1].volume));
  // The length that was asked for is not what counts: the same rows without an end say it by their steps.
  const Result steps = read(R"([{"t":"2026-11-20T00:00:00Z","c":1},{"t":"2026-11-20T06:00:00Z","c":2},{"t":"2026-11-20T12:00:00Z","c":3}])");
  CHECK(steps.ok && steps.series[0].barSeconds == 6 * 3600);
}

TEST(series_reads_the_bars_of_an_envelope_with_strings_for_numbers_and_the_newest_first) {
  const Result r = read(fixture("goldprice/bars-XAU-USD.json"));
  CHECK(r.ok && r.series.size() == 1 && r.shape == Shape::Structs);
  if (!r.ok) return;
  const Series & s = r.series[0];
  CHECK(s.bars.size() == 3 && s.bars[0].time < s.bars[1].time && s.bars[1].time < s.bars[2].time);  // oldest first
  CHECK(std::abs(s.bars[2].open - 3630.79) < 1e-9 && std::abs(s.bars[2].close - 3551.34) < 1e-9 && !isDefined(s.bars[2].volume));
  CHECK(noteSays(r, "fields not used: is_closed"));
}

TEST(series_reads_a_struct_for_each_series_and_tells_the_series_in_its_rows_apart_by_their_text) {
  const Result r = read(kNested);
  CHECK(r.ok && r.shape == Shape::Nested && r.series.size() == 3);
  if (!r.ok || r.series.size() != 3) return;
  CHECK(r.series[0].label("Code") == "AU" && r.series[0].label("Currency") == "EUR" && r.series[0].label("RIC") == "XAUFIXPM=");
  CHECK(r.series[1].label("Code") == "AU" && r.series[1].label("Currency") == "USD" && r.series[1].label("RateType") == "P");
  CHECK(r.series[2].label("Code") == "RH" && r.series[2].label("Currency") == "USD");
  CHECK(r.series[0].bars.size() == 2 && r.series[1].bars.size() == 2 && r.series[2].bars.size() == 1);
  // Bid and ask are what they are named; the gram's price is not a role, and is said.
  const Bar & eur = r.series[0].bars[1];
  CHECK(eur.bid == 1.0978 && eur.ask == 1.098 && !isDefined(eur.close));
  CHECK(r.series[1].bars[1].bid == r.series[1].bars[1].ask);
  CHECK(noteSays(r, "told apart by Currency") && noteSays(r, "fields not used: AskGram, BidGram"));
}

TEST(series_reads_a_struct_of_arrays_and_series_by_name) {
  const Result cols = read(R"({"t":[1700000000,1700086400],"o":[1,2],"h":[3,4],"l":[0.5,1.5],"c":[2,3],"v":[10,20]})");
  CHECK(cols.ok && cols.shape == Shape::Columns && cols.series.size() == 1 && cols.series[0].bars.size() == 2);
  CHECK(cols.series[0].bars[1].open == 2 && cols.series[0].bars[1].high == 4 && cols.series[0].bars[1].low == 1.5 && cols.series[0].bars[1].close == 3);
  CHECK(cols.series[0].bars[1].volume == 20 && cols.series[0].barSeconds == 86400);

  const Result byName = read(R"({"XAU":[[1700000000,1],[1700086400,2]],"XAG":[[1700000000,10],[1700086400,20]]})");
  CHECK(byName.ok && byName.shape == Shape::Named && byName.series.size() == 2);
  if (!byName.ok || byName.series.size() != 2) return;
  CHECK(byName.series[0].label("key") == "XAU" && byName.series[1].label("key") == "XAG" && byName.series[1].bars[1].close == 20);

  // Named series of structs, in an envelope.
  const Result both = read(R"({"data":{"XAU":[{"date":"2026-11-25","price":1},{"date":"2026-11-26","price":2}],"XAG":[{"date":"2026-11-25","price":3}]}})");
  CHECK(both.ok && both.series.size() == 2 && both.series[0].bars.size() == 2 && both.series[1].label("key") == "XAG");
}

TEST(series_one_series_has_no_labels_to_tell_it_apart_and_one_that_is_the_same_in_every_row_is_its_label) {
  const Result r = read(R"([{"symbol":"XAU","t":1700000000,"c":1},{"symbol":"XAU","t":1700086400,"c":2}])");
  CHECK(r.ok && r.series.size() == 1 && r.series[0].label("symbol") == "XAU" && r.series[0].bars.size() == 2);
}

TEST(series_an_answer_that_says_it_is_an_error_is_no_series_and_says_what) {
  const Result r = read(R"({"statusCode":400,"error":{"type":"BAD_REQUEST","description":"Could not resolve argument `symbol`."}})");
  CHECK(!r.ok && r.error == "Could not resolve argument `symbol`.");
  const Result text = read(R"({"error":"rate limited","data":[[1700000000,1]]})");
  CHECK(!text.ok && text.error == "rate limited");
  const Result status = read(R"({"status":503,"message":"try later"})");
  CHECK(!status.ok && status.error == "status 503: try later");
  const Result fine = read(R"({"error":null,"status":200,"data":[[1700000000,1]]})");
  CHECK(fine.ok && fine.series[0].bars.size() == 1);
  const Result html = read("<html>maintenance</html>");
  CHECK(!html.ok && html.error == "not JSON");
  CHECK(!read("{}").ok && !read("5").ok && !read("{\"a\":1}").ok);
}

TEST(series_does_not_guess_between_things_it_cannot_tell_apart) {
  // Several numbers and none named a value.
  const Result numbers = read(R"([{"when":"2026-11-25","a":1,"b":2},{"when":"2026-11-26","a":3,"b":4}])");
  CHECK(!numbers.ok && numbers.error.find("several numbers") != std::string::npos);
  // A row that holds two arrays of rows.
  const Result two = read(R"([{"Code":"AU","A":[{"t":1700000000,"c":1}],"B":[{"t":1700000000,"c":2}]}])");
  CHECK(!two.ok && two.error.find("more than one array of rows") != std::string::npos);
  // An envelope with two members that could be the series, neither named like one.
  const Result members = read(R"({"first":[{"t":1700000000,"c":1}],"second":{"nested":[{"t":1700000000,"c":2}]},"third":[{"t":1700000000,"c":3}]})");
  CHECK(!members.ok && members.error.find("ambiguous") != std::string::npos);
  // The same where one is named like a series: that one.
  const Result named = read(R"({"first":[{"t":1700000000,"c":1}],"bars":[{"t":1700000000,"c":3}]})");
  CHECK(named.ok && named.series.size() == 2);  // two arrays of rows are series by name: the answer is not an envelope
  const Result lone = read(R"({"payload":{"inner":[{"t":1700000000,"c":2}]}})");
  CHECK(lone.ok && lone.series.size() == 1);
}

TEST(series_the_hints_say_what_the_heuristics_cannot) {
  const std::string body = R"({"result":{"x":[{"when":"2026-11-25","a":1,"b":2},{"when":"2026-11-26","a":3,"b":4}],"y":[{"when":"2026-11-25","a":9,"b":9}]}})";
  Hints rows;
  rows.rows = "/result/x";
  rows.fields = {{"time", "when"}, {"close", "b"}};
  const Result r = read(body, rows);
  CHECK(r.ok && r.series.size() == 1 && r.series[0].bars.size() == 2 && r.series[0].bars[1].close == 4);
  Hints missing;
  missing.rows = "/result/z";
  CHECK(read(body, missing).error == "nothing at /result/z");
}

TEST(series_sorts_oldest_first_keeps_the_later_row_of_a_start_and_leaves_out_what_is_not_a_bar) {
  const Result r = read(R"([[1700172800, 3], [1700000000, 1], [1700086400, 2], [1700086400, 2.5], ["x", 5], [1700259200, null]])");
  CHECK(r.ok && r.series[0].bars.size() == 3 && r.series[0].bars[1].close == 2.5 && r.series[0].bars[0].time == 1700000000);
  CHECK(noteSays(r, "2 rows that are not bars left out"));
  const Result structs = read(R"([{"t":1700000000,"c":1},{"t":"bad","c":2},{"c":3},5])");
  CHECK(structs.ok && structs.series[0].bars.size() == 1 && noteSays(structs, "3 rows that are not bars left out"));
}

TEST(series_an_answer_with_no_rows_is_an_empty_series_and_not_a_failure) {
  const Result r = read(R"({"statusCode":200,"data":[]})");
  CHECK(r.ok && r.series.size() == 1 && r.series[0].bars.empty());
  const Result nested = read(R"([{"Code":"AU","Prices":[]},{"Code":"RH","Prices":[]}])");
  CHECK(nested.ok && nested.series.size() == 2 && nested.series[1].label("Code") == "RH" && nested.series[1].bars.empty());
}
