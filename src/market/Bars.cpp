#include "market/Bars.h"

#include "model/Dates.h"

#include <algorithm>

namespace market {

namespace {

constexpr int64_t kDay = 86400;

constexpr bool isLeap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
constexpr unsigned daysIn(int year, unsigned month) {
  constexpr unsigned days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && isLeap(year) ? 29 : days[month - 1];
}

// The day after the same date `months` earlier (a day that the earlier month does not have becomes its last).
int64_t dayAfterMonthsBack(int64_t dayNumberOfLast, int months) {
  const Date d = dateOfDayNumber(dayNumberOfLast);
  int total = d.year * 12 + static_cast<int>(d.month) - 1 - months;
  const int year = total / 12;
  const unsigned month = static_cast<unsigned>(total % 12) + 1;
  return dayNumber(year, month, std::min(d.day, daysIn(year, month))) + 1;
}

}  // namespace

const char * name(Level level) {
  switch (level) {
    case Level::Day: return "day";
    case Level::Week: return "week";
    case Level::Month: return "month";
    case Level::Year: return "year";
    case Level::Years3: return "3years";
    case Level::Years10: return "10years";
  }
  return "";
}

std::optional<Level> parseLevel(const std::string & text) {
  for (Level level : kLevels)
    if (text == name(level)) return level;
  return std::nullopt;
}

Resolution barOf(Level level) {
  switch (level) {
    case Level::Day: return Resolution::Min5;
    case Level::Week: return Resolution::Hour1;
    case Level::Month: return Resolution::Hour4;
    case Level::Year: return Resolution::Day;
    case Level::Years3:
    case Level::Years10: return Resolution::Week;
  }
  return Resolution::Day;
}

int64_t barKey(Resolution bar, Timestamp utcSeconds) {
  switch (bar) {
    case Resolution::Min5:
    case Resolution::Hour1:
    case Resolution::Hour4: return slotOfInstant(bar, utcSeconds);
    case Resolution::Day: return dayNumberOfInstant(utcSeconds);
    case Resolution::Week: return weekStart(dayNumberOfInstant(utcSeconds));
    default: return 0;  // not a size that is stored
  }
}

Timestamp barStart(Resolution bar, int64_t key) {
  switch (bar) {
    case Resolution::Min5:
    case Resolution::Hour1:
    case Resolution::Hour4: return instantOfSlot(bar, key);
    case Resolution::Day:
    case Resolution::Week: return (key - kUnixEpochDayNumber) * kDay;
    default: return 0;
  }
}

int64_t windowFirstKey(Level level, Timestamp lastInstant) {
  const Resolution bar = barOf(level);
  const int64_t last = barKey(bar, lastInstant);
  const int64_t lastDay = dayNumberOfInstant(lastInstant);
  switch (level) {
    case Level::Day: return last - 288 + 1;
    case Level::Week: return last - 7 * 24 + 1;
    case Level::Month: return barKey(bar, barStart(Resolution::Day, dayAfterMonthsBack(lastDay, 1)));
    case Level::Year: return dayAfterMonthsBack(lastDay, 12);
    case Level::Years3:
    case Level::Years10: {
      const int64_t from = dayAfterMonthsBack(lastDay, level == Level::Years3 ? 36 : 120);
      const int64_t monday = weekStart(from);
      return monday == from ? from : monday + 7;
    }
  }
  return last;
}

Timestamp windowStartTime(Level level, Timestamp lastInstant) { return barStart(barOf(level), windowFirstKey(level, lastInstant)); }

std::vector<Bar> aggregate(Resolution bar, std::vector<MinuteBar> minutes) {
  std::stable_sort(minutes.begin(), minutes.end(), [](const MinuteBar & a, const MinuteBar & b) { return a.time < b.time; });
  std::vector<Bar> bars;
  for (size_t i = 0; i < minutes.size(); i++) {
    // The later of two minutes with the same time is the one that stands.
    if (i + 1 < minutes.size() && minutes[i + 1].time == minutes[i].time) continue;
    const MinuteBar & m = minutes[i];
    const int64_t key = barKey(bar, m.time);
    if (bars.empty() || bars.back().key != key) {
      bars.push_back({key, m.open, m.high, m.low, m.close});
    } else {
      Bar & b = bars.back();
      b.high = std::max(b.high, m.high);
      b.low = std::min(b.low, m.low);
      b.close = m.close;
    }
  }
  return bars;
}

std::vector<Bar> joinBars(std::vector<Bar> earlier, std::vector<Bar> later) {
  for (Bar & bar : later) {
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

std::vector<Bar> cutToWindow(Level level, std::vector<Bar> bars, Timestamp lastInstant) {
  const int64_t first = windowFirstKey(level, lastInstant), last = barKey(barOf(level), lastInstant);
  std::erase_if(bars, [&](const Bar & b) { return b.key < first || b.key > last; });
  return bars;
}

}  // namespace market
