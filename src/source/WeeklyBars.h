#pragma once

#include "model/Dates.h"
#include "model/Types.h"

#include <algorithm>
#include <vector>

// The weekly bars of daily ones, for the sources whose provider has none and whose 3 and 10 years read weekly bars (the weeks are Monday's to Sunday's, keyed
// by the day number of the Monday): a week's open is its first bar's, its close its last bar's, its high and low the extremes, its volume the sum where any bar
// has one; a bar with only a close (one value a day) stands for all four. The first week is left out unless the bars have its Monday: a week that is cut at the start
// would be a bar of fewer days than the others, and would replace a whole one that another source has. `days` is oldest first, by day number.
inline std::vector<Candle> weeksOfDays(const std::vector<Candle> & days) {
  std::vector<Candle> out;
  for (const Candle & d : days) {
    const int64_t start = weekStart(d.time);
    if (out.empty() || out.back().time != start) {
      if (out.empty() && d.time != start) {  // the first week lacks its Monday: it is not whole
        out.push_back(Candle{start, kUndefined, kUndefined, kUndefined, kUndefined, kUndefined});
        continue;
      }
      out.push_back(Candle{start, isDefined(d.open) ? d.open : d.close, isDefined(d.high) ? d.high : d.close, isDefined(d.low) ? d.low : d.close, d.close, d.volume});
      continue;
    }
    Candle & w = out.back();
    if (!isDefined(w.close)) continue;  // inside the week that is left out
    w.high = std::max(w.high, isDefined(d.high) ? d.high : d.close);
    w.low = std::min(w.low, isDefined(d.low) ? d.low : d.close);
    w.close = d.close;
    if (isDefined(d.volume)) w.volume = isDefined(w.volume) ? w.volume + d.volume : d.volume;
  }
  if (!out.empty() && !isDefined(out.front().close)) out.erase(out.begin());
  return out;
}
