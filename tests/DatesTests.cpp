#include "TestFramework.h"

#include "model/Dates.h"

TEST(a_day_number_counts_days_from_the_first_of_january_of_the_year_one) {
  // The convention of Date::Calc's Date_to_Days and Python's date.toordinal().
  CHECK(dayNumber(1, 1, 1) == 1 && dayNumber(1, 12, 31) == 365 && dayNumber(2, 1, 1) == 366);
  CHECK(dayNumber(1998, 5, 1) == 729510);
  CHECK(dayNumber(1970, 1, 1) == 719163 && kUnixEpochDayNumber == 719163);
  CHECK(dayNumber(1987, 1, 1) == 725372 && dayNumber(2019, 7, 29) == 737269);
  CHECK(dayNumber(2026, 11, 20) == 739940 && dayNumber(2026, 11, 21) == 739941);
  CHECK(dayNumber(2000, 3, 1) - dayNumber(2000, 2, 28) == 2);  // 2000 was a leap year
  CHECK(dayNumber(1900, 3, 1) - dayNumber(1900, 2, 28) == 1);  // and 1900 was not
}

TEST(a_day_number_converts_back_to_its_date) {
  CHECK((dateOfDayNumber(1) == Date{1, 1, 1}) && (dateOfDayNumber(365) == Date{1, 12, 31}) && (dateOfDayNumber(366) == Date{2, 1, 1}));
  CHECK((dateOfDayNumber(729510) == Date{1998, 5, 1}) && (dateOfDayNumber(739940) == Date{2026, 11, 20}));
  // Every day from 1850 to 2250 comes back as itself, and the days follow each other.
  int64_t previous = dayNumber(1850, 1, 1) - 1;
  for (int64_t n = dayNumber(1850, 1, 1); n <= dayNumber(2250, 12, 31); n++) {
    Date d = dateOfDayNumber(n);
    if (dayNumber(d.year, d.month, d.day) != n || n != previous + 1) {
      CHECK(false);
      break;
    }
    previous = n;
  }
  CHECK(dateOfDayNumber(dayNumber(2024, 2, 29)) == (Date{2024, 2, 29}));
}

TEST(the_date_of_an_instant_depends_on_the_time_zone_it_is_read_in) {
  // 2026-11-20 22:30:00 UTC.
  const Timestamp t = 1795213800;
  CHECK(dayNumberOfInstant(t) == dayNumber(2026, 11, 20));
  CHECK(dayNumberOfInstant(t, 2 * 3600) == dayNumber(2026, 11, 21));   // Helsinki in winter: already the 21st
  CHECK(dayNumberOfInstant(t, -5 * 3600) == dayNumber(2026, 11, 20));  // New York: still the 20th
  CHECK(dayNumberOfInstant(0) == dayNumber(1970, 1, 1) && dayNumberOfInstant(86399) == dayNumber(1970, 1, 1));
  CHECK(dayNumberOfInstant(86400) == dayNumber(1970, 1, 2));
  CHECK(dayNumberOfInstant(-1) == dayNumber(1969, 12, 31));  // before the epoch: the floor, not the truncation
  CHECK(dayNumberOfInstant(-86400) == dayNumber(1969, 12, 31) && dayNumberOfInstant(-86401) == dayNumber(1969, 12, 30));
}

TEST(weekdays_weeks_months_and_quarters_follow_from_the_day_number_alone) {
  CHECK(weekday(dayNumber(1, 1, 1)) == 0);                      // a Monday
  CHECK(weekday(dayNumber(2026, 11, 16)) == 0);                 // a Monday
  CHECK(weekday(dayNumber(2026, 11, 20)) == 4);                 // a Friday
  CHECK(weekday(dayNumber(2026, 11, 22)) == 6);                 // a Sunday
  CHECK(weekday(dayNumber(1970, 1, 1)) == 3);                   // a Thursday
  CHECK(weekStart(dayNumber(2026, 11, 20)) == dayNumber(2026, 11, 16));
  CHECK(weekStart(dayNumber(2026, 11, 16)) == dayNumber(2026, 11, 16) && weekStart(dayNumber(2026, 11, 22)) == dayNumber(2026, 11, 16));
  CHECK(weekStart(dayNumber(2027, 1, 1)) == dayNumber(2026, 12, 28));  // a week across the new year
  CHECK(monthStart(dayNumber(2026, 11, 20)) == dayNumber(2026, 11, 1) && monthStart(dayNumber(2024, 2, 29)) == dayNumber(2024, 2, 1));
  CHECK(quarterStart(dayNumber(2026, 11, 20)) == dayNumber(2026, 10, 1) && quarterStart(dayNumber(2026, 3, 31)) == dayNumber(2026, 1, 1));
  CHECK(quarterStart(dayNumber(2026, 4, 1)) == dayNumber(2026, 4, 1) && quarterStart(dayNumber(2026, 12, 31)) == dayNumber(2026, 10, 1));
}

