#pragma once

#include "history/HistoryProvider.h"
#include "model/Period.h"

#include <chrono>
#include <string>
#include <vector>

// Nordnet's price history, from its price time series: the one place that knows its endpoint, headers and answers. The history loader
// asks it, for the edges that are wanted, for the request that feeds the period on the screen (a request is remembered with the edge
// and the time it was loaded, and what the store has already for the period is used without one):
//
//   day      DAY_1 at MIN_5: the last session's 5-minute bars (about 100 for a stock, 10-15 KB) and the previous close
//   week     WEEK_1 at HOUR_1: about 45 hourly bars (5-8 KB)
//   month    MONTH_3 at DAY, the request of the quarter: it is shared, so a month then a quarter is one request (3-10 KB)
//   quarter  MONTH_3 at DAY: about 60 to 90 bars (3-10 KB)
//   year     YEAR_1 at WEEK: about 53 weekly bars (5-8 KB); the daily bars of a year would be five times that for a trace
//            that has 22 cells
//   3 years  YEAR_3 at WEEK: about 156 weekly bars
//   10 years YEAR_10 at WEEK: about 520 weekly bars
//
// A fund has a NAV once a day and no intraday bars, so its day and week are the quarter's request. The whole history is for a server
// that backfills once and keeps it (Full): the whole period, and where Nordnet answers with a coarser resolution for it (a commodity's
// whole period comes weekly, back to 1997) that is stored too and the longest period that gives days is asked for next (the last ten
// years, five, three). Full exists and is tested; nothing in the interactive program uses it. The resolution that is stored is the one
// the answer says it returned, never the one asked for: Nordnet substitutes silently. A fund's series is its NAV (a bar with only a
// close).
//
// The day, week and year series fill in over time, so each has a lifetime (Options): once it is older, an edge that is still wanted is
// asked for again (the loader queues only what is wanted, so only the rows on screen and the margin, never in the background). The
// quarter's daily bars never expire: discovery keeps today's bar. What this adds, for a screen of 40 rows on the day: 40 requests every
// 15 minutes, 160 an hour (one every 22 seconds, about 1,400 over an 8.5-hour session), a round of 20 seconds at a 500 ms pause,
// 10-15 KB each. The screen after and the one before are wanted as well, so with them up to 120 requests (a minute at that pause) every
// 15 minutes. The week adds the same every hour and the year every day; only the period shown is asked for.
//
// A 404 (any other 4xx) is Missing, a refusal (403, 429) Refused, and no answer or a 5xx Offline.
class NordnetHistory : public HistoryProvider {
 public:
  enum class Depth { Recent, Full };
  struct Options {
    Depth depth = Depth::Recent;                                   // the last three months, or all there is
    std::chrono::milliseconds day_lifetime{15 * 60 * 1000};        // DAY_1 at MIN_5
    std::chrono::milliseconds week_lifetime{60 * 60 * 1000};       // WEEK_1 at HOUR_1
    std::chrono::milliseconds year_lifetime{24 * 60 * 60 * 1000};  // YEAR_1 at WEEK
    std::chrono::milliseconds year3_lifetime{24 * 60 * 60 * 1000};   // YEAR_3 at WEEK
    std::chrono::milliseconds year10_lifetime{24 * 60 * 60 * 1000};  // YEAR_10 at WEEK
    std::chrono::milliseconds quarter_lifetime{0};                // MONTH_3 at DAY: discovery keeps today's bar
  };
  // The requests, as the loader names them.
  enum Request : int { Quarter = 0, Day = 1, Week = 2, Year = 3, Year3 = 4, Year10 = 5, Whole = 6 };

  NordnetHistory() = default;
  explicit NordnetHistory(Options options) : options_(options) {}

  std::string dataset() const override;
  // Nordnet's edges, and the indicators' (Morningstar's prices, which Nordnet passes on): both are asked for by the order book.
  std::vector<std::string> datasets() const override;
  std::optional<Need> need(const Snapshot &, const EdgeKey &, Period) const override;
  std::chrono::milliseconds lifetime(int request) const override;
  std::optional<std::chrono::seconds> keep(Resolution) const override;
  Outcome load(Store &, Http &, const EdgeKey &, int request, const Wait &) override;

  // One answer of the price time series, as the bars it holds.
  struct Series {
    Resolution resolution = Resolution::Day;  // the one the answer returned
    std::vector<Candle> bars;                 // oldest first: by slot number (intraday), else by day number (a period's first day for the coarser)
    double previousClose = kUndefined;        // the close before the session, as the answer gives it
    std::vector<std::string> periods;         // the periods it offers
    bool supported = false;                   // the resolution is one we store
  };
  // False, with `error` set, when `json` is not an answer. `nav`: the points are a fund's, with only a value.
  static bool parse(const std::string & json, bool nav, Series & out, std::string & error);

 private:
  Options options_;
};
