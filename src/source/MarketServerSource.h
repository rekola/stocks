#pragma once

#include "source/MarketServerHistory.h"
#include "source/Source.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// The client of the market-data server (src/market, docs/market-server.md): reads its read-only HTTP API and writes what it gets into the store,
// so that the server's instruments are rows like any source's. This and MarketServerHistory are the only code that knows the server's paths,
// parameters and JSON (Api.h).
//
//   load()     GET /instruments (no bars): every instrument with its edge, the dataset that wrote it, its listing and what makes its base
//              node. Each is made as the server made it (a currency by its code, an instrument by its ISIN and kind, an index quoted in
//              points), under its own dataset with the code and name the server says, and the listing as it says it; the edge made must be
//              the one the server names, or the instrument is skipped. Rows only: the bars are loaded when the rows are on the screen, in
//              batches, by the history provider (historyProvider()).
//   again      every refreshInterval(): the server changes only when it ingests, so a new instrument is looked for every ten minutes, and
//              every half minute while the server cannot be reached.
//
// Datasets are never combined here: the server does not combine them, and each instrument stays under the dataset that produced it, so that
// whatever combines them later finds each one's rows intact. The server sends bars only, so no quote, previous close, change, high or low is
// made up: the store derives the price, the changes and the ranges from the bars, as for any source.
//
// This source is the application's whole supply of market data while it runs (suppliesAllData()): nothing else is loaded, and nothing is loaded
// from a provider for what the server has not got.
class MarketServerSource : public Source {
 public:
  static constexpr const char * kDefaultUrl = "http://127.0.0.1:8080";
  struct Options {
    std::string url = kDefaultUrl;
    std::chrono::milliseconds refresh = std::chrono::minutes(10);       // the next look at /instruments
    std::chrono::milliseconds retry = std::chrono::seconds(30);         // after a look that got no answer
    std::chrono::milliseconds history_lifetime = std::chrono::minutes(5);
  };
  MarketServerSource() : MarketServerSource(Options{}) {}
  explicit MarketServerSource(Options);

  std::string name() const override { return "market-server"; }
  std::string description() const override { return "the market-data server (src/market): every dataset and bar it has, and nothing from the providers; runs alone"; }
  // Its datasets are the server's, known only once /instruments is read, and none of them is the source's own: it reports none, so the check
  // for two sources that write one dataset has nothing to compare, and what keeps others from writing beside it is suppliesAllData().
  std::vector<std::string> datasetCodes() const override { return {}; }
  bool suppliesAllData() const override { return true; }
  void load(Store &, Http &, SourceListener &) override;
  std::chrono::milliseconds refreshInterval() const override { return reached_ ? options_.refresh : options_.retry; }
  std::shared_ptr<HistoryProvider> historyProvider() override { return history_; }

  // What a /instruments answer is made into: the rows. Throws std::runtime_error where the answer is no list of instruments. `said` is
  // told once of each instrument that cannot be made (it is skipped), `datasets` gets the codes of the datasets of those that were.
  static void write(const std::string & body, Store &, const std::function<void(const std::string &)> & said, std::vector<std::string> & datasets);

 private:
  void report(SourceListener &, const std::string & text);

  Options options_;
  std::shared_ptr<MarketServerHistory> history_;
  std::atomic<bool> reached_{true};  // the last look got an answer
  std::string lastReport_;           // what was last said, so that a server that stays down is said once
  std::set<std::string> skipped_;    // the ids that cannot be made, said once
};
