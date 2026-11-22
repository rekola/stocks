#pragma once

#include "model/Store.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

// What the fund-company sources (Aktia, OP) have in common: a fund is an instrument with an ISIN and a quote currency of euro,
// priced once a day by its NAV, as one dataset says it on a market of that company's own.

// One fund as a company's list says it.
struct FundRow {
  std::string isin, name;
  std::string symbol;      // what the company calls it (its ticker, else the ISIN)
  std::string identifier;  // the company's own id for it: what its price history is asked for by
  std::optional<double> managementFee, subscriptionFee, redemptionFee;  // per cent
  std::optional<int64_t> risk, rating, esg;                              // the risk class, the Morningstar rating, the ESG rating
};

// A number the company gives as a number or as a string of one ("280.18"); nothing where it is missing, null or not a number.
std::optional<double> looseNumber(const nlohmann::json & object, const char * key);
std::optional<int64_t> looseInteger(const nlohmann::json & object, const char * key);

// A history answer of [[time in ms, value], ...] as NAV bars (a close and nothing else), oldest first, one per day, the later of
// two on a day standing. The answer may also be wrapped as a chart's series (`[{"name": ..., "data": [[...]]}]`). False, with `error`
// set, when there is no point in it.
bool parseNavSeries(const std::string & body, std::vector<Candle> & bars, std::string & error);

// The trading day of a NAV stamped `ms` milliseconds after the epoch: the nearest midnight, so that a stamp at midnight UTC and
// one at midnight in Helsinki (a few hours before it, in UTC) are both on the day they say.
int64_t navDay(int64_t ms);

// Whether the series is too sparse to show: a fund priced monthly, quarterly or yearly. Judged by the typical (median) gap
// between its latest points, so a daily fund with a few missing days is kept. A series of one bar is not judged.
bool tooSparse(const std::vector<Candle> & bars);

// The fund and its history into the store: the instrument by its ISIN (kind fund), its fees and ratings, and the edge on
// `market` in euro under `dataset`. False, writing nothing, when there are no bars.
bool writeFund(Store &, const std::string & dataset, const std::string & market, const FundRow &, const std::vector<Candle> & bars);
