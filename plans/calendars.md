# Market calendars

Status: plan, nothing built. A living document, like [server.md](server.md): what was
measured, what is decided, what is proposed and what is open. A section near the end is
for whoever works on the market regime embeddings: it says what the calendars give them
and what they must watch, not the mathematics.

## Why the data needs calendars

- **Dates from instants.** A daily bar is keyed by its trading date (a day number), and
  some providers give only an instant. Today the discovery adapter takes the UTC date of the
  last tick, and the candle adapter reads the date from the nearest local midnight of a
  stamp (see server.md). A calendar says which trading date an instant belongs to.
- **Checking the data.** A session of a given length should hold a given number of bars
  at a given resolution; fewer means a gap in the data. Without the calendar a gap in a
  quiet instrument cannot be told from a market that was closed.
- **Putting bars of different markets side by side**: which hours two markets share,
  and which slots of a global grid a market is open in.
- **Rebuilding bars**: the US hourly bars that start at :30 (see server.md), weekly and
  monthly bars made from daily ones for 24-hour markets.
- **The regime embeddings**, which work on time slots across all markets.

## What a calendar is

- A **session** is an interval [open, close) in UTC, belonging to a **trading date**. It
  has a **kind**: regular, or extended (pre-market, post-market).
- A trading date has no session (the market is closed), one, or several (a lunch break
  splits the day: Tokyo trades 09:00 to 11:30 and 12:30 to 15:30 local time).
- The **trading date is a label**, not the UTC date and not always the local date of the
  open. A session that crosses UTC midnight (Tokyo's morning is on the same UTC date, but
  futures that open on Sunday evening in New York belong to Monday's trading date, and
  currencies "roll" at a convention's cut) has the date the market gives it. The calendar
  defines the label.
- A **calendar** is the set of sessions of one market (or one convention) over time,
  with an id. Markets have calendars; a dataset states which calendar its daily bars
  follow, which is not always its market's (a currency's days may run from London
  midnight whatever the venue). In the graph of statements in server.md, "market has
  calendar" and "dataset's day is cut by calendar" are statements.
- A **day definition** is therefore the calendar that cuts a daily bar. Two daily bars are
  comparable when their definitions are the same; otherwise through the intraday bars of
  the hours they share (server.md, "Comparing across markets").

## What was measured

All of this was read from the data, not assumed. Sources: the Python library
`exchange_calendars` (MIT licence, version 4.13), one-minute bars and chart
metadata from a candle source, and a discovery source's history.

Regular hours of the exchanges that matter first (the discovered markets and the lists on disk):

| Calendar | Market | Time zone | Regular hours (local) | UTC in winter | UTC in summer |
|---|---|---|---|---|---|
| `XHEL` | Nasdaq Helsinki | Europe/Helsinki | 10:00 to 18:30 | 08:00 to 16:30 | 07:00 to 15:30 |
| `XSTO` | Nasdaq Stockholm | Europe/Stockholm | 09:00 to 17:30 | 08:00 to 16:30 | 07:00 to 15:30 |
| `XCSE` | Nasdaq Copenhagen | Europe/Copenhagen | 09:00 to 17:00 | 08:00 to 16:00 | 07:00 to 15:00 |
| `XOSL` | Oslo | Europe/Oslo | 09:00 to 16:20 | 08:00 to 15:20 | 07:00 to 14:20 |
| `XETR` | Xetra | Europe/Berlin | 09:00 to 17:30 | 08:00 to 16:30 | 07:00 to 15:30 |
| `XLON` | London | Europe/London | 08:00 to 16:30 | 08:00 to 16:30 | 07:00 to 15:30 |
| `XNYS` | New York | America/New_York | 09:30 to 16:00 | 14:30 to 21:00 | 13:30 to 20:00 |
| `XTKS` | Tokyo | Asia/Tokyo | 09:00 to 15:30 (lunch break) | 00:00 to 06:30 | 00:00 to 06:30 |

- **Checked against one-minute bars**, 2026-11-20: NOKIA.HE has 510 bars from 08:00
  to 16:29 UTC (a bar is labelled by its start, so the last one is 16:29 for a 16:30
  close), which is `XHEL`; AAPL has 391 bars from 14:30 to 21:00 UTC, which is `XNYS`
  (390 minutes of trading and the bar of the 16:00 closing print). Both agree with the
  library to the minute.
- **The candle source also gives today's sessions**: the chart metadata has `currentTradingPeriod`
  with the pre, regular and post periods as UTC instants (for AAPL pre-market 09:00 to
  14:30 and post-market 21:00 to 01:00 UTC), and the exchange's time zone name. Its
  intraday bars leave out the extended hours unless asked.
