#pragma once

#include "market/Dataset.h"
#include "market/RawCache.h"
#include "source/Http.h"

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// The daily prices of the precious metals that karasmussen.fi's price page publishes and labels as the London Metal Exchange's, as a market-data
// dataset: gold, silver, platinum and palladium, each in USD, EUR, NOK, SEK and DKK. (The codes in the answer, `XAUFIXPM=`, `LDNXAG=`, are
// those of London fixings, which the page's label does not say: whether the prices are the exchange's, or a reseller's of the London market's, is
// the provider's to say. The dataset is named for the label, `LME`, which the page says it has the data under licence from; the four fixed metals are
// priced on the London Bullion Market (XLBM). The page has rhodium too, whose rows are of another kind and which neither market prices: it is not carried.) Everything that names the
// endpoint, its fields and its formats is in this adapter. It is an offline dataset, loaded by market-ingest on request (src/market/offline/).
//
// One request answers for a range of days, every metal and currency in it:
//   GET https://karasmussen.fi/wp-json/price-history/v1/prices?from=2026-01-01&to=2026-12-31   (the page's script asks as a browser does)
//   [{"Code":"AU","RateType":"P","RIC":"XAUFIXPM=","Prices":[{"Currency":"EUR","TradeTime":"2026-11-26T15:00:00","Bid":1.0978,"Ask":1.098,
//                                                         "BidGram":104.79,"AskGram":104.81}, ...]}, ...]
// A price has one row for each metal, currency and day. `TradeTime` is UTC (it moves an hour with London's clocks). `BidGram` and `AskGram` are the price of a
// gram in the row's currency; `Bid` and `Ask` are the price of an ounce in dollars for USD, and the exchange rate (what the price in dollars was
// converted with) for every other currency. So the answer gives one price, the fix in dollars, and a bid and an ask only where it was converted: a
// difference of a few thousandths of a per cent in the rate, which is what converting costs and not the metal's own spread. USD rows have
// `Bid` equal to `Ask`.
//
// What is kept:
//   - the day's price, per troy ounce (the unit of the metals' ISINs: gram x 31.1035), as a daily bar with open, high, low and close the same, no volume, and the
//     weekly bars of those. The mean of the bid and the ask where there are two sides. No 5 minute or 1 hour bars: there is one price a day.
//   - the last row, as the instrument's `Quote` (the summary's last, bid and ask). Bid and ask are written only where the answer gives them apart, since a row
//     with the two equal says one price. The history of bid and ask is not kept, because the store has no series for it (plans/quotes.md, "The quote series").
// A year's answer is kept in the raw cache as it came (`lme_YYYY.json`). A year that is past is asked for once; the current year again when asked to refresh.
namespace lme {

constexpr double kGramsPerTroyOunce = 31.1035;  // the figure the provider divides by

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
// False, with `error`, where the body is not a list of metals with prices. A row that is not a price is counted in `skipped`, and a metal or a currency
// the dataset does not carry is not.
bool parse(const std::string & body, Parsed & out, std::string & error);

std::string url(int year);       // the request for a calendar year
std::string fileName(int year);  // "lme_2026.json"

// The instruments: each metal in each currency, as built into the adapter (the endpoint has no list). The code of one is "xaueur".
const std::vector<market::InstrumentSpec> & instruments();

struct Settings {
  std::chrono::milliseconds pause{1000};  // between two requests
  int retries = 2;                        // more tries of a request that failed
  std::function<Timestamp()> now;         // UTC seconds; the system clock where empty
  std::function<void(std::chrono::milliseconds)> sleep;  // the thread's sleep where empty
};

class Lme : public market::Dataset {
 public:
  Lme(Http & http, market::RawCache cache, Settings settings = {});
  std::string code() const override { return "LME"; }
  std::string name() const override { return "London Metal Exchange (karasmussen.fi)"; }
  std::vector<market::InstrumentSpec> discover(const market::Log & log, bool refresh) override;
  market::LoadResult load(const market::InstrumentSpec & instrument, const market::LoadOptions & options, const market::Log & log) override;

 private:
  struct Year {
    bool ok = false;
    std::string why;
    std::vector<Row> rows;
  };
  // A year's rows, read once for the run: from the cache, or else from the site (which then puts the answer in the cache).
  const Year & year(int year, bool refresh, int currentYear, market::LoadResult &, const market::Log & log);
  void pace();
  Timestamp fetchedAt(const std::string & file) const;
  void noteFetched(const std::string & file);
  Http & http_;
  market::RawCache cache_;
  Settings settings_;
  std::map<int, Year> years_;
  std::chrono::steady_clock::time_point lastRequest_{};
  bool requested_ = false;
};

}  // namespace lme
