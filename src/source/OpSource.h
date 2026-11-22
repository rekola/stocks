#pragma once

#include "source/FundRows.h"
#include "source/Source.h"

#include <chrono>

// OP's funds, from the fund list behind its website and the price charts beside it. No key, but the list is asked for with a token
// that the funds page hands out (it is in a script on the page). The dataset is "op" and the market "OP Funds"; a fund is its ISIN, a bar
// is its NAV of the day in euro. The funds that are priced less often than daily (monthly, quarterly) are left out.
class OpSource : public Source {
 public:
  // `pause` is the wait between requests; they are made one at a time.
  explicit OpSource(std::chrono::milliseconds pause = std::chrono::seconds(2), std::chrono::milliseconds refresh = std::chrono::hours(6))
      : pause_(pause), refresh_(refresh) {}

  std::string name() const override { return "op"; }
  std::string description() const override { return "OP: funds and their NAV history (no key)"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  struct Product {
    FundRow fund;
    std::string notation;  // OP's id for the fund's price series
  };
  // The token of the funds page: the part of the query that the list and the charts are asked for with ("a=b&c=d", without a leading
  // `&` or `?`). False, with `error` set, when the page has none.
  static bool parseToken(const std::string & page, std::string & token, std::string & error);
  // The daily funds of the list answer that have a valid ISIN; `unnamed` gets the names of the rows that have none (or a wrong one), which
  // are not stored. False, with `error` set, when `json` is not a list.
  static bool parseList(const std::string & json, std::vector<Product> & products, std::vector<std::string> & unnamed, std::string & error);
  // Whether OP prices the fund less often than daily.
  static bool pricedLessOften(const std::string & isin);

  static std::string listUrl(const std::string & token);
  static std::string chartUrl(const std::string & notation, const std::string & token);

 private:
  std::chrono::milliseconds pause_, refresh_;
};
