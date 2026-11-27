#include "market/Bars.h"

#include "model/Dates.h"
#include "model/Period.h"
#include "model/SummaryRecords.h"

#include <algorithm>

namespace market {

std::optional<Resolution> parseResolution(const std::string & text) {
  for (Resolution r : kResolutions)
    if (text == name(r)) return r;
  return std::nullopt;
}

int64_t barKey(Resolution r, Timestamp utcSeconds) {
  switch (r) {
    case Resolution::Min5:
    case Resolution::Hour1: return slotOfInstant(r, utcSeconds);
    case Resolution::Day: return dayNumberOfInstant(utcSeconds);
    case Resolution::Week: return weekStart(dayNumberOfInstant(utcSeconds));
    default: return 0;  // not a resolution that is made
  }
}

Timestamp barStart(Resolution r, int64_t key) {
  switch (r) {
    case Resolution::Min5:
    case Resolution::Hour1: return instantOfSlot(r, key);
    case Resolution::Day:
    case Resolution::Week: return (key - kUnixEpochDayNumber) * 86400;
    default: return 0;
  }
}

int64_t keepSeconds(Resolution r) {
  const auto keep = defaultKeep(r);
  return keep ? keep->count() : 0;
}

Timestamp windowStartTime(Resolution r, Timestamp lastInstant) {
  // The bar that holds the first instant of the window is in it (the model counts the window from the instants of the bars).
  return barStart(r, barKey(r, lastInstant - keepSeconds(r)));
}

std::vector<Candle> aggregate(Resolution r, std::vector<MinuteBar> minutes) {
  std::stable_sort(minutes.begin(), minutes.end(), [](const MinuteBar & a, const MinuteBar & b) { return a.time < b.time; });
  std::vector<Candle> bars;
  for (size_t i = 0; i < minutes.size(); i++) {
    // The later of two minutes with the same time is the one that stands.
    if (i + 1 < minutes.size() && minutes[i + 1].time == minutes[i].time) continue;
    const MinuteBar & m = minutes[i];
    const int64_t key = barKey(r, m.time);
    if (bars.empty() || bars.back().time != key) {
      bars.push_back({key, m.open, m.high, m.low, m.close, kUndefined});
    } else {
      Candle & b = bars.back();
      b.high = std::max(b.high, m.high);
      b.low = std::min(b.low, m.low);
      b.close = m.close;
    }
  }
  return bars;
}

std::vector<Candle> joinBars(std::vector<Candle> earlier, std::vector<Candle> later) {
  for (Candle & bar : later) {
    auto it = std::lower_bound(earlier.begin(), earlier.end(), bar.time, [](const Candle & b, int64_t key) { return b.time < key; });
    if (it != earlier.end() && it->time == bar.time) {
      it->high = std::max(it->high, bar.high);
      it->low = std::min(it->low, bar.low);
      it->close = bar.close;
    } else {
      earlier.insert(it, bar);
    }
  }
  return earlier;
}

std::vector<Candle> cutToWindow(Resolution r, std::vector<Candle> bars, Timestamp lastInstant) {
  // Bars after the last instant are not the window's; then the model's own cut, from the last bar back.
  const int64_t last = barKey(r, lastInstant);
  std::erase_if(bars, [&](const Candle & b) { return b.time > last; });
  summary::trim(bars, r, keepSeconds(r));
  return bars;
}

}  // namespace market
