#pragma once

#include "history/HistoryProvider.h"
#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

// goldprice.dev (https://api.goldprice.dev, no key): the spot prices of gold (XAU), silver (XAG) and copper (HG) in 31 currencies, and their
// daily bars. `/v1/prices` answers every symbol in every currency in one request: each row has the symbol, the quote currency, the unit its price
// is for ("troy_ounce", "pound"), the price (a string), when it was computed and whether it is stale. Without a key the prices are 15
// minutes late, and the answer says no more of where they come from. A stale price is not stored.
//
// Each symbol is an instrument by the ISIN source/KnownIsins.h gives it, so the gold here is the gold of Nordnet and of GoldApiSource (another
// dataset of the same instrument), priced on XXXX, one edge in each currency asked for (EUR and USD) in the dataset "GOLDPRICE": its quote, at the instant
// it was computed, whose last price is the price in the instrument's unit (no bar: a live price is not history) (the answer's own unit is read, never assumed: copper per pound is
// multiplied to be per tonne, a unit that is not known leaves the row out and says so), replaced at every load. The
// history is the API's daily bars (GoldPriceHistory), which a key-less caller has for the last 30 days of gold in dollars only.
class GoldPriceSource : public Source {
 public:
  // `currencies`: the quote currencies to store; empty is all the API has.
  explicit GoldPriceSource(std::chrono::milliseconds refresh = std::chrono::minutes(5), std::vector<std::string> currencies = {"EUR", "USD"});

  std::string name() const override { return "goldprice"; }
  std::vector<std::string> datasetCodes() const override { return {kGoldPriceDataset}; }
  std::string description() const override { return "goldprice.dev: spot prices of gold, silver and copper, in euros and dollars, with daily bars (no key; 15 minutes late)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }
  std::shared_ptr<HistoryProvider> historyProvider() override { return history_; }

  struct Price {
    std::string symbol, currency, unit;
    double price = 0;
    Timestamp time = 0;  // when it was computed, in UTC seconds
    bool stale = false;
  };
  // The `/v1/prices` answer: false, with `error` set, when it is not a list of prices. A row without a symbol, a currency, a unit, a price above
  // zero or a time is left out.
  static bool parsePrices(const std::string & body, std::vector<Price> & out, std::string & error);

 private:
  std::chrono::milliseconds refresh_;
  std::vector<std::string> currencies_;
  std::shared_ptr<HistoryProvider> history_;
};
