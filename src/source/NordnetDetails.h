#pragma once

#include "source/Source.h"

#include <chrono>
#include <set>
#include <string>
#include <vector>

// What the provider says of one instrument on its own page, beyond its lists: a company's description, number of shares,
// country and classification; a fund's (or an ETF's) description, cut-off time, Morningstar rating and risk, fees and risk
// statistics. All are said as attributes of the instrument. One request an instrument, asked for by the order book id the
// lists gave, so the whole universe is filled in a batch at a time, between the list loads, and an instrument that has its
// description is not asked again.
class NordnetDetailsSource : public Source {
 public:
  // `pause` is the wait between requests; `batch` how many instruments one load fills in before it lets the lists load again.
  explicit NordnetDetailsSource(std::chrono::milliseconds pause = std::chrono::milliseconds(500), size_t batch = 300,
                                std::chrono::milliseconds refresh = std::chrono::seconds(30))
      : pause_(pause), batch_(batch), refresh_(refresh) {}

  std::string name() const override { return "nordnet-details"; }
  std::string description() const override { return "Nordnet: the description, shares, ratings and fees of every instrument, no key"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return refresh_; }

  static std::string stockUrl(const std::string & orderBook);
  static std::string fundUrl(const std::string & orderBook);

  // What a stock's page says; false, with `error` set, when `json` is not one.
  static bool parseStock(const std::string & json, std::vector<Attribute> & said, std::string & error);
  // The same of a fund's or ETF's.
  static bool parseFund(const std::string & json, std::vector<Attribute> & said, std::string & error);

 private:
  std::chrono::milliseconds pause_;
  size_t batch_;
  std::chrono::milliseconds refresh_;
  std::set<uint32_t> tried_;  // the instruments asked for, found or not: a failure is not repeated until the next start
};
