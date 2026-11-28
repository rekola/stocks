#pragma once

#include "model/Dates.h"
#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

// The daily prices of gold, silver, platinum, palladium and rhodium in dollars.
//
// One request answers for a range of days, every metal and currency in it, as the page's script asks (browser headers, the page as Referer):
//   GET https://karasmussen.fi/wp-json/price-history/v1/prices?from=2019-11-01&to=2026-11-28
//   [{"Code":"AU","RateType":"P","RIC":"XAUFIXPM=","Prices":[{"Currency":"USD","TradeTime":"2026-11-26T15:03:00","Bid":3578.9,"Ask":3578.9,
//                                                         "BidGram":115.06,"AskGram":115.06}, ...]}, ...]
// A metal has one row for each currency and day; `TradeTime` is UTC (it moves an hour with London's clocks). Only the dollar's rows are the metal's own price: `Bid` and
// `Ask` are the fix of an ounce, equal, since a fix is one price. The rows of the other currencies (EUR, NOK, SEK, DKK) are that price converted with an exchange rate
// (`Bid` and `Ask` are then the sides of the rate, and the gram's price after them), so they say nothing the dollar's does not and carry the cost of converting as
// if it were the metal's spread: they are left out.
//
// What is written, for each metal, an edge of the metal's ISIN (the gold of the other sources), the dollar and "KAR", on the London Bullion Market (XLBM), whose
// price the fix is, for gold, silver, platinum and palladium, and on no market for rhodium (it has no fix). It is one row of the metal with the other spot
// prices when the rows are of an instrument whatever its market (plans/data-model.md):
//   - the daily bars, one value a day, per troy ounce (the unit of the metals' ISINs), and the weekly bars of those (the 3 and 10 years read weeks,
//     which the answer has none of);
//   - the last day's price as the edge's `Quote`: a last price, with no bid or ask.
// The whole history is asked for at each load (about 1 MB, a few hundred kB compressed): what the store has already it keeps. It begins in November 2019
// (the dollar's rows of gold in December 2020).
class KarSource : public Source {
 public:
  explicit KarSource(std::chrono::milliseconds refresh = std::chrono::hours(3), std::function<Timestamp()> now = {});

  std::string name() const override { return "kar"; }
  std::vector<std::string> datasetCodes() const override { return {kKarDataset}; }
  std::string description() const override { return "daily prices of gold, silver, platinum, palladium and rhodium in dollars (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  // One metal on one day, as the price of a troy ounce in dollars.
  struct Row {
    std::string metal;   // the answer's code: "AU"
    Timestamp time = 0;  // UTC seconds
    double price = 0;
  };
  struct Parsed {
    std::vector<Row> rows;
    size_t skipped = 0;  // dollar rows that were not prices (no time, a price that is not a number above zero, a bid above the ask)
  };
  // False, with `error`, where the body is not a list of metals with prices. A dollar row that is not a price is counted in `skipped`; a metal that is not
  // carried and the rows of the other currencies are left out and not counted.
  static bool parse(const std::string & body, Parsed & out, std::string & error);
  // The request for the days from the first one there are prices of to `last`'s.
  static std::string url(Timestamp last);

 private:
  std::chrono::milliseconds refresh_;
  std::function<Timestamp()> now_;
};
