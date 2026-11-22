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
  bool primary = false;           // the bars are the period's own (a stand-in's, or none, is false)
  double base = kUndefined;       // the price the change is measured from
  double last = kUndefined;       // the last price of the period
  std::vector<float> closes;      // the trace: the period's closes, oldest first (only when asked for)
  bool hasChange() const { return isDefined(base) && isDefined(last) && base != 0; }
  // In percent; undefined where there is no number.
  double change() const { return hasChange() ? (last / base - 1.0) * 100.0 : kUndefined; }
};
PeriodView periodView(const Snapshot &, const EdgeKey &, Period, bool withTrace);

// The colour rule, in one place: up (green) or down (red) by the sign of a change, none for no number.
std::optional<bool> trendUp(double change);