- **Holidays**: `XHEL` is closed on 10 weekdays in 2026 (1 January, 6 January, Good
  Friday 3 April, Easter Monday 6 April, 1 May, Ascension 14 May, Midsummer Eve 19 June,
  24, 25 and 31 December), and has no early closes that year.
- **Daylight saving differs between regions**: Helsinki is 7 hours ahead of New York on
  337 days of 2026 and 6 on the other 28 (mid to end of March, and the last week of
  October), because the two change clocks on different dates. Tokyo has no daylight
  saving; the euro markets and New York keep the same local hours all year, so their UTC
  hours move by one hour twice a year.
- **The library's history is not trustworthy for the early years.** By default it covers
  about 20 years back from today and about a year ahead (the last session it knew was
  2027-11-19). It can be started at 1987, but then it shows the *same regular hours for
  every year* for `XHEL`, `XNYS` and `XETR`, from 1987 to 2027. Exchanges have changed
  their hours over those decades, and the library does not model that for these
  calendars. So for the daily history back to 1987 the dates of sessions are good where
  holidays are known, but the hours are not, before roughly 2006.

## Where calendars come from

- **A generator, not a runtime library.** A tool (Python, in `tools/calendars/`, like
  `tools/lists/`) builds the calendars from `exchange_calendars` (holidays, special
  opens and closes, breaks) and the IANA time zone database (the daylight saving rules),
  and writes plain data files into the repository. The C++ code only reads files. So it
  needs neither Python nor a time zone database at run time, the result is reviewable
  and the same on every platform. (C++20's time zone support in `<chrono>` is not
  portable enough yet: it depends on the standard library, and on Windows on a time
  zone database file being present.)
- **Measured data as a check.** One-minute bars from the candle source and the discovery source's intraday history
  are compared with the files to find where they disagree: a changed time, a closure the
  library did not know, an early close.
- **24-hour markets are written by hand** (see below), from the venues' own rules.
- **Other libraries** to know about: `pandas-market-calendars` (MIT) and QuantLib's
  calendars (BSD, C++). Not needed while `exchange_calendars` covers the exchanges.

## Representation

Proposed.

- **Explicit sessions, one row each**, not rules with exceptions: a file per calendar,
  `data/calendars/<id>.csv`, with a `# calendar:` line, a header and one row per session:

      date,kind,open,close
      739940,regular,1795161600,1795192200

  where `date` is the day number of the trading date and `open` and `close` are UTC Unix
  seconds (this row is Nasdaq Helsinki on 2026-11-20, 08:00 to 16:30 UTC). About 250 rows a year, 40 years, 100 calendars: a million rows, which is
  small. Explicit rows make every year correct by construction, including the years
  where the hours were different, and the C++ side is a lookup, not a rule engine.
- **A table of which market uses which calendar**, and which calendar cuts which
  dataset's days: `data/calendars/markets.csv`.
- **The C++ side** (pure functions over the loaded tables):

      sessions(calendar, dayNumber)                      the sessions of a trading date
      isOpen(calendar, instant)
      tradingDate(calendar, instant)                     the label of an instant, if it is in a session
      openSlots(calendar, resolution, firstSlot, count)  a mask over a slot range
      expectedBars(calendar, resolution, dayNumber)      how many bars a full session holds

  The number of bars of a session at a resolution is the session's length divided by
  the length of the resolution (510 minutes is 102 five-minute slots); a half-filled
  slot at the edge of a session is what makes the "US hourly" problem.
- **Years ahead**: the files extend a year or two into the future, regenerated when a
  new year's holidays are published. A calendar that has run out says so instead of
  guessing.

## 24-hour and nearly 24-hour markets

Not covered by `exchange_calendars`, to be written and checked (what follows is from
memory and not yet checked against data):

- **Currencies** trade from Sunday evening to Friday evening, around the clock. The
  week is cut at 17:00 in New York, which moves in UTC with New York's daylight saving;
  one candle source cuts the *day* at London midnight (measured: its daily stamps are at 23:00 UTC
  in summer and 00:00 in winter). Holidays barely close the market, but the providers'
  data thins out.
- **Futures** (the commodities lists) trade nearly 24 hours with a daily break of about an
  hour (for the CME, around 17:00 in Chicago, where also the trading date rolls) and
  shortened sessions on US holidays. `exchange_calendars` may have the CME calendars; to
  be checked.
- **Crypto** is 24 hours, every day, and its day is cut at UTC midnight.

For these a calendar is a weekly pattern (when the week opens and closes, the daily
break) plus the holiday overrides, expanded into the same session rows as the others.

