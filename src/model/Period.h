#pragma once

#include "model/Store.h"

#include <array>
#include <vector>

// The time periods a table can show, and what each one means for a row: which series feeds its trace, and the base price its
// change is measured from. The change and the trace of a row always show the same period, and the sign of that change is the
// colour of both (trendUp). Nothing here knows the screen or where the bars came from.
//
//   day      the last session in the data (the UTC date of the latest bar; there are no market calendars yet): the trace is that
//            session's own bars, the base the previous close. 5-minute bars, else 1, 2, 15 or 30-minute ones.
//   week     the last 7 calendar days: hourly bars (the daily ones if there are no hourly), the base the first bar's close.
//   month    the last 30 days: daily bars.
//   quarter  the last 90 days (three months): daily bars.
//   year     the last 365 days: weekly bars, or daily ones that reach back that far.
//
// A rate (a row quoted in per cent, `isRate`) is not a price: its change is the difference between the last value and the base, in
// basis points (a base of 0, or a negative one, is as good as any), where a price's is the percentage change. The base, the last
// value and the trace are the same for both.
//
// The last price of a period is the Price column's: a period's own series is loaded now and then, while the daily bar (the one
// the Price column shows) is replaced every few minutes. Where the latest daily bar is at least as new as the series' last bar
// (the day: its date is the session's; the week: on or after the day of the last hourly bar; the year: on or after the start
// of the last weekly bar) its close is the period's `last`, and the trace ends with it as one more point when it differs from
// the last bar's close, so the Price, the Change and the line agree. The base is never moved. A daily bar older than the series
// changes nothing; a fund's daily bar is its latest NAV and works the same way.
//
// A period is shown only from bars that cover it: the first bar must be within a third of the period of its start (so the 60
// daily bars the made-up data has are a quarter, one daily bar is not a week). Else the row has no number for the period,
// never a wrong one.
enum class Period : uint8_t { Day, Week, Month, Quarter, Year };

inline constexpr size_t kPeriodCount = 5;
// What the table shows until a period is chosen: the three months, the table as it was before there were periods.
inline constexpr Period kDefaultPeriod = Period::Quarter;

struct PeriodInfo {
  const char * id;     // "day": the commands are show-period-<id>
  const char * label;  // "1d", on the tag and in the change heading ("Change 1d")
  const char * trace;  // "1 day", the heading of the trace column
  int64_t days;        // how many calendar days it spans (the day: one session)
  std::array<Resolution, 5> feeds;  // the series that feed it, finest first
  size_t feedCount;
  size_t primary;      // the first `primary` feeds are the period's own bars; the rest stand in where those are missing
};
const PeriodInfo & info(Period);
// The period after / before, wrapping round.
Period nextPeriod(Period);
Period previousPeriod(Period);

// What a row shows for a period.
struct PeriodView {
  bool rate = false;              // the row is a rate: its change is a difference in basis points
  bool primary = false;           // the bars are the period's own (a stand-in's, or none, is false)
  double base = kUndefined;       // the price the change is measured from
  double last = kUndefined;       // the last price of the period
  std::vector<float> closes;      // the trace: the period's closes, oldest first (only when asked for)
  std::vector<Timestamp> times;   // when each close is, as UTC seconds (a daily or longer bar: midnight of its date), one for each of `closes`
  bool dated = false;             // the bars are days or longer, so the times say a date and no time of day
  bool hasChange() const { return isDefined(base) && isDefined(last) && (rate || base != 0); }
  // For a rate in basis points, else in percent; undefined where there is no number.
  double change() const {
    if (!hasChange()) return kUndefined;
    return rate ? (last - base) * 100.0 : (last / base - 1.0) * 100.0;
  }
};
// Whether the edge is a rate: its quote is the per cent unit (a Unit instrument with that symbol). The one rule for it.
bool isRate(const Snapshot &, const EdgeKey &);
PeriodView periodView(const Snapshot &, const EdgeKey &, Period, bool withTrace);

// The colour rule, in one place: up (green) or down (red) by the sign of a change, none for no number.
std::optional<bool> trendUp(double change);
