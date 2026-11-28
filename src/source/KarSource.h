#pragma once

#include "model/Dates.h"
#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

// The daily prices of gold, silver, platinum and palladium that K. A. Rasmussen's price page (karasmussen.fi) publishes, each in USD, EUR, NOK, SEK and DKK.
// No key. The page says it uses the London Metal Exchange's data under licence, while the codes in the answer (`XAUFIXPM=`, `LDNXAG=`) are those of
// London fixings: what the prices are is the publisher's to say, so the dataset is the publisher's, "KAR", and the metals are on the London Bullion
// Market (XLBM). Rhodium, which the page has too in rows of another kind, is not carried.
//
// One request answers for a range of days, every metal and currency in it, as the page's script asks (browser headers, the page as Referer):
//   GET https://karasmussen.fi/wp-json/price-history/v1/prices?from=2019-11-01&to=2026-11-28
//   [{"Code":"AU","RateType":"P","RIC":"XAUFIXPM=","Prices":[{"Currency":"EUR","TradeTime":"2026-11-26T15:00:00","Bid":1.0978,"Ask":1.098,
//                                                         "BidGram":104.79,"AskGram":104.81}, ...]}, ...]
// A metal has one row for each currency and day. `TradeTime` is UTC (it moves an hour with London's clocks). `BidGram` and `AskGram` are the price of a gram in
// the row's currency; `Bid` and `Ask` are the price of an ounce in dollars for USD, and the exchange rate it was converted with for every other
// currency. So the answer gives one price, the fix in dollars, and a bid and an ask only where it was converted: the sides of the rate, a few
// thousandths of a per cent apart, which is what converting costs and not the metal's own spread. USD rows have `Bid` equal to `Ask`.
//
// What is written, for each metal in each currency, an edge of the metal's ISIN (the gold of the other sources), the currency, XLBM and "KAR":
//   - the daily bars, one value a day, per troy ounce (the unit of the metals' ISINs: the gram x 31.1035), at the mean of the bid and the ask, and the
//     weekly bars of those (the 3 and 10 years read weeks, which the answer has none of);
//   - the last day's price as the edge's `Quote`: last, and a bid and an ask where the answer gives them apart (not for USD, which is one price). The
//     history of bid and ask is not kept: the store has no series for it (plans/quotes.md).
// The whole history is asked for at each load (about 1 MB, a few hundred kB compressed): what the store has already it keeps.
class KarSource : public Source {
 public:
  explicit KarSource(std::chrono::milliseconds refresh = std::chrono::hours(3), std::function<Timestamp()> now = {});

  std::string name() const override { return "kar"; }
  std::vector<std::string> datasetCodes() const override { return {kKarDataset}; }
  std::string description() const override { return "K. A. Rasmussen: daily prices of gold, silver, platinum and palladium in USD, EUR, NOK, SEK and DKK, with bid and ask (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  static constexpr double kGramsPerTroyOunce = 31.1035;  // the figure the provider divides by

  // One metal in one currency on one day, as a price per troy ounce.
  struct Row {
    std::string metal;     // the answer's code: "AU"
    std::string currency;  // "EUR"
    Timestamp time = 0;    // UTC seconds
    double bid = 0, ask = 0;
    double mid() const { return (bid + ask) / 2; }
  };
  struct Parsed {
    std::vector<Row> rows;
    size_t skipped = 0;  // rows that were not prices (no time, a price that is not a number above zero, a bid above the ask)
  };
  // False, with `error`, where the body is not a list of metals with prices. A row that is not a price is counted in `skipped`; a metal or a currency
  // that is not carried is left out and not counted.
  static bool parse(const std::string & body, Parsed & out, std::string & error);
  // The request for the days from the first one there are prices of to `last`'s.
  static std::string url(Timestamp last);

 private:
  std::chrono::milliseconds refresh_;
  std::function<Timestamp()> now_;
};
