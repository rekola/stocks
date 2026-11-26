#include "TestFramework.h"
#include "market/Bars.h"
#include "model/Dates.h"

using namespace market;

namespace {
// 2012-02-01 05:01:00 UTC
constexpr Timestamp kSample = 1328072460;
}

TEST(market_bar_keys_of_the_sample_row) {
  CHECK(barKey(Resolution::Hour1, kSample) == 368909);
  CHECK(barKey(Resolution::Day, kSample) == 734534);
  CHECK(barKey(Resolution::Min5, kSample) == 1328072460 / 300);
  CHECK(barKey(Resolution::Day, 0) == 719163);  // 1970-01-01, Date_to_Days
  CHECK(barKey(Resolution::Day, 86399) == 719163);
  CHECK(barKey(Resolution::Day, 86400) == 719164);
  CHECK(barKey(Resolution::Hour1, 3599) == 0 && barKey(Resolution::Hour1, 3600) == 1);
  CHECK(barKey(Resolution::Min5, 299) == 0 && barKey(Resolution::Min5, 300) == 1);
}

TEST(market_bar_start_is_the_inverse_of_the_key) {
  for (Resolution level : kLevels) {
    const int64_t key = barKey(level, kSample);
    CHECK(barKey(level, barStart(level, key)) == key);
    CHECK(barStart(level, key) <= kSample);
    CHECK(barKey(level, barStart(level, key) - 1) == key - 1);
  }
}

TEST(market_levels_are_parsed_by_name) {
  CHECK(parseLevel("5m") == Resolution::Min5);
  CHECK(parseLevel("1h") == Resolution::Hour1);
  CHECK(parseLevel("1d") == Resolution::Day);
  CHECK(!parseLevel("all") && !parseLevel("1m") && !parseLevel(""));
}

TEST(market_aggregate_takes_first_open_max_high_min_low_last_close) {
  // Four minutes in the hour 05:xx, shuffled; the 06:00 minute is the next hour.
  std::vector<MinuteBar> minutes = {
      {kSample + 120, 3, 9, 1, 4},  // 05:03
      {kSample, 1, 5, 0.5, 2},      // 05:01
      {kSample + 60, 2, 7, 1.5, 3}, // 05:02
      {kSample + 3540, 5, 6, 4, 5.5},  // 06:00
  };
  const auto hourly = aggregate(Resolution::Hour1, minutes);
  CHECK(hourly.size() == 2);
  CHECK(hourly[0].key == 368909 && hourly[1].key == 368910);
  CHECK(hourly[0].open == 1 && hourly[0].high == 9 && hourly[0].low == 0.5 && hourly[0].close == 4);
  CHECK(hourly[1] == (Bar{368910, 5, 6, 4, 5.5}));
  CHECK(aggregate(Resolution::Day, minutes).size() == 1);
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
  // 2012-02-01 23:30 UTC and 2012-02-02 00:00 UTC are different days though both are the 1st in EST.
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

TEST(market_windows_end_at_the_last_bar) {
  CHECK(windowFirstKey(Resolution::Min5, 1000) == 1000 - 288 + 1);
  CHECK(windowFirstKey(Resolution::Hour1, 5000) == 5000 - 744 + 1);
  const int64_t last = dayNumber(2026, 11, 13);
  CHECK(windowFirstKey(Resolution::Day, last) == dayNumber(2016, 11, 13) + 1);
  // 29 February ten years back is not there in a non-leap year
  CHECK(windowFirstKey(Resolution::Day, dayNumber(2024, 2, 29)) == dayNumber(2014, 2, 28) + 1);
  CHECK(windowFirstKey(Resolution::Day, dayNumber(2020, 2, 29)) == dayNumber(2010, 2, 28) + 1);
  // the first instant to cover
  CHECK(windowStartTime(Resolution::Day, kSample) == barStart(Resolution::Day, windowFirstKey(Resolution::Day, 734534)));
}

TEST(market_cut_to_window_drops_what_is_outside) {
  std::vector<Bar> bars;
  for (int64_t k = 100; k < 1000; k++) bars.push_back({k, 1, 1, 1, 1});
  const auto cut = cutToWindow(Resolution::Min5, bars, 900);
  CHECK(cut.size() == 288 && cut.front().key == 613 && cut.back().key == 900);
}
