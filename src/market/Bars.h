#pragma once

#include "model/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The bars the market-data server keeps, and how a minute of quotes becomes one. Nothing here knows a provider.
//
// Three levels are stored, named by the model's `Resolution` (the code that goes in the database is its value):
//
//   5m (Min5)   5-minute bars   the last 1 day     feeds the day's sparkline
//   1h (Hour1)  hourly bars     the last 31 days   feeds the week's and the month's
//   1d (Day)    daily bars      the last 10 years  feeds 3 months, 1 year, 3 years and 10 years
//
// Each period ends at the instrument's last available bar (not at the current time), so a window is given by its last key.
// There are no 1-minute bars here: they are the raw files' (the cache's) business.
namespace market {

// A minute of quotes, as an adapter reads it: the start of the minute as UTC seconds.
struct MinuteBar {
  Timestamp time = 0;
  double open = 0, high = 0, low = 0, close = 0;
};

// A stored bar: its key is the number of the bar (see barKey).
struct Bar {
  int64_t key = 0;
  double open = 0, high = 0, low = 0, close = 0;
  friend bool operator==(const Bar &, const Bar &) = default;
};

// The levels, finest first.
inline constexpr std::array<Resolution, 3> kLevels = {Resolution::Min5, Resolution::Hour1, Resolution::Day};

bool isLevel(Resolution);
// "5m", "1h" or "1d" (the model's names); none for anything else, "all" included.
std::optional<Resolution> parseLevel(const std::string & text);

// THE bucketing: the key of the bar that an instant (UTC seconds) is in.
//   5m  floor(seconds / 300)
//   1h  floor(seconds / 3600), the hour number
//   1d  floor(seconds / 86400) + 719163, the day number (Perl's Date::Calc Date_to_Days: 1 January of the year 1 is 1)
// All of it in UTC. Where a US session that opens at 09:30 New York time falls on these numbers is not decided, so a change to
// the bucketing is a change to this function and to barStart() below, and nowhere else.
int64_t barKey(Resolution level, Timestamp utcSeconds);
// The instant a bar starts at (UTC seconds): the inverse of barKey for the first instant of the bar.
Timestamp barStart(Resolution level, int64_t key);

// How much a level keeps, counted back from its last bar: the first key of the window that ends at `lastKey`. 5m: 1 day (288 bars),
// 1h: 31 days (744 bars), 1d: 10 years (the days after the same date ten years before).
int64_t windowFirstKey(Resolution level, int64_t lastKey);
// The first instant of the window that ends with the bar that `lastInstant` is in: what the files that feed the level have to cover.
Timestamp windowStartTime(Resolution level, Timestamp lastInstant);

// The bars of the minutes, ascending by key. A bar is its first open, its highest high, its lowest low and its last close. The
// minutes need not be in order; two with the same time are one, the later in the input winning.
std::vector<Bar> aggregate(Resolution level, std::vector<MinuteBar> minutes);
// Two runs of bars that follow each other (the end of one and the start of the next may share a key: a bar that a file boundary
// cut in two): `later` is appended to `earlier`, and a key in both is one bar made of the two.
std::vector<Bar> joinBars(std::vector<Bar> earlier, std::vector<Bar> later);
// The bars of the window that ends at `lastKey`, that is the keys from windowFirstKey to lastKey.
std::vector<Bar> cutToWindow(Resolution level, std::vector<Bar> bars, int64_t lastKey);

}  // namespace market
