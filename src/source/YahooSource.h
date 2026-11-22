#pragma once

#include "source/Source.h"
#include "source/InstrumentLists.h"

#include <chrono>

// Yahoo Finance's chart endpoint: daily candles, dividends and splits for every instrument of its listserse. Unofficial
// and without a key, so it can change or throttle without notice; for personal use. The dataset is "yahoo".
class YahooSource : public Source {
 public:
  // Loads `list`: one request per entry. `range` is how far back to ask (1y, 5y, max, ...); `pause` is the wait between
  // requests. Yahoo has no discovery, so the instruments are lists of its own (data/lists), read when it loads.
  explicit YahooSource(InstrumentList list, std::string range = "1y",
                       std::chrono::milliseconds pause = std::chrono::milliseconds(100))
      : list_(std::move(list)), range_(std::move(range)), pause_(pause) {}
  explicit YahooSource(ListSpec spec, std::string range = "1y",
                       std::chrono::milliseconds pause = std::chrono::milliseconds(100))
      : spec_(std::move(spec)), fromFiles_(true), range_(std::move(range)), pause_(pause) {}

  std::string name() const override { return "yahoo"; }
  std::string description() const override { return "Yahoo Finance: daily candles, dividends and splits (no key)"; }
  void load(Store &, Http &, SourceListener &) override;

  // One chart response into the store as `entry`. False, with `error` set, when it holds no data.
  static bool parse(const std::string & json, const ListEntry & entry, Store &, std::string & error);

 private:
  InstrumentList list_;
  ListSpec spec_;
  bool fromFiles_ = false;
  std::string range_;
  std::chrono::milliseconds pause_;
};