## Using the calendars on the data

- **Trading date of an instant** replaces the guesses in the adapters (the UTC date of
  a discovery tick, which is wrong for a market east of UTC+9 around its open).
- **Expected bars and gaps.** From the sessions and the resolution, the bars that should
  exist; the missing ones are reported, and the loader can ask for exactly those.
- **US hourly bars on the hour**: from finer bars, onto the UTC hour grid, with the
  session edges known, and the half-hour bars at the open and the closing print marked.
- **Weeks, months and quarters for date bars** are by trading date, so a calendar's
  Monday is a Monday; made from daily bars by date arithmetic (server.md).
- **Cross-listings**: an instrument listed on two markets has two calendars (the same
  ETF on Xetra and in Stockholm). Its two series are aligned through the open masks.

## For the regime embeddings

The embeddings have a plan of their own, [embeddings.md](embeddings.md). This is only what
the calendars can give them, and what to watch for; the mathematics is being worked on
elsewhere.

- **A global UTC grid.** If the embeddings use slots of, say, 15 minutes on one UTC
  grid, then at every slot each market is either open or not, so there is always a
  specific slot for each market, and a daylight saving change moves *where in the
  session* a slot falls, not *which slot it is*. Whether the phase matters is for the
  mathematics to decide; the calendars can supply both: an **open mask** per market per
  slot, and where the slot is in the session (minutes since the open, minutes to the
  close, the fraction of the session), for features that want it.
- **States, not just open and closed**: regular session, extended hours, lunch break,
  closed for the day, holiday, weekend. A 24-hour market is open in nearly every slot
  (except the weekend and its daily break), an exchange only in about a quarter of them.
  A mask has to be kept, and missing values must not be filled in as if the market had
  traded.
- **Short sessions**: early closes and the half-filled slots at an open and a close
  (09:30 in New York, a 16:20 close in Oslo) need a rule: a partial slot is kept and
  marked, or left out.
- **Labels**: a bar is labelled by its start (the last one-minute bar of a 16:30 close is
  16:29). Features must not use a bar before it has closed.
- **Weekly and daily seasonality** is local: the open of each market, lunch, the close.
  A UTC time of day mixes these over the year because of daylight saving (the US open is
  at 14:30 UTC in winter and 13:30 in summer); the calendars can give local time of day
  and the weekday of the trading date.
- **Adjusted prices.** Returns across a split are wrong unless the prices are adjusted, and
  a candle source adjusts its history afterwards, so a series changes when a split happens (see
  server.md). The embeddings must be built from a pinned version of the data (the
  server can answer "as of version N") or recomputed when it changes.
- **Different grids**: daily bars from markets with different day definitions are not on
  one grid (server.md). Where the day definitions differ, use the intraday bars of the
  shared hours.
- **Cross-listed instruments** appear twice with different calendars.

## Open questions

- **Hours before about 2006**, and for any exchange that changed them: where the history of
  trading hours comes from (the exchanges' notices, other libraries, the legacy data), and
  how it is checked. The daily history back to 1987 needs the dates of sessions more than
  their hours, but the intraday work and the "same hours" comparison need the hours.
- **Which calendars first**: the discovery source's markets are numbered, not named (4 is Xetra, 11
  Nasdaq Stockholm, 24 Nasdaq Helsinki; 53 to 55 appear to be First North in Sweden,
  Finland and Denmark; 52, 58 and 61 to 64 are unknown). Whether First North, Spotlight and
  the multilateral venues keep the hours of the main exchange needs checking.
- **Auctions, pre-market and post-market**: in the session or separate; the data providers
  differ (a candle source leaves extended hours out of its intraday bars by default).
- **The calendar of a daily bar** when a provider's day is not its market's (currencies
  cut at London midnight): where it is recorded and who checks it.
- **Who regenerates the files**, how often, and how a disagreement between the files and
  the measured data is handled.
- **Time zone changes in the past** (a country that changed its rules): the IANA database
  has them; whether the generator can be trusted back to 1987.

## Steps (not started)

1. `tools/calendars/build_calendars.py`: the generator for a first set of exchanges (the
   table above), the files for about 2000 to a year ahead, and a script that compares them
   with one-minute bars from the candle source.
2. The C++ reader and the functions above, with tests (the sessions of known days, the
   trading date of an instant on both sides of a daylight saving change, the expected
   bars of a session, the open mask).
3. Use it for the trading date in the adapters, for gap detection and for rebuilding the
   US hourly bars.
4. The 24-hour calendars (currencies, futures, crypto), by hand, then checked against data.
5. The history of trading hours for the early years.
