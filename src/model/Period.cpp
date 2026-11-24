#include "model/Period.h"

#include "model/Dates.h"

#include <algorithm>
#include <cstddef>
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
  v.light = activity(bars);
}

// Fills in the last price and the trace of a period from its series `bars`. The series is loaded now and then, the daily bar is
// replaced every few minutes: where the daily bar is as new as the series (its date `since` or later) its close is the latest
// price, the one the Price column shows, and the trace ends with it so the line ends where the change does.
void finish(const Seq<Candle> & bars, const std::optional<Candle> & daily, int64_t since, PeriodView & v, bool withTrace) {
  v.last = bars.back().close;
  if (daily && daily->time >= since && isDefined(daily->close)) v.last = daily->close;
  if (!withTrace) return;
  trace(bars, v);
  if (v.last != bars.back().close) {
    v.closes.push_back(static_cast<float>(v.last));
    if (!v.light.empty()) v.light.push_back(v.light.back());  // the latest price is the last bar's, as it goes on
  }
}

}  // namespace

std::vector<float> activity(const Seq<Candle> & bars) {
  std::vector<double> traded;
  for (const Candle & c : bars)
    if (isDefined(c.volume) && c.volume > 0) traded.push_back(c.volume);
  if (traded.empty()) return {};
  std::nth_element(traded.begin(), traded.begin() + static_cast<std::ptrdiff_t>(traded.size() / 2), traded.end());
  const double usual = traded[traded.size() / 2];
  std::vector<float> light;
  light.reserve(bars.size());
  for (const Candle & c : bars) {
    const double v = isDefined(c.volume) && c.volume > 0 ? c.volume : 0.0;
    light.push_back(static_cast<float>(std::min(1.0, v / usual / 4.0)));
  }
  return light;
}

const PeriodInfo & info(Period p) { return kInfo[static_cast<size_t>(p)]; }
Period nextPeriod(Period p) { return static_cast<Period>((static_cast<size_t>(p) + 1) % kPeriodCount); }
Period previousPeriod(Period p) { return static_cast<Period>((static_cast<size_t>(p) + kPeriodCount - 1) % kPeriodCount); }

std::optional<bool> trendUp(double change) {
  if (!isDefined(change)) return std::nullopt;
  return change >= 0;
}

bool isRate(const Snapshot & snapshot, const EdgeKey & edge) {
  return snapshot.kind(edge.quote) == InstrumentKind::Unit && snapshot.symbol(edge.quote) == kPercentUnit;
}

PeriodView periodView(const Snapshot & snapshot, const EdgeKey & edge, Period period, bool withTrace) {
  const PeriodInfo & in = info(period);
  PeriodView v;
  v.rate = isRate(snapshot, edge);
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
      finish(bars, days.empty() ? std::nullopt : std::optional<Candle>(days.back()), session, v, withTrace);
      if (auto previous = snapshot.previousClose(edge, session)) v.base = *previous;
      else
        for (size_t i = days.size(); i-- > 0;)
          if (days[i].time < session) {
            v.base = days[i].close;
            break;
          }
      return v;
    }
    // No intraday bars: the change since the previous daily bar, and no trace.
    if (days.size() == 2) {
      v.last = days[1].close;
      v.base = days[0].close;
    }
    return v;
  }
  // The latest daily bar, read once (and not for the periods that are daily bars themselves).
  std::optional<Candle> daily;
  if (period == Period::Week || period == Period::Year) {
    const CandleSeries newestDay = snapshot.lastCandles(edge, Resolution::Day, 1);
    if (!newestDay.candles.empty()) daily = newestDay.candles.back();
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
    // A daily stand-in is the daily bars already: nothing newer to take.
    finish(bars, r == Resolution::Day ? std::nullopt : daily, end, v, withTrace);
    return v;
  }
  return v;
}
