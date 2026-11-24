#pragma once

#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <chrono>
#include <string>
#include <vector>

// goldprice.dev (https://api.gold-api.com): the spot prices of metals and a few coins, no key. One request lists the symbols
// (`/symbols`: Gold XAU, Silver XAG, Platinum XPT, Palladium XPD, Copper HG, Bitcoin BTC, Ethereum ETH ...), and one request a price
// for a symbol in a currency (`/price/XAU/EUR`: the price, the currency, the time it was updated). Every symbol is asked for in each
// of the currencies (EUR and USD), so a price in euros is the API's own and not a conversion of ours.
//
// Each symbol is an instrument (by the ISIN source/KnownIsins.h gives it: the real one of the commodity for the metals, one of ours for the coins), priced as it is quoted on XXXX (the market of what is priced and
// not traded), one edge for each currency in the dataset "GOLDPRICE": one daily bar, on the UTC date the API says it was updated,
// whose close is the last price (no open, high, low or volume: the answer has none), replaced at every load and added on a new day.
// The metals are commodities, so they are in the Commodities preset; the coins are of no kind. The physical metals estimate (PhysicalSource)
// reads the gold here, in both currencies.
//
// The symbols and the currencies are loaded one answer at a time with a pause, gold first so that what is estimated from it need not
// wait; each symbol is one publication. A refusal (403, 429) ends the load; another failure ends only that request.
class GoldPriceSource : public Source {
 public:
  explicit GoldPriceSource(std::chrono::milliseconds refresh = std::chrono::minutes(2), std::chrono::milliseconds pause = std::chrono::milliseconds(250),
                           std::vector<std::string> currencies = {"EUR", "USD"})
      : refresh_(refresh), pause_(pause), currencies_(std::move(currencies)) {}

  std::string name() const override { return "goldprice"; }
  std::vector<std::string> datasetCodes() const override { return {kGoldPriceDataset}; }
  std::string description() const override { return "goldprice.dev: spot prices of gold, silver, platinum, palladium, copper and two coins, in euros and dollars (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  struct Symbol {
    std::string symbol, name;
  };
  struct Price {
    std::string symbol, name, currency;
    double price = 0;
    int64_t day = 0;  // the UTC date of the update, as a day number
  };
  // The `/symbols` answer; false, with `error` set, when it is not a list of symbols. A row without a symbol is left out.
  static bool parseSymbols(const std::string & body, std::vector<Symbol> & out, std::string & error);
  // A `/price` answer; false, with `error` set, when it has no price above zero, or no currency, symbol or update date.
  static bool parsePrice(const std::string & body, Price & out, std::string & error);

 private:
  std::chrono::milliseconds refresh_, pause_;
  std::vector<std::string> currencies_;
  std::vector<Symbol> known_;  // the last list that was read: kept where a later request fails
};
