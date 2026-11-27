#pragma once

#include "model/Types.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

// Dates as day numbers: the absolute number of the day in the proleptic Gregorian calendar, 1 being 1 January of the year
// 1 (Date::Calc's Date_to_Days, Python's date.toordinal()). 1998-05-01 is 729510. A date is not an instant: trading does
// not start at midnight, and a daily bar belongs to its date, whatever the time zone. Pure arithmetic, no time zone data.

struct Date {
  int year = 1;
  unsigned month = 1, day = 1;
  friend bool operator==(const Date &, const Date &) = default;
};

// The day number of 1970-01-01: a Unix day number plus this is a day number.
constexpr int64_t kUnixEpochDayNumber = 719163;

// Days since 1970-01-01 of a civil date, and back (Howard Hinnant's algorithms, in the public domain).
constexpr int64_t daysSinceUnixEpoch(int64_t year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + static_cast<int64_t>(dayOfEra) - 719468;
}

constexpr Date civilFromUnixDays(int64_t days) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
  const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
  const int64_t y = static_cast<int64_t>(yearOfEra) + era * 400;
  const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const unsigned mp = (5 * dayOfYear + 2) / 153;
  const unsigned d = dayOfYear - (153 * mp + 2) / 5 + 1;
  const unsigned m = mp < 10 ? mp + 3 : mp - 9;
  return {static_cast<int>(y + (m <= 2)), m, d};
}

constexpr int64_t dayNumber(int year, unsigned month, unsigned day) {
  return daysSinceUnixEpoch(year, month, day) + kUnixEpochDayNumber;
}
constexpr Date dateOfDayNumber(int64_t number) { return civilFromUnixDays(number - kUnixEpochDayNumber); }

// The date of an instant, in the time zone `utcOffsetSeconds` east of UTC (0 for UTC): what a provider's instant says
// about the trading date, given the exchange's offset.
constexpr int64_t dayNumberOfInstant(Timestamp utcSeconds, int utcOffsetSeconds = 0) {
  const int64_t local = utcSeconds + utcOffsetSeconds;
  const int64_t days = local >= 0 ? local / 86400 : -((-local + 86399) / 86400);  // floor
  return days + kUnixEpochDayNumber;
}

// The day number of a date written "2026-11-20" (exactly that: four digits, two, two), or none.
inline std::optional<int64_t> dayNumberOfIsoDate(const std::string & text) {
  int year = 0;
  unsigned month = 0, day = 0;
  char tail = 0;
  if (text.size() != 10 || std::sscanf(text.c_str(), "%4d-%2u-%2u%c", &year, &month, &day, &tail) != 3) return std::nullopt;
  if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31) return std::nullopt;
  return dayNumber(year, month, day);
}

// The instant, in UTC seconds, of a time written "2026-11-24T14:19:44Z" or "2026-11-25T14:22:01.418143+00:00" (the seconds may have a fraction, and
// the zone is Z or an offset of hours and minutes), or none.
inline std::optional<Timestamp> instantOfIsoTimestamp(const std::string & text) {
  const auto day = text.size() >= 19 ? dayNumberOfIsoDate(text.substr(0, 10)) : std::nullopt;
  unsigned hour = 0, minute = 0, second = 0;
  char tail = 0;
  if (!day || (text[10] != 'T' && text[10] != ' ') || std::sscanf(text.c_str() + 11, "%2u:%2u:%2u%c", &hour, &minute, &second, &tail) != 4) return std::nullopt;
  if (hour > 23 || minute > 59 || second > 60) return std::nullopt;
  size_t at = 19;
  if (at < text.size() && text[at] == '.')
    while (++at < text.size() && text[at] >= '0' && text[at] <= '9') {}
  int offset = 0;  // seconds east of UTC
  if (at < text.size() && text[at] == 'Z' && at + 1 == text.size()) {
  } else if (at + 6 == text.size() && (text[at] == '+' || text[at] == '-')) {
    unsigned h = 0, m = 0;
    char end = 0;
    if (std::sscanf(text.c_str() + at + 1, "%2u:%2u%c", &h, &m, &end) != 2 || h > 23 || m > 59) return std::nullopt;
    offset = static_cast<int>(h * 3600 + m * 60) * (text[at] == '-' ? -1 : 1);
  } else {
    return std::nullopt;
  }
  return (*day - kUnixEpochDayNumber) * 86400 + hour * 3600 + minute * 60 + second - offset;
}

// The weekday of a day number, Monday 0 to Sunday 6 (1 January of the year 1 was a Monday).
constexpr int weekday(int64_t number) { return static_cast<int>(((number - 1) % 7 + 7) % 7); }

// The first day of the week (a Monday, as in ISO 8601), month and quarter that a day number is in: the key of the bars
// of those resolutions.
constexpr int64_t weekStart(int64_t number) { return number - weekday(number); }
constexpr int64_t monthStart(int64_t number) {
  const Date d = dateOfDayNumber(number);
  return dayNumber(d.year, d.month, 1);
}
constexpr int64_t quarterStart(int64_t number) {
  const Date d = dateOfDayNumber(number);
  return dayNumber(d.year, (d.month - 1) / 3 * 3 + 1, 1);
}

// The trading date a provider's daily bar is for, from the instant it stamps the bar with. Providers stamp a day's bar at
// the start of that trading day in the exchange's time zone: local midnight (futures, currencies) or the open. The offset
// from UTC they give is today's, and on the day itself it can be an hour different (daylight saving), so the date is the
// one of the *nearest* local midnight, which absorbs that hour: reading the instant in today's offset would put summer
// bars on the day before. Wrong for a stamp after local noon.
constexpr int64_t tradingDateOfDailyStamp(Timestamp utcSeconds, int utcOffsetSeconds) {
  return dayNumberOfInstant(utcSeconds, utcOffsetSeconds + 12 * 3600);
}

// Slot numbers of the intraday resolutions: the key of a bar is the number of its slot, the UTC Unix seconds of its start
// divided by the length of the resolution, rounded down. The hour number is the UTC seconds divided by 3600, the
// 5-minute slot number the seconds divided by 300, and so on, as in the legacy platform. (Day numbers are counted from
// the year 1, see above; the origin of these is the Unix epoch.) In UTC, so no time zone and no daylight saving.
constexpr int64_t slotOfInstant(Resolution r, Timestamp utcSeconds) {
  const int64_t length = seconds(r);  // intraday only
  return utcSeconds >= 0 ? utcSeconds / length : -((-utcSeconds + length - 1) / length);
}
constexpr Timestamp instantOfSlot(Resolution r, int64_t slot) { return slot * seconds(r); }

// Whether a bar that starts at `utcSeconds` starts on a slot boundary. One that does not cannot be keyed by its slot
// without losing its offset: a provider's hourly bars on US exchanges can start at :30.
constexpr bool onSlotBoundary(Resolution r, Timestamp utcSeconds) { return utcSeconds % seconds(r) == 0; }
