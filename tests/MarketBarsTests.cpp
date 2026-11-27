#include "TestFramework.h"
#include "market/Bars.h"
#include "model/Dates.h"
#include "model/Period.h"

using namespace market;

namespace {
constexpr Timestamp kSample = 1328072460;  // 2012-02-01 05:01:00 UTC
Timestamp at(int y, unsigned m, unsigned d, int hour = 0, int minute = 0) { return daysSinceUnixEpoch(y, m, d) * 86400 + hour * 3600 + minute * 60; }
}  // namespace

TEST(market_bar_keys_of_the_sample_row) {
  CHECK(barKey(Resolution::Hour1, kSample) == 368909);
  CHECK(barKey(Resolution::Day, kSample) == 734534);
  CHECK(barKey(Resolution::Min5, kSample) == 1328072460 / 300);
  CHECK(barKey(Resolution::Day, 0) == 719163);  // 1970-01-01, Date_to_Days
  CHECK(barKey(Resolution::Day, 86399) == 719163 && barKey(Resolution::Day, 86400) == 719164);
  CHECK(barKey(Resolution::Hour1, 3599) == 0 && barKey(Resolution::Hour1, 3600) == 1);
  CHECK(barKey(Resolution::Min5, 299) == 0 && barKey(Resolution::Min5, 300) == 1);
}

TEST(market_a_week_is_keyed_by_the_day_number_of_its_monday) {
  const int64_t monday = dayNumber(2026, 11, 9);  // a Monday
  CHECK(weekday(monday) == 0);
  for (int d = 0; d < 7; d++) CHECK(barKey(Resolution::Week, at(2026, 11, 9 + d, 23, 59)) == monday);
  CHECK(barKey(Resolution::Week, at(2026, 11, 16)) == monday + 7);
  CHECK(barKey(Resolution::Week, at(2026, 11, 8, 23, 59)) == monday - 7);  // a Sunday belongs to the week before
}

TEST(market_bar_start_is_the_inverse_of_the_key) {
  for (Resolution r : kResolutions) {
    const int64_t key = barKey(r, kSample);
    CHECK(barKey(r, barStart(r, key)) == key);
    CHECK(barStart(r, key) <= kSample);
    CHECK(barKey(r, barStart(r, key) - 1) < key);
  }
}

TEST(market_resolutions_are_parsed_by_the_models_names) {
  CHECK(parseResolution("5m") == Resolution::Min5 && parseResolution("1h") == Resolution::Hour1);
  CHECK(parseResolution("1d") == Resolution::Day && parseResolution("1w") == Resolution::Week);
  CHECK(!parseResolution("all") && !parseResolution("1m") && !parseResolution(""));
}

TEST(market_how_much_is_kept_is_what_the_application_keeps_for_its_periods) {
  CHECK(keepSeconds(Resolution::Min5) == 86400);
  CHECK(keepSeconds(Resolution::Hour1) == 7 * 86400);
  CHECK(keepSeconds(Resolution::Day) == 365 * 86400);
  CHECK(keepSeconds(Resolution::Week) == 3650 * 86400);
  for (Resolution r : kResolutions) CHECK(keepSeconds(r) == defaultKeep(r)->count());
}

TEST(market_aggregate_takes_first_open_max_high_min_low_last_close) {
  std::vector<MinuteBar> minutes = {
      {kSample + 120, 3, 9, 1, 4},     // 05:03
      {kSample, 1, 5, 0.5, 2},         // 05:01
      {kSample + 60, 2, 7, 1.5, 3},    // 05:02
      {kSample + 3540, 5, 6, 4, 5.5},  // 06:00
  };
  const auto hourly = aggregate(Resolution::Hour1, minutes);
  CHECK(hourly.size() == 2);
  CHECK(hourly.size() == 2 && hourly[0].time == 368909 && hourly[1].time == 368910);
  CHECK(hourly[0].open == 1 && hourly[0].high == 9 && hourly[0].low == 0.5 && hourly[0].close == 4 && !isDefined(hourly[0].volume));
  CHECK(hourly[1].open == 5 && hourly[1].high == 6 && hourly[1].low == 4 && hourly[1].close == 5.5);
  CHECK(aggregate(Resolution::Day, minutes).size() == 1 && aggregate(Resolution::Week, minutes).size() == 1);
  const auto five = aggregate(Resolution::Min5, minutes);
  CHECK(five.size() == 2 && five[0].open == 1 && five[0].close == 4);  // 05:01..05:03 are one slot (05:00), 06:00 another
}

TEST(market_aggregate_keeps_the_later_of_two_minutes_with_one_time_and_a_day_is_the_utc_day) {
  const auto bars = aggregate(Resolution::Hour1, {{kSample, 1, 1, 1, 1}, {kSample, 2, 2, 2, 2}});
  CHECK(bars.size() == 1 && bars[0].open == 2);
  CHECK(aggregate(Resolution::Day, {}).empty());
  // 2012-02-01 23:30 UTC and 2012-02-02 00:00 UTC are two days, though in HistData's EST both are the 1st
  const auto days = aggregate(Resolution::Day, {{kSample + 18 * 3600 + 29 * 60, 1, 1, 1, 1}, {kSample + 18 * 3600 + 59 * 60, 2, 2, 2, 2}});
  CHECK(days.size() == 2 && days[1].time == days[0].time + 1);
}

TEST(market_join_merges_a_bar_cut_in_two_by_a_file_boundary) {
  const std::vector<Candle> a = {{1, 1, 5, 1, 2, kUndefined}, {2, 2, 6, 2, 3, kUndefined}};
  const std::vector<Candle> b = {{2, 9, 8, 0.5, 4, kUndefined}, {3, 4, 4, 4, 4, kUndefined}};
  const auto joined = joinBars(a, b);
  CHECK(joined.size() == 3);
  CHECK(joined.size() == 3 && joined[1].open == 2 && joined[1].high == 8 && joined[1].low == 0.5 && joined[1].close == 4);  // first open, max, min, last close
  CHECK(joined.size() == 3 && joined[2].time == 3);
  CHECK(joinBars({}, b).size() == 2);
}

TEST(market_windows_end_at_the_last_bar_by_the_models_rule) {
  const Timestamp last = at(2026, 11, 13, 21, 58);
  std::vector<Candle> bars;
  const int64_t end = barKey(Resolution::Min5, last);
  for (int64_t k = end - 1000; k <= end + 5; k++) bars.push_back({k, 1, 1, 1, 1, kUndefined});
  const auto cut = cutToWindow(Resolution::Min5, bars, last);
  CHECK(cut.size() == 289 && cut.front().time == end - 288 && cut.back().time == end);  // the day back from the last bar, both ends in
  // what the files have to reach back to
  CHECK(windowStartTime(Resolution::Day, last) == at(2025, 11, 13));
  CHECK(windowStartTime(Resolution::Hour1, last) == at(2026, 11, 6, 21));
  CHECK(windowStartTime(Resolution::Week, last) == barStart(Resolution::Week, barKey(Resolution::Week, last - 3650 * 86400)));
}