TEST(a_daily_bar_stamped_at_the_start_of_the_trading_day_is_dated_by_the_nearest_local_midnight) {
  auto at = [](int y, unsigned m, unsigned d, int hour) { return daysSinceUnixEpoch(y, m, d) * 86400 + hour * 3600; };
  // Helsinki: bars at the open, 07:00 or 08:00 UTC; the offset given is today's (winter, +2 h).
  CHECK(tradingDateOfDailyStamp(at(2026, 7, 15, 7), 7200) == dayNumber(2026, 7, 15));
  CHECK(tradingDateOfDailyStamp(at(2026, 1, 15, 8), 7200) == dayNumber(2026, 1, 15));
  // New York futures: bars at local midnight, 04:00 UTC in summer and 05:00 in winter; the offset given is winter's, -5 h.
  CHECK(tradingDateOfDailyStamp(at(2026, 7, 15, 4), -18000) == dayNumber(2026, 7, 15));
  CHECK(tradingDateOfDailyStamp(at(2026, 1, 15, 5), -18000) == dayNumber(2026, 1, 15));
  CHECK(dayNumberOfInstant(at(2026, 7, 15, 4), -18000) == dayNumber(2026, 7, 14));  // read in today's offset it is the day before
  // London currencies: bars at local midnight, 23:00 UTC of the day before in summer and 00:00 in winter; offset 0.
  CHECK(tradingDateOfDailyStamp(at(2026, 7, 14, 23), 0) == dayNumber(2026, 7, 15));
  CHECK(tradingDateOfDailyStamp(at(2026, 1, 15, 0), 0) == dayNumber(2026, 1, 15));
  // A date marker at 00:00 UTC (dividends) keeps its date in every one of those zones.
  for (int offset : {7200, -18000, 0, 32400}) CHECK(tradingDateOfDailyStamp(at(2025, 2, 3, 0), offset) == dayNumber(2025, 2, 3));
}

TEST(an_intraday_bar_is_keyed_by_its_slot_number_in_utc) {
  // 2026-11-20 16:00:00 UTC.
  const Timestamp t = 1795190400;
  CHECK(slotOfInstant(Resolution::Hour1, t) == t / 3600 && slotOfInstant(Resolution::Hour1, t) == 498664);  // the hour number
  CHECK(slotOfInstant(Resolution::Min5, t) == t / 300 && slotOfInstant(Resolution::Min15, t) == t / 900);
  CHECK(slotOfInstant(Resolution::Hour4, t) == t / 14400 && slotOfInstant(Resolution::Min1, t) == t / 60);
  // Any instant in the slot is in the same slot; the next second is the next slot.
  CHECK(slotOfInstant(Resolution::Hour1, t + 3599) == 498664 && slotOfInstant(Resolution::Hour1, t + 3600) == 498665);
  CHECK(slotOfInstant(Resolution::Hour1, t - 1) == 498663);
  CHECK(slotOfInstant(Resolution::Hour1, -1) == -1 && slotOfInstant(Resolution::Hour1, -3600) == -1 && slotOfInstant(Resolution::Hour1, -3601) == -2);
  CHECK(instantOfSlot(Resolution::Hour1, 498664) == t && instantOfSlot(Resolution::Min5, slotOfInstant(Resolution::Min5, t)) == t);
  for (Timestamp x : {Timestamp(0), t, t + 1234, Timestamp(-7777)})
    CHECK(instantOfSlot(Resolution::Hour1, slotOfInstant(Resolution::Hour1, x)) <= x && x - instantOfSlot(Resolution::Hour1, slotOfInstant(Resolution::Hour1, x)) < 3600);
}

TEST(a_bar_that_does_not_start_on_a_slot_boundary_is_found_out) {
  auto at = [](int y, unsigned m, unsigned d, int hour, int minute) { return daysSinceUnixEpoch(y, m, d) * 86400 + hour * 3600 + minute * 60; };
  CHECK(onSlotBoundary(Resolution::Hour1, at(2026, 11, 20, 8, 0)));    // Helsinki and Nordnet: on the hour
  CHECK(!onSlotBoundary(Resolution::Hour1, at(2026, 11, 20, 14, 30)));  // a US exchange's open, 09:30 in New York
  CHECK(onSlotBoundary(Resolution::Min30, at(2026, 11, 20, 14, 30)));   // but on a 30-minute boundary
  CHECK(onSlotBoundary(Resolution::Min5, at(2026, 11, 20, 14, 30)) && onSlotBoundary(Resolution::Min15, at(2026, 11, 20, 14, 30)));
}
