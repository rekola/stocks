#include "market/Bars.h"

#include "model/Dates.h"

#include <algorithm>

namespace market {

namespace {

constexpr int64_t kDay = 86400;
constexpr int64_t kFiveMinuteBarsInADay = kDay / 300;
constexpr int64_t kHourlyBarsIn31Days = 31 * 24;
constexpr int kDailyYears = 10;

constexpr bool isLeap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }

}  // namespace

bool isLevel(Resolution r) { return r == Resolution::Min5 || r == Resolution::Hour1 || r == Resolution::Day; }

std::optional<Resolution> parseLevel(const std::string & text) {
  for (Resolution level : kLevels)
    if (text == name(level)) return level;
  return std::nullopt;
}

int64_t barKey(Resolution level, Timestamp utcSeconds) {
  switch (level) {
    case Resolution::Min5:
    case Resolution::Hour1: return slotOfInstant(level, utcSeconds);
    case Resolution::Day: return dayNumberOfInstant(utcSeconds);
    default: return 0;  // not a stored level
  }
}

Timestamp barStart(Resolution level, int64_t key) {
  switch (level) {
    case Resolution::Min5:
    case Resolution::Hour1: return instantOfSlot(level, key);
    case Resolution::Day: return (key - kUnixEpochDayNumber) * kDay;
    default: return 0;
  }
}

int64_t windowFirstKey(Resolution level, int64_t lastKey) {
  switch (level) {
    case Resolution::Min5: return lastKey - kFiveMinuteBarsInADay + 1;
    case Resolution::Hour1: return lastKey - kHourlyBarsIn31Days + 1;
    case Resolution::Day: {
      // The same date ten years back (29 February becomes the 28th), and the window starts the day after it.
      const Date d = dateOfDayNumber(lastKey);
      const int year = d.year - kDailyYears;
      const unsigned day = d.month == 2 && d.day == 29 && !isLeap(year) ? 28 : d.day;
      return dayNumber(year, d.month, day) + 1;
    }
    default: return lastKey;
  }
}

Timestamp windowStartTime(Resolution level, Timestamp lastInstant) {
  return barStart(level, windowFirstKey(level, barKey(level, lastInstant)));
}

std::vector<Bar> aggregate(Resolution level, std::vector<MinuteBar> minutes) {
  std::stable_sort(minutes.begin(), minutes.end(), [](const MinuteBar & a, const MinuteBar & b) { return a.time < b.time; });
  std::vector<Bar> bars;
  for (size_t i = 0; i < minutes.size(); i++) {
    // The later of two minutes with the same time is the one that stands.
    if (i + 1 < minutes.size() && minutes[i + 1].time == minutes[i].time) continue;
    const MinuteBar & m = minutes[i];
    const int64_t key = barKey(level, m.time);
    if (bars.empty() || bars.back().key != key) {
      bars.push_back({key, m.open, m.high, m.low, m.close});
    } else {
      Bar & bar = bars.back();
      bar.high = std::max(bar.high, m.high);
      bar.low = std::min(bar.low, m.low);
      bar.close = m.close;
    }
  }
  return bars;
}

std::vector<Bar> joinBars(std::vector<Bar> earlier, std::vector<Bar> later) {
  for (Bar & bar : later) {
    // Bars are ascending, so a key that is in both is the end of `earlier` and the start of `later`; look back for it all the same
    // when the runs overlap by more than the one bar.
    auto it = std::lower_bound(earlier.begin(), earlier.end(), bar.key, [](const Bar & b, int64_t key) { return b.key < key; });
    if (it != earlier.end() && it->key == bar.key) {
      it->high = std::max(it->high, bar.high);
      it->low = std::min(it->low, bar.low);
      it->close = bar.close;
    } else {
      earlier.insert(it, bar);
    }
  }
  return earlier;
}

std::vector<Bar> cutToWindow(Resolution level, std::vector<Bar> bars, int64_t lastKey) {
  const int64_t first = windowFirstKey(level, lastKey);
  std::erase_if(bars, [&](const Bar & b) { return b.key < first || b.key > lastKey; });
  return bars;
}

}  // namespace market
