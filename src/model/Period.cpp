#include "model/Period.h"

#include "model/Dates.h"

#include <limits>

namespace {

using R = Resolution;
const PeriodInfo kInfo[kPeriodCount] = {
    {"day", "1d", "1 day", 1, {R::Min5, R::Min1, R::Min2, R::Min15, R::Min30}, 5, 5},
    {"week", "1w", "1 week", 7, {R::Hour1, R::Day}, 2, 1},
    {"month", "1m", "1 month", 30, {R::Day}, 1, 1},
    {"quarter", "3m", "3 months", 90, {R::Day}, 1, 1},
    {"year", "1y", "1 year", 365, {R::Week, R::Day}, 2, 2},
};

int64_t dayOf(Resolution r, int64_t key) { return isDateResolution(r) ? key : dayNumberOfInstant(instantOfSlot(r, key)); }
// The first key of a bar that is on `day` or later.
int64_t keyOf(Resolution r, int64_t day) { return isDateResolution(r) ? day : slotOfInstant(r, (day - kUnixEpochDayNumber) * 86400); }
constexpr int64_t kEnd = std::numeric_limits<int64_t>::max();

void trace(const Seq<Candle> & bars, PeriodView & v) {
  v.closes.reserve(bars.size());
  for (const Candle & c : bars) v.closes.push_back(static_cast<float>(c.close));
}

}  // namespace

const PeriodInfo & info(Period p) { return kInfo[static_cast<size_t>(p)]; }
Period nextPeriod(Period p) { return static_cast<Period>((static_cast<size_t>(p) + 1) % kPeriodCount); }
Period previousPeriod(Period p) { return static_cast<Period>((static_cast<size_t>(p) + kPeriodCount - 1) % kPeriodCount); }

std::optional<bool> trendUp(double change) {
  if (!isDefined(change)) return std::nullopt;
  return change >= 0;
}

PeriodView periodView(const Snapshot & snapshot, const EdgeKey & edge, Period period, bool withTrace) {
  const PeriodInfo & in = info(period);
  PeriodView v;
  if (period == Period::Day) {
    const CandleSeries daily = snapshot.lastCandles(edge, Resolution::Day, 2);
    const Seq<Candle> & days = daily.candles;
    for (size_t f = 0; f < in.feedCount; f++) {
      const Resolution r = in.feeds[f];
      const CandleSeries newest = snapshot.lastCandles(edge, r, 1);
      if (newest.candles.empty()) continue;
      const int64_t session = dayOf(r, newest.candles.back().time);
      if (!days.empty() && session < days.back().time) continue;  // older than the daily bars: a session gone by
      const Seq<Candle> bars = snapshot.candles(edge, r, {keyOf(r, session), kEnd}).candles;
      v.primary = true;
      v.last = bars.back().close;
      if (auto previous = snapshot.previousClose(edge, session)) v.base = *previous;
      else
        for (size_t i = days.size(); i-- > 0;)
          if (days[i].time < session) {
            v.base = days[i].close;
            break;
          }
      if (withTrace) trace(bars, v);
      return v;
    }
    // No intraday bars: the change since the previous daily bar, and no trace.
    if (days.size() == 2) {
      v.last = days[1].close;
      v.base = days[0].close;
    }
    return v;
  }
  for (size_t f = 0; f < in.feedCount; f++) {
    const Resolution r = in.feeds[f];
    const CandleSeries newest = snapshot.lastCandles(edge, r, 1);
    if (newest.candles.empty()) continue;
    const int64_t end = dayOf(r, newest.candles.back().time), start = end - in.days + 1;
    const Seq<Candle> bars = snapshot.candles(edge, r, {keyOf(r, start), kEnd}).candles;
    if (bars.empty() || dayOf(r, bars[0].time) > start + in.days / 3) continue;  // does not reach back far enough
    v.primary = f < in.primary;
    v.base = bars[0].close;
    v.last = bars.back().close;
    if (withTrace) trace(bars, v);
    return v;
  }
  return v;
}
