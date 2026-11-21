#pragma once

#include "source/Source.h"
#include "source/Universe.h"

#include <chrono>

// Yahoo Finance's chart endpoint: daily candles, dividends and splits for every instrument of the universe. Unofficial
// and without a key, so it can change or throttle without notice; for personal use. The dataset is "yahoo".
class YahooSource : public Source {
 public:
  // Loads `universe`: one request per entry. `range` is how far back to ask (1y, 5y, max, ...); `pause` is the wait
  // between requests.
  explicit YahooSource(Universe universe, std::string range = "1y",
                       std::chrono::milliseconds pause = std::chrono::milliseconds(100))
      : universe_(std::move(universe)), range_(std::move(range)), pause_(pause) {}

  std::string name() const override { return "yahoo"; }
  std::string description() const override { return "Yahoo Finance: daily candles, dividends and splits (no key)"; }
  void load(Store &, Http &, SourceListener &) override;

  // One chart response into the store as `entry`. False, with `error` set, when it holds no data.
  static bool parse(const std::string & json, const UniverseEntry & entry, Store &, std::string & error);

 private:
  Universe universe_;
  std::string range_;
  std::chrono::milliseconds pause_;
};
