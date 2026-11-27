#pragma once

#include "model/Types.h"

#include <array>
#include <optional>
#include <string>
#include <vector>

// How a minute of quotes becomes the bars the model keeps. Nothing here knows a provider or a database. The bars are the model's own
// `Candle`s, written with `StoreWrites::appendCandles` like any source's, at the resolutions the periods of the application are drawn from
// (Period.h): 5-minute bars for the day, hourly for the week, daily for the month and the quarter, weekly for the year, 3 years and 10 years.
// How much of each the summaries keep is the model's (`defaultKeep`: 1 day, 7 days, 365 days and 10 years); the bars written to a BarStore
// are the ones this writes, which for now are those windows.
namespace market {

// A minute of quotes, as an adapter reads it: the start of the minute as UTC seconds.
struct MinuteBar {
  Timestamp time = 0;
  double open = 0, high = 0, low = 0, close = 0;
};

// The resolutions that are made, shortest first.
inline constexpr std::array<Resolution, 4> kResolutions = {Resolution::Min5, Resolution::Hour1, Resolution::Day, Resolution::Week};

// "5m", "1h", "1d" or "1w" (the model's names); none for anything else, "all" included.
std::optional<Resolution> parseResolution(const std::string & text);

// THE bucketing: the key of the bar of the given size that an instant (UTC seconds) is in.
//   5-minute and hourly bars: floor(seconds / length), the slot number (the hour number for an hour)
//   day bars: floor(seconds / 86400) + 719163, the day number (Perl's Date::Calc Date_to_Days: 1 January of the year 1 is 1)
//   week bars: the day number of the Monday of the week
// All of it in UTC. Where a US session that opens at 09:30 New York time falls on these numbers is not decided, so a change to the
// bucketing is a change to this function and to barStart() below, and nowhere else.
int64_t barKey(Resolution, Timestamp utcSeconds);
// The first instant of a bar (UTC seconds): the inverse of barKey.
Timestamp barStart(Resolution, int64_t key);

// How far back from the last bar a resolution is kept (the model's `defaultKeep`), and the first instant of the window that ends with the
// bar `lastInstant` is in: what the files that feed the resolution have to reach back to.
int64_t keepSeconds(Resolution);
Timestamp windowStartTime(Resolution, Timestamp lastInstant);

// The bars of the minutes, ascending by key. A bar is its first open, its highest high, its lowest low and its last close; it has no volume.
// The minutes need not be in order; two with the same time are one, the later in the input winning.
std::vector<Candle> aggregate(Resolution, std::vector<MinuteBar> minutes);
// Two runs of bars that follow each other (the end of one and the start of the next may share a key: a bar that a file boundary cut in two):
// `later` is appended to `earlier`, and a key in both is one bar made of the two.
std::vector<Candle> joinBars(std::vector<Candle> earlier, std::vector<Candle> later);
// The bars of the window of the resolution that ends with the bar that `lastInstant` is in (the model's rule for a queue).
std::vector<Candle> cutToWindow(Resolution, std::vector<Candle> bars, Timestamp lastInstant);

}  // namespace market
