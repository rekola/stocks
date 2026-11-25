#pragma once

#include "history/HistoryProvider.h"
#include "model/Period.h"

#include <chrono>
#include <string>
#include <vector>

// Yahoo Finance's longer history, from the same chart endpoint as YahooSource: the one place that knows how to ask it for weekly bars.
// The daily bars of the last year come with the list (YahooSource); the 3 and 10 years are one request here, `range=10y&interval=1wk`
// (about 520 weekly bars, 20-30 KB), that feeds both, asked for when one of those periods is on the screen and never for the others
// (they have no need of it). The ticker is the listing's symbol. A 404 (any other 4xx) is Missing, a refusal (403, 429) Refused, and no
// answer or a 5xx Offline.
class YahooHistory : public HistoryProvider {
 public:
  explicit YahooHistory(std::chrono::milliseconds lifetime = std::chrono::hours(24)) : lifetime_(lifetime) {}

  std::string dataset() const override;
  std::optional<Need> need(const Snapshot &, const EdgeKey &, Period) const override;
  std::chrono::milliseconds lifetime(int) const override { return lifetime_; }
  Outcome load(Store &, Http &, const EdgeKey &, int request, const Wait &) override;
  std::chrono::milliseconds pause() const override { return std::chrono::milliseconds(100); }

  // The weekly bars of one chart response, oldest first, each keyed by the first day of its week. False, with `error` set, when the
  // body is not a chart with candles.
  static bool parse(const std::string & json, std::vector<Candle> & bars, std::string & error);

 private:
  std::chrono::milliseconds lifetime_;
};
