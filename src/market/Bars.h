#pragma once

#include "model/Types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The bars the market-data server keeps, and how a minute of quotes becomes one. Nothing here knows a provider.
//
// A level is a sparkline window with the bar size that draws it well, and the period it keeps, counted back from the instrument's last
// available bar (not from the current time):
//
//   level     bar            keeps            key of a bar
//   day       5 minutes      1 day            the 5-minute slot number, floor(unix seconds / 300)
//   week      1 hour         7 days           the hour number, floor(unix seconds / 3600)
//   month     4 hours        1 month          the 4-hour slot number, floor(unix seconds / 14400)
//   year      1 day          1 year           the day number (Perl's Date::Calc Date_to_Days: 1 January of the year 1 is 1)
//   3years    1 week         3 years          the day number of the Monday that starts the week
//   10years   1 week         10 years         the same
//
// There are no 1-minute bars here: they are the raw files' (the cache's) business. All of it is in UTC.
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

// The levels, shortest period first. The value is what the database holds.
enum class Level : uint8_t { Day = 0, Week = 1, Month = 2, Year = 3, Years3 = 4, Years10 = 5 };
inline constexpr std::array<Level, 6> kLevels = {Level::Day, Level::Week, Level::Month, Level::Year, Level::Years3, Level::Years10};

// "day", "week", "month", "year", "3years", "10years".
const char * name(Level);
// none for anything else ("all" included).
std::optional<Level> parseLevel(const std::string & text);
// The size of the level's bars.
Resolution barOf(Level);

// THE bucketing: the key of the bar of the given size that an instant (UTC seconds) is in.
//   5-minute, hour and 4-hour bars: floor(seconds / length), the slot number
//   day bars: floor(seconds / 86400) + 719163, the day number
//   week bars: the day number of the Monday of the week, which is the key of the bar
// All of it in UTC. Where a US session that opens at 09:30 New York time falls on these numbers is not decided, so a change to the
// bucketing is a change to this function and to barStart() below, and nowhere else.
int64_t barKey(Resolution bar, Timestamp utcSeconds);
// The first instant of a bar (UTC seconds): the inverse of barKey.
Timestamp barStart(Resolution bar, int64_t key);

// The first key of the level's window that ends with the bar `lastInstant` is in, and the first instant it covers: what the files that
// feed the level have to reach back to. A window starts the day after the same date a period back (29 February becomes the 28th); a
// level of weeks starts at the first Monday from there, so that it holds whole weeks only.
int64_t windowFirstKey(Level, Timestamp lastInstant);
Timestamp windowStartTime(Level, Timestamp lastInstant);

// The bars of the minutes, ascending by key. A bar is its first open, its highest high, its lowest low and its last close. The
// minutes need not be in order; two with the same time are one, the later in the input winning.
std::vector<Bar> aggregate(Resolution bar, std::vector<MinuteBar> minutes);
// Two runs of bars that follow each other (the end of one and the start of the next may share a key: a bar that a file boundary
// cut in two): `later` is appended to `earlier`, and a key in both is one bar made of the two.
std::vector<Bar> joinBars(std::vector<Bar> earlier, std::vector<Bar> later);
// The bars of the level's window that ends with the bar `lastInstant` is in.
std::vector<Bar> cutToWindow(Level, std::vector<Bar> bars, Timestamp lastInstant);

}  // namespace market
