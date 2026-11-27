#pragma once

#include "history/HistoryProvider.h"
#include "model/Period.h"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// The bars of the market-data server (src/market, docs/market-server.md): the one place of the application that knows its /summary, its
// parameters and its JSON. The server is read-only and has the bars of every dataset it ingested, under the dataset that wrote them, so this
// provider serves every dataset the server has (datasets(): what /instruments said, set by the source that read it), and answers for up to
// kBatch edges in one request: GET /summary?edge=ID,ID,...&resolution=R (Api.h: at most 100 edges a request).
//
// A request is the resolution a period's sparkline and change are drawn from (Period.h), so the periods that share one share a request:
//
//   day               5m and 1d    the day's 5-minute bars, and the daily bars its change is measured from (Period.h: readsOf)
//   week              1h
//   month, quarter    1d
//   year, 3y, 10y     1w
//
// What an answer says of an edge (Api.h): it is in "instruments" with the queue asked for (Ok: the bars are written, as the server's queue
// holds them, its kind as the writer said it), or its id is in "missing" (the server has no such edge: Missing), or it is in "instruments"
// without the queue, which is on the server's disk and not in memory yet (Offline: the loader asks again after its pause). An edge that
// never has that resolution would be asked for for ever, so the edge is given up (Missing) after kTries such answers. No answer, or a 5xx, is
// Offline; a 403 or 429 is Refused. A 400 is a bug in this client and any other answer a server that is not this one: both throw, which the
// loader says once and counts the whole batch as missing. Nothing here ever asks another provider.
//
// A queue of the server keeps the windows the model's defaultKeep says (Period.h), so keep() is that; and the server changes only when it
// ingests, so a request is good for a few minutes (lifetime).
class MarketServerHistory : public HistoryProvider {
 public:
  enum Request : int { Intraday = 0, Hourly = 1, Daily = 2, Weekly = 3 };
  static constexpr size_t kBatch = 50;  // under the server's cap of 100 edges a request
  static constexpr int kTries = 3;      // how many times an edge listed without its queue is asked for before it is given up

  explicit MarketServerHistory(std::string baseUrl, std::chrono::milliseconds lifetime = std::chrono::minutes(5));

  // The datasets the server has: the codes /instruments said, replacing what was said before.
  void setDatasets(std::vector<std::string> codes);

  std::string dataset() const override;
  std::vector<std::string> datasets() const override;
  std::optional<Need> need(const Snapshot &, const EdgeKey &, Period) const override;
  std::chrono::milliseconds lifetime(int) const override { return lifetime_; }
  std::optional<std::chrono::seconds> keep(Resolution) const override;
  size_t batchSize() const override { return kBatch; }
  Outcome load(Store &, Http &, const EdgeKey &, int request, const Wait &) override;
  std::vector<Outcome> loadBatch(Store &, Http &, const std::vector<EdgeKey> &, int request, const Wait &) override;

  // The request that feeds a period, and the resolutions it asks for (the first is the one its edges must have).
  static Request requestOf(Period);
  static std::vector<Resolution> resolutionsOf(int request);

 private:
  std::string url_;
  std::chrono::milliseconds lifetime_;
  mutable std::mutex mutex_;
  std::vector<std::string> datasets_;
  std::map<std::string, int> later_;  // how many times an edge and request were answered "not loaded yet", by "id request"
};

// The id the API calls an edge by: the texts of its four keys joined by "/" (Api.h).
std::string marketServerEdgeId(const EdgeKey &);
