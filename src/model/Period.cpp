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

void trace(const Seq<Candle> & bars, PeriodView & v) {
  v.closes.reserve(bars.size());
  for (const Candle & c : bars) v.closes.push_back(static_cast<float>(c.close));
  v.light = activity(bars);
}

// The bars of a queue from the first one at or after `key`.
Seq<Candle> from(const Seq<Candle> & bars, int64_t key) {
  const auto at = std::lower_bound(bars.begin(), bars.end(), key, [](const Candle & c, int64_t k) { return c.time < k; });
  return bars.drop(static_cast<size_t>(at - bars.begin()));
}

// Fills in the last price and the trace of a period from its bars. The last price is the quote's, the one the Price column shows (the store
// sets it from any bar newer than it, so it is never older than the bars), and the trace ends with it as one more point when it differs from
// the last bar's close, so the Price, the Change and the line agree. With no quote it is the last bar's close.
void finish(const Seq<Candle> & bars, const std::optional<Quote> & quote, PeriodView & v, bool withTrace) {
  v.last = quote && isDefined(quote->last) ? quote->last : bars.back().close;
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

std::optional<std::chrono::seconds> defaultKeep(Resolution r) {
  int64_t days = 0;
  for (const PeriodInfo & in : kInfo)
    for (size_t f = 0; f < in.feedCount; f++)
      if (in.feeds[f] == r) days = std::max(days, in.days);
  if (days == 0) return std::nullopt;
  return std::chrono::seconds(days * 86400);
}

std::vector<Resolution> readsOf(Period p) {
  const PeriodInfo & in = info(p);
  std::vector<Resolution> out(in.feeds.begin(), in.feeds.begin() + static_cast<std::ptrdiff_t>(in.feedCount));
  if (p == Period::Day) out.push_back(Resolution::Day);  // the change of the day is measured from a daily bar where there is no close before the session
  return out;
}

Covered periodCovered(const Snapshot & snapshot, const EdgeKey & edge, Period period) {
  const PeriodInfo & in = info(period);
  Covered out;
  if (period == Period::Day) {
    const auto days = snapshot.coverage(edge, Resolution::Day);
    for (size_t f = 0; f < in.feedCount; f++) {
      const auto c = snapshot.coverage(edge, in.feeds[f]);
      if (!c) continue;
      if (days && dayOf(in.feeds[f], c->last_key) < days->last_key) continue;  // older than the daily bars: a session gone by
      out.covered = true;
      out.fetched = c->fetched;
      return out;
    }
    return out;
  }
  for (size_t f = 0; f < in.feedCount; f++) {
    const Resolution r = in.feeds[f];
    const auto c = snapshot.coverage(edge, r);
    if (!c) continue;
    const int64_t end = dayOf(r, c->last_key), start = end - in.days + 1;
    if (dayOf(r, c->first_key) > start + in.days / 3) continue;  // does not reach back far enough
    out.covered = f < in.primary;
    out.fetched = c->fetched;
    return out;
  }
  return out;
}
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
  const std::optional<Quote> quote = snapshot.quote(edge);
  if (period == Period::Day) {
    const Seq<Candle> days = snapshot.queue(edge, Resolution::Day);
    for (size_t f = 0; f < in.feedCount; f++) {
      const Resolution r = in.feeds[f];
      const Seq<Candle> queue = snapshot.queue(edge, r);
      if (queue.empty()) continue;
      const int64_t session = dayOf(r, queue.back().time);
      if (!days.empty() && session < days.back().time) continue;  // older than the daily bars: a session gone by
      const Seq<Candle> bars = from(queue, keyOf(r, session));
      v.primary = true;
      finish(bars, quote, v, withTrace);
      if (auto previous = snapshot.previousClose(edge, session)) v.base = *previous;
      else
        for (size_t i = days.size(); i-- > 0;)
          if (days[i].time < session) {
            v.base = days[i].close;
            break;
          }
      return v;
    }
    // No intraday bars: the change since the daily bar before the day of the price, and no trace.
    if (!days.empty()) {
      const int64_t today = quote && quote->time != 0 ? dayNumberOfInstant(quote->time) : days.back().time;
      for (size_t i = days.size(); i-- > 0;)
        if (days[i].time < today) {
          v.base = days[i].close;
          v.last = quote && isDefined(quote->last) ? quote->last : days.back().close;
          break;
        }
    }
    return v;
  }
  for (size_t f = 0; f < in.feedCount; f++) {
    const Resolution r = in.feeds[f];
    const Seq<Candle> queue = snapshot.queue(edge, r);
    if (queue.empty()) continue;
    const int64_t end = dayOf(r, queue.back().time), start = end - in.days + 1;
    const Seq<Candle> bars = from(queue, keyOf(r, start));
    if (bars.empty() || dayOf(r, bars[0].time) > start + in.days / 3) continue;  // does not reach back far enough
    v.primary = f < in.primary;
    v.base = bars[0].close;
    finish(bars, quote, v, withTrace);
    return v;
  }
  // No feed reaches back far enough: the trace is what there is (a new instrument, a source with a short history), and there is no change, since it
  // would be over a shorter time than the heading says.
  for (size_t f = 0; f < in.feedCount; f++) {
    const Resolution r = in.feeds[f];
    const Seq<Candle> queue = snapshot.queue(edge, r);
    if (queue.empty()) continue;
    const Seq<Candle> bars = from(queue, keyOf(r, dayOf(r, queue.back().time) - in.days + 1));
    if (bars.empty()) continue;
    finish(bars, quote, v, withTrace);
    v.base = kUndefined;
    return v;
  }
  return v;
}
