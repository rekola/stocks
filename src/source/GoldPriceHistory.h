#pragma once

#include "history/HistoryProvider.h"

#include <chrono>
#include <string>
#include <vector>

// goldprice.dev's daily bars (`/v1/bars?symbol=XAU/USD&from=...&to=...`: open, high, low and close as strings, the bar's UTC start, newest first, a
// cursor to the page after). One request gives the bars of the month, the quarter and the year (it asks for what the plan allows: 30 days
// without a key, so only the month is covered whole). The API has bars for gold, silver and copper in dollars only on that plan (copper is `XCU` there, `HG` in the prices, and per pound: the instrument's tonne is converted): an empty answer or a 403 `plan_gated` (the
// edge is not on the plan) is Missing, a 429 Refused, no answer or a 5xx Offline. The 3 and 10 years read weekly bars, which the API has none of: they are made of the daily bars the store has (the weeks of every load so far, so a load at least once a month keeps them whole), and a longer history, from another source or a paid plan, joins them. Bars of the day and the week would be intraday, which a plan
// without a key has none of, so they are not asked for.
class GoldPriceHistory : public HistoryProvider {
 public:
  struct Options {
    int days = 29;  // how far back to ask: the free plan's depth is 30 days and a day further is refused whole
    std::chrono::milliseconds lifetime{6 * 60 * 60 * 1000};
  };
  GoldPriceHistory() = default;
  explicit GoldPriceHistory(Options options) : options_(options) {}

  std::string dataset() const override;
  std::optional<Need> need(const Snapshot &, const EdgeKey &, Period) const override;
  std::chrono::milliseconds lifetime(int) const override { return options_.lifetime; }
  Outcome load(Store &, Http &, const EdgeKey &, int request, const Wait &) override;

  struct Bars {
    std::vector<Candle> bars;  // oldest first, by day number
    std::string next_cursor;   // empty: no more
  };
  // The weekly bars of daily ones (oldest first, by day number; the weeks are Monday's to Sunday's): a week's open is its first bar's, its close its last bar's,
  // its high and low the extremes, its volume the sum where any bar has one. The first week is left out unless the bars have its Monday: a week that is
  // cut at the start would be a bar of fewer days than the others, and would replace a whole one that another source has.
  static std::vector<Candle> weeks(const std::vector<Candle> & days);
  // False, with `error` set, when `body` is not an answer. A bar with no close or start is left out; a missing open, high or low is undefined.
  static bool parse(const std::string & body, Bars & out, std::string & error);

 private:
  Options options_;
};
