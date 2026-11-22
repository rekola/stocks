#pragma once

#include "source/FundRows.h"
#include "source/Source.h"

#include <chrono>
#include <map>

// Aktia's funds, from the data service behind its website: the list of funds, each one's fees and its NAV history. No key. The
// dataset is "aktia" and the market "Aktia Funds"; a fund is its ISIN, a bar is its NAV of the day in euro.
//
// The list gives Aktia's own product ids and no ISINs (except in the id of the Luxembourg funds, `LU0823428346EUR`), so the
// ISINs of the others are a table here. A product the table does not know is reported and left out; one the table marks as having
// none (a fund that is closed, joined to another, or priced only quarterly) is left out without a word.
class AktiaSource : public Source {
 public:
  // `pause` is the wait between requests; they are made one at a time.
  explicit AktiaSource(std::chrono::milliseconds pause = std::chrono::seconds(2),
                       std::chrono::milliseconds refresh = std::chrono::hours(6))
      : pause_(pause), refresh_(refresh) {}

  std::string name() const override { return "aktia"; }
  std::string description() const override { return "Aktia: funds and their NAV history (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  struct Product {
    FundRow fund;
    std::string productId;
  };
  // The funds of the list answer: the ones with an ISIN. `unknown` gets the ids the table has nothing for, with the Finnish
  // names. False, with `error` set, when `json` is not a list.
  static bool parseList(const std::string & json, std::vector<Product> & products, std::vector<std::string> & unknown,
                        std::string & error);
  // The ISIN the table has for an id: null where it has none (nothing: a product that is not wanted), and for an id it does not
  // know, `known` is false.
  static std::optional<std::string> isinOf(const std::string & productId, bool & known);
  // The fees of a product-base-details answer, onto `fund`. False, with `error` set, when `json` is not one.
  static bool parseFees(const std::string & json, FundRow & fund, std::string & error);

 private:
  std::chrono::milliseconds pause_, refresh_;
  std::map<std::string, FundRow> fees_;  // what the fee requests gave, by product id: they are asked once
};
