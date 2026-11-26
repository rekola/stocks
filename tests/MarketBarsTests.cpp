#include "TestFramework.h"
#include "market/Bars.h"
#include "model/Dates.h"

using namespace market;

namespace {
constexpr Timestamp kSample = 1328072460;  // 2012-02-01 05:01:00 UTC
Timestamp at(int y, unsigned m, unsigned d, int hour = 0, int minute = 0) { return daysSinceUnixEpoch(y, m, d) * 86400 + hour * 3600 + minute * 60; }
}  // namespace

TEST(market_bar_keys_of_the_sample_row) {
  CHECK(barKey(Resolution::Hour1, kSample) == 368909);
  CHECK(barKey(Resolution::Day, kSample) == 734534);
  CHECK(barKey(Resolution::Min5, kSample) == 1328072460 / 300);
  CHECK(barKey(Resolution::Hour4, kSample) == 1328072460 / 14400);
  CHECK(barKey(Resolution::Day, 0) == 719163);  // 1970-01-01, Date_to_Days
  CHECK(barKey(Resolution::Day, 86399) == 719163);
  CHECK(barKey(Resolution::Day, 86400) == 719164);
  CHECK(barKey(Resolution::Hour1, 3599) == 0 && barKey(Resolution::Hour1, 3600) == 1);
  CHECK(barKey(Resolution::Min5, 299) == 0 && barKey(Resolution::Min5, 300) == 1);
  CHECK(barKey(Resolution::Hour4, 14399) == 0 && barKey(Resolution::Hour4, 14400) == 1);
}

TEST(market_a_week_is_keyed_by_the_day_number_of_its_monday) {
  // 2026-11-09 was a Monday
  const int64_t monday = dayNumber(2026, 11, 9);
  CHECK(weekday(monday) == 0);
  for (int d = 0; d < 7; d++) CHECK(barKey(Resolution::Week, at(2026, 11, 9 + d, 23, 59)) == monday);
  CHECK(barKey(Resolution::Week, at(2026, 11, 16)) == monday + 7);
  CHECK(barKey(Resolution::Week, at(2026, 11, 8, 23, 59)) == monday - 7);  // a Sunday belongs to the week before
}

TEST(market_bar_start_is_the_inverse_of_the_key) {
  for (Resolution bar : {Resolution::Min5, Resolution::Hour1, Resolution::Hour4, Resolution::Day, Resolution::Week}) {
    const int64_t key = barKey(bar, kSample);
    CHECK(barKey(bar, barStart(bar, key)) == key);
    CHECK(barStart(bar, key) <= kSample);
    CHECK(barKey(bar, barStart(bar, key) - 1) == key - 1 || bar == Resolution::Week && barKey(bar, barStart(bar, key) - 1) == key - 7);
  }
}

TEST(market_levels_are_named_by_the_window_and_know_their_bars) {
  CHECK(parseLevel("day") == Level::Day && parseLevel("week") == Level::Week && parseLevel("month") == Level::Month);
  CHECK(parseLevel("year") == Level::Year && parseLevel("3years") == Level::Years3 && parseLevel("10years") == Level::Years10);
  CHECK(!parseLevel("all") && !parseLevel("5m") && !parseLevel(""));
  CHECK(barOf(Level::Day) == Resolution::Min5 && barOf(Level::Week) == Resolution::Hour1 && barOf(Level::Month) == Resolution::Hour4);
  CHECK(barOf(Level::Year) == Resolution::Day && barOf(Level::Years3) == Resolution::Week && barOf(Level::Years10) == Resolution::Week);
  for (Level level : kLevels) CHECK(parseLevel(name(level)) == level);
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
  CHECK(hourly[0].key == 368909 && hourly[1].key == 368910);
  CHECK(hourly[0].open == 1 && hourly[0].high == 9 && hourly[0].low == 0.5 && hourly[0].close == 4);
  CHECK(hourly[1] == (Bar{368910, 5, 6, 4, 5.5}));
  CHECK(aggregate(Resolution::Day, minutes).size() == 1);
  CHECK(aggregate(Resolution::Week, minutes).size() == 1);
  CHECK(aggregate(Resolution::Hour4, minutes).size() == 1);  // 04:00 to 08:00
  const auto five = aggregate(Resolution::Min5, minutes);
  CHECK(five.size() == 2);  // 05:01..05:03 are one slot (05:00), 06:00 another
  CHECK(five[0].open == 1 && five[0].close == 4);
}

