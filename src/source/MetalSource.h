#pragma once

#include "source/DatasetCodes.h"
#include "source/Http.h"
#include "source/SeriesReader.h"
#include "source/Source.h"

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

// The prices of metals from a provider that is described by a schema, as one fund company is by a fund schema: a file in data/metals (a schema that comes with the
// program) and not code, so that the code names no provider. The answers are read by `series::read` (SeriesReader.h), which tells their shape, so a schema only
// says what it cannot: where to ask, what for and how the days are told apart.
//
//   name, dataset      what the schema is called (`metals:NAME` picks it) and the code of its dataset ("SOME": one to eleven capitals, digits, hyphens, periods or hashes, and never
//                      changed, since what is kept is kept under it)
//   title              the name the table shows for the dataset; the code, if there is none
//   market             the MIC the edges are on (XXXX, no market, where there is none)
//   pauseMs, refreshHours   the wait between two requests (1000) and how often to load again (3)
//   headers            sent with every request, over a Chrome's (`browserHeaders`: a Referer says which page asks, an empty value leaves the browser's out)
//   request            {url, interval, years, overlapDays, time}: the request for the daily history. `url` has `{from}`, `{to}` and `{interval}`, and `{symbol}` and `{currency}` where the
//                      answer is of one metal in one currency: there is then a request for each. Without `{symbol}` the answer has every metal and currency (series told
//                      apart by their labels: see `labels` of an item and `currencyLabel`), and there is one request. `interval` (P1D) is what is asked for, `years` (10) how far back
//                      the first request goes, `overlapDays` (14) how far before the last day the store has the later ones begin (so a load after the first asks for two weeks,
//                      not ten years), `time` how `{from}` and `{to}` are written: "iso" (2026-11-28T11:36:00Z, the colons percent-encoded, for a URL) or "date" (2026-11-28).
//   currencyLabel      where an answer of every metal and currency says the currency: the label of the series ("Currency").
//   request.recent     {interval, days}: the recent history in finer bars (PT1H, 28), for a provider that has them for the last weeks only (an hourly range longer than a month
//                      comes back daily). The days and the weeks of the last `days` are made of them, and the bars themselves are written (5 minutes and 1 hour, which the
//                      store keeps for a day and a week). The first load asks for the whole history in `interval` and then for the recent in `recent.interval`; a load after it asks
//                      for the recent only (one request), and for the daily history from the last day it has only where the recent does not reach back to it.
//   dayKey             which day a bar is of: "start" (the date it starts on, the default) or "end" (the date of its last second: a bar from 15:00 to 15:00 is of the day it ends,
//                      so the one that starts on a Sunday evening is Monday's)
//   dayBoundaryHour    the hour (UTC) at which a provider's day begins, where it is not midnight (0): the bars before it are of the day before. Bars of any length up to a day are made
//                      into days by it, so a day of hourly bars is the same as a daily bar of the provider.
//   foldWeekends       a bar that comes to a Saturday or a Sunday (the few hours of a Friday evening) is added to the day before (false)
//   fields, rows       hints for the reader (SeriesReader.h `Hints`): which key is which role, a JSON pointer to the rows. Only where its heuristics do not tell.
//   items              [{symbol, name, isin, currencies, type, unit, market, labels}]: what is asked for. `isin` is the metal's (the gold of the other sources), `currencies` ["USD"],
//                      `type` ("METAL"), `unit` ("troy oz") what a price is for, which the table's name for the metal says, `market` the MIC of this metal where it is not the
//                      schema's, `labels` what tells its series in an answer of every metal ({"Code": "AU"}).
//
// What is written, for each metal and currency: an edge of the metal's ISIN, the currency, the market and the schema's dataset, with the daily bars (open, high, low, close; a
// close alone where the answer has a price a day, and the mean of a bid and an ask where it has those), the weekly bars made of them (the 3 and 10 years read weeks), and the
// last price as the edge's quote. A bar length that is not a day's (the answer may give finer or coarser bars than it was asked for) is not written, and is said.
// The first load asks for `years`; where the store has the bars of an edge it asks from `overlapDays` before the last day it has, so that the data used stays small.
struct MetalSchema {
  struct Item {
    std::string symbol, name, isin, type = "METAL", unit = "troy oz", market;  // market: the schema's where empty
    std::vector<std::string> currencies{"USD"};
    std::map<std::string, std::string> labels;
  };
  std::string name, dataset, title, market = "XXXX", currencyLabel = "Currency";
  std::chrono::milliseconds pause{1000}, refresh{std::chrono::hours(3)};
  HttpHeaders headers;
  std::string url, interval = "P1D", time = "iso", dayKey = "start";
  int years = 10, overlapDays = 14, dayBoundaryHour = 0;
  struct Recent {
    std::string interval;  // empty: none
    int days = 0;
  } recent;
  bool foldWeekends = false;
  bool perItem() const { return url.find("{symbol}") != std::string::npos; }
  series::Hints hints;
  std::vector<Item> items;

  // False, with `error` set, when the text is not a schema.
  static bool parse(const std::string & json, MetalSchema &, std::string & error);
  // A schema by its name ("x": data/metals/x.json, here or in the source tree), or by the path of a file.
  static bool load(const std::string & nameOrPath, MetalSchema &, std::string & error);
};

class MetalSource : public Source {
 public:
  // `now` is the clock (UTC seconds), the system's where empty.
  explicit MetalSource(MetalSchema schema, std::function<Timestamp()> now = {});

  std::string name() const override { return "metals:" + schema_.name; }
  std::vector<std::string> datasetCodes() const override { return {schema_.dataset}; }
  std::string description() const override { return schema_.name + ": daily prices of metals (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return schema_.refresh; }

  // The request for one metal in one currency (for an answer of every metal, the one request: the item and the currency are not used), as the schema makes it, for the days from
  // `since` (UTC seconds; 0: the schema's `years` back) to `now`, in the schema's `interval` or the one given.
  static std::string url(const MetalSchema &, const MetalSchema::Item &, const std::string & currency, Timestamp now, Timestamp since = 0, const std::string & interval = {});
  // The daily bars of a series, each of its day (by the schema's `dayKey` and `dayBoundaryHour`), the days of several bars of one day joined (the first open, the last close, the extremes),
  // and the bars that come to a weekend folded into the day before where the schema says so. Oldest first. `dropFirstDay`: the first day is left out (of bars shorter than a
  // day, a request that begins in the middle of a day has only part of it).
  static std::vector<Candle> days(const MetalSchema &, const series::Series &, bool dropFirstDay = false);

 private:
  // The day (UTC seconds) the next request for an edge begins: where the store has its daily and weekly bars, the overlap before the last day it has; else 0, the whole history.
  Timestamp sinceOf(const Snapshot &, const MetalSchema::Item &, const std::string & currency) const;
  // One metal in one currency, its series in hand (the long one, the recent one, or both): checked, made into bars and written. False where it was not.
  bool write(Store &, const MetalSchema::Item &, const std::string & currency, const series::Series * history, const series::Series * recent, Timestamp now, const std::string & label,
             SourceListener &);
  MetalSchema schema_;
  std::function<Timestamp()> now_;
};
