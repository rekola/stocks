#pragma once

#include "source/Source.h"

#include <chrono>
#include <functional>
#include <vector>

// One daily rate of the European Central Bank's euro foreign exchange reference rates: how many units of `currency` one euro
// costs, on a date (a day number).
struct EcbRate {
  std::string currency;
  int64_t day = 0;
  double rate = 0;
};

// The ECB's euro foreign exchange reference rates (about 30 currencies, daily, since 1999), from the ECB's own data portal: an
// open, official source (no key; the ECB's terms ask that it be cited as the source, which the dataset's attributes do and the
// README does). The rates are published for information, around 16:00 CET on working days.
//
// The rates are all euro against another currency, a star around the euro. Each is an edge EUR -> X on the market `Currencies`
// under the dataset `ecb` (a bar with only a close: the ECB gives one rate a day), and the cross of two of them is the
// quotient of its two edges (a derived rate, which is not stored).
//
// How much: the last `days` days, in one request (400 days of about 30 currencies is half a megabyte); the whole history
// since 1999 is one request of 15 MB, for a server that backfills once.
class EcbSource : public Source {
 public:
  struct Options {
    int days = 400;                                  // how far back; 0 is all there is (since 1999)
    std::chrono::minutes refresh{360};               // the rates change once a working day
    std::function<int64_t()> now;                    // UTC seconds; the system's clock if empty (for tests)
  };
  EcbSource() : EcbSource(Options{}) {}
  explicit EcbSource(Options options) : options_(std::move(options)) {}

  std::string name() const override { return "ecb"; }
  std::string description() const override { return "European Central Bank euro foreign exchange reference rates, no key"; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return options_.refresh; }

  // The rates of a csvdata answer (`format=csvdata&detail=dataonly`), by the names of its columns; False, with `error` set, when it
  // is not one. A row with no rate (the ECB marks a missing day so) is left out.
  static bool parse(const std::string & csv, std::vector<EcbRate> & rates, std::string & error);
  // The request: all currencies, daily, from the date `days` days before `now` (UTC seconds), or from 1999 for 0.
  static std::string url(int days, int64_t now);

 private:
  Options options_;
};
