#pragma once

#include "source/Source.h"

#include <chrono>
#include <set>
#include <string>
#include <vector>

// What the provider says of one instrument on its own page, beyond its lists: a company's description, number of shares,
// country and classification; a fund's (or an ETF's) description, cut-off time, Morningstar rating and risk, fees and risk
// statistics. All are said as attributes of the instrument. One request an instrument, asked for by the order book id the
// lists gave; an instrument that has its description is not asked again.
class NordnetDetails {
 public:
  static std::string stockUrl(const std::string & orderBook);
  static std::string fundUrl(const std::string & orderBook);

  // What a stock's page says; false, with `error` set, when `json` is not one.
  static bool parseStock(const std::string & json, std::vector<Attribute> & said, std::string & error);
  // The same of a fund's or ETF's.
  static bool parseFund(const std::string & json, std::vector<Attribute> & said, std::string & error);

  // Asks for the pages of up to `batch` instruments of the Nordnet dataset that lack them, `pause` apart. True if more remain.
  // A refusal (403, 429) stops it for the session: it is said, and false is returned then and at every call after.
  bool loadBatch(Store &, Http &, SourceListener &, std::chrono::milliseconds pause, size_t batch);

 private:
  std::set<uint64_t> tried_;  // the instruments asked for, found or not: a failure is not repeated until the next start
  bool refused_ = false;      // the provider refused: nothing is asked for again this session
};
