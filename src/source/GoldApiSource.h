#pragma once

#include "source/DatasetCodes.h"
#include "source/Source.h"

#include <chrono>
#include <string>
#include <vector>

// gold-api.com (https://api.gold-api.com): the spot prices of metals and a few coins, no key. One request lists the symbols
// (`/symbols`: Gold XAU, Silver XAG, Platinum XPT, Palladium XPD, Copper HG, Bitcoin BTC, Ethereum ETH ...), and one request a price
// for a symbol in a currency (`/price/XAU/EUR`: the price, the currency, the time it was updated). Every symbol is asked for in each
// of the currencies (EUR and USD), so a price in euros is the API's own and not a conversion of ours.
//
// Each symbol is an instrument (by the ISIN source/KnownIsins.h gives it: the real one of the commodity for the metals, one of ours for the coins), priced as it is quoted on XXXX (the market of what is priced and
// not traded), one edge for each currency in the dataset "GOLD-API": one daily bar, on the UTC date the API says it was updated,
// whose close is the last price (no open, high, low or volume: the answer has none), replaced at every load and added on a new day.
// A price is for the unit its row names ("Gold, per troy oz", "Copper, per tonne", a coin per coin): the unit goes with the ISIN (every source
// that has the instrument gives it in that one), and the API says none, so it is the adapter's (the table in GoldApiSource.cpp), which also
// converts what the API quotes otherwise (copper, per pound, is multiplied to be per metric tonne). The physical estimate (PhysicalSource) uses gold and platinum per troy ounce.
// The API says of its prices only that they come from several data providers, the next one taking over when one is down, and that it checks them
// against each other; not which provider answered, nor how a price is made of them (no median or mean is claimed). So a price is the API's own: it
// may differ a little from any one market's or source's, and which provider gave it can change between two loads. The API itself says it cannot
// guarantee the data and that prices are to be verified before a financial decision.
// The metals are commodities, so they are in the Commodities preset; the coins are of no kind. The physical metals estimate (PhysicalSource)
// reads the gold here, in both currencies.
//
// The symbols and the currencies are loaded one answer at a time with a pause, gold first so that what is estimated from it need not
// wait; each symbol is one publication. A refusal (403, 429) ends the load; another failure ends only that request.
class GoldApiSource : public Source {
 public:
  explicit GoldApiSource(std::chrono::milliseconds refresh = std::chrono::minutes(2), std::chrono::milliseconds pause = std::chrono::milliseconds(250),
                           std::vector<std::string> currencies = {"EUR", "USD"})
      : refresh_(refresh), pause_(pause), currencies_(std::move(currencies)) {}

  std::string name() const override { return "gold-api"; }
  std::vector<std::string> datasetCodes() const override { return {kGoldApiDataset}; }
  std::string description() const override { return "gold-api.com: spot prices of gold, silver, platinum, palladium, copper and two coins, in euros and dollars (no key)"; }
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