TEST(market_aggregate_keeps_the_later_of_two_minutes_with_one_time) {
  const auto bars = aggregate(Resolution::Hour1, {{kSample, 1, 1, 1, 1}, {kSample, 2, 2, 2, 2}});
  CHECK(bars.size() == 1 && bars[0].open == 2);
  CHECK(aggregate(Resolution::Day, {}).empty());
}

TEST(market_a_day_is_the_utc_day_not_the_est_day) {
  const auto bars = aggregate(Resolution::Day, {{kSample + 18 * 3600 + 29 * 60, 1, 1, 1, 1}, {kSample + 18 * 3600 + 59 * 60, 2, 2, 2, 2}});
  CHECK(bars.size() == 2 && bars[1].key == bars[0].key + 1);
}

TEST(market_join_merges_a_bar_cut_in_two_by_a_file_boundary) {
  const std::vector<Bar> a = {{1, 1, 5, 1, 2}, {2, 2, 6, 2, 3}};
  const std::vector<Bar> b = {{2, 9, 8, 0.5, 4}, {3, 4, 4, 4, 4}};
  const auto joined = joinBars(a, b);
  CHECK(joined.size() == 3);
  CHECK(joined[1] == (Bar{2, 2, 8, 0.5, 4}));  // first open of the earlier, max, min, last close of the later
  CHECK(joined[2].key == 3);
  CHECK(joinBars({}, b) == b);
}

TEST(market_windows_end_at_the_last_bar_and_start_the_day_after_the_date_a_period_back) {
  const Timestamp last = at(2026, 11, 13, 21, 58);
  CHECK(windowFirstKey(Level::Day, last) == barKey(Resolution::Min5, last) - 287);
  CHECK(windowFirstKey(Level::Week, last) == barKey(Resolution::Hour1, last) - 167);
  // a month back: 2026-10-13, so the window starts on the 14th at midnight
  CHECK(barStart(Resolution::Hour4, windowFirstKey(Level::Month, last)) == at(2026, 10, 14));
  CHECK(windowFirstKey(Level::Year, last) == dayNumber(2025, 11, 14));
  // 29 February a period back is not there in a year that is not a leap year
  CHECK(windowFirstKey(Level::Year, at(2024, 2, 29, 12)) == dayNumber(2023, 2, 28) + 1);
  CHECK(windowFirstKey(Level::Month, at(2026, 3, 31, 12)) == barKey(Resolution::Hour4, at(2026, 3, 1)));  // 28 February + 1
  // weeks start on a Monday: the first one from the day after three (ten) years back
  CHECK(windowFirstKey(Level::Years3, last) == dayNumber(2023, 11, 20));  // 2023-11-14 is a Tuesday
  CHECK(windowFirstKey(Level::Years10, last) == dayNumber(2016, 11, 14));  // and this one a Monday
  CHECK(weekday(windowFirstKey(Level::Years3, last)) == 0 && weekday(windowFirstKey(Level::Years10, last)) == 0);
  CHECK(windowStartTime(Level::Years10, last) == at(2016, 11, 14));
}

TEST(market_cut_to_window_drops_what_is_outside) {
  const Timestamp last = at(2026, 11, 13, 21, 58);
  std::vector<Bar> bars;
  const int64_t end = barKey(Resolution::Min5, last);
  for (int64_t k = end - 1000; k <= end + 5; k++) bars.push_back({k, 1, 1, 1, 1});
  const auto cut = cutToWindow(Level::Day, bars, last);
  CHECK(cut.size() == 288 && cut.front().key == end - 287 && cut.back().key == end);
}
