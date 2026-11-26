#pragma once

#include "model/Store.h"

#include <array>
#include <chrono>
#include <optional>
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
//   3 years  the last 1095 days: weekly bars.
//   10 years the last 3650 days: weekly bars.
//
// A rate (a row quoted in per cent, `isRate`) is not a price: its change is the difference between the last value and the base, in
// basis points (a base of 0, or a negative one, is as good as any), where a price's is the percentage change. The base, the last
// value and the trace are the same for both.
//
// The last price of a period is the Price column's: the edge's quote, which the store keeps at least as new as any bar it was given (a bar
// newer than the quote sets it), and the trace ends with it as one more point when it differs from the last bar's close, so the Price, the
// Change and the line agree. The base is never moved. The bars are the edge's queues (Summary.h), not its data, and a queue that is not in
// memory has none: the row shows its price and no trace until it is read. A fund's NAV is a bar of one value and works the same way.
//
// The change of a period is shown only from bars that cover it (the trace is whatever bars there are): the first bar must be within a third of the period of its start (so the 60
// daily bars the made-up data has are a quarter, one daily bar is not a week). Else the row has no number for the period,
// never a wrong one.
enum class Period : uint8_t { Day, Week, Month, Quarter, Year, Year3, Year10 };

inline constexpr size_t kPeriodCount = 7;
static_assert(kPeriodCount == kChangeCount, "an edge keeps a change for each period (Summary.h)");
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

// How long a window of bars a queue of this resolution keeps, measured back from its last bar, where nothing says otherwise (a provider
// says: HistoryProvider::keep): the longest period that is listed above as fed by the resolution; none for a resolution no period reads, which
// has no queue.
std::optional<std::chrono::seconds> defaultKeep(Resolution);
// The queues a period reads, the finest first: the series that feed it, and for the day the daily bars its change is measured from.
std::vector<Resolution> readsOf(Period);
// Whether the queues the edge has, in memory or only on the disk, cover the period (what `PeriodView::primary` says of one that is in
// memory), and when the queue that does was fetched; for the loader, which asks before the network.
struct Covered {
  bool covered = false;
  TimestampMs fetched = 0;
};
Covered periodCovered(const Snapshot &, const EdgeKey &, Period);

// What a row shows for a period.
struct PeriodView {
  bool rate = false;              // the row is a rate: its change is a difference in basis points
  bool primary = false;           // the bars are the period's own (a stand-in's, or none, is false)
  double base = kUndefined;       // the price the change is measured from
  double last = kUndefined;       // the last price of the period
  std::vector<float> closes;      // the trace: the period's closes, oldest first (only when asked for)
  std::vector<Timestamp> times;   // when each close is, as UTC seconds (a daily or longer bar: midnight of its date), one for each of `closes`
  double span = 1.0;              // how much of the period the trace covers, 0 to 1: less than 1 only where the bars do not reach back far enough (no change then)
  bool dated = false;             // the bars are days or longer, so the times say a date and no time of day
  std::vector<float> light;       // how bright the trace is at each close, 0 to 1: the volume of its bar against the period's usual (see activity())
  std::vector<double> volumes;    // the volume of each close's bar (undefined where there is none), one for each of `closes`; empty where no bar has one
  bool hasChange() const { return isDefined(base) && isDefined(last) && (rate || base != 0); }
  // For a rate in basis points, else in percent; undefined where there is no number.
  double change() const {
    if (!hasChange()) return kUndefined;
    return rate ? (last - base) * 100.0 : (last / base - 1.0) * 100.0;
  }
};
// Whether the edge is a rate: its quote is the per cent unit (a Unit instrument with that symbol). The one rule for it.
bool isRate(const Snapshot &, const EdgeKey &);
// The brightness of a trace at each bar: 0 for no volume (a medium brightness), 0.25 for the period's usual volume (its median
// bar), and 1 at four times that or more, so a bar with abnormal volume is the one lit past white. A volume that is not
// defined counts as none. Empty where no bar has a volume (the trace is then medium throughout).
std::vector<float> activity(const Seq<Candle> & bars);
PeriodView periodView(const Snapshot &, const EdgeKey &, Period, bool withTrace);

// The colour rule, in one place: up (green) or down (red) by the sign of a change, none for no number.
std::optional<bool> trendUp(double change);
