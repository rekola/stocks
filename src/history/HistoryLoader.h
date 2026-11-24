#pragma once

#include "history/HistoryProvider.h"
#include "model/Period.h"
#include "model/Store.h"
#include "source/Http.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class CountedHttp;

// One request of one edge: the edge and which of its provider's requests (an id the provider defines).
struct HistoryAsk {
  EdgeKey edge;
  int request = 0;
  friend bool operator==(const HistoryAsk &, const HistoryAsk &) = default;
};
template <>
struct std::hash<HistoryAsk> {
  size_t operator()(const HistoryAsk & a) const { return hashCombine(std::hash<EdgeKey>()(a.edge), static_cast<size_t>(a.request)); }
};

// Loads price history on a thread of its own, for the edges that are wanted: a caller says which (want(): the rows on screen and a
// margin, most wanted first) and the loader has the dataset's HistoryProvider load them, one request at a time with a pause between
// them, the bars written to the store as they arrive. The loader only queues, paces, retries and remembers; where the history is, how
// much of it a period needs and what an answer looks like are the provider's (HistoryProvider.h), and it names none.
//
// What is asked of an edge is what its provider says the period needs (HistoryProvider::need), once, and again only once it has
// expired (HistoryProvider::lifetime; a request that has none is once per session). Only want() queues an expired request, so only the
// rows on screen and the margin are asked for again, never in the background, and a row scrolled away is not. What the store has
// already for the period is used without a request, for a request not loaded before.
//
//   Failures: Missing (a 404, any other 4xx) is remembered for the session for that edge. A refusal (403, 429) stops that provider for the
//   session, and not the others. No answer or a 5xx pauses requests for a minute, and the edge is asked for again afterwards.
class HistoryLoader {
 public:
  using Ask = HistoryAsk;
  struct Options {
    std::chrono::milliseconds pause{500};            // between two requests
    std::chrono::milliseconds offline_pause{60000};  // after no answer
  };
  // `changed` runs on the loader's thread after bars were written; a program hands it to the UI thread. `message` says
  // what went wrong (a refusal).
  HistoryLoader(std::shared_ptr<Store> store, std::unique_ptr<Http> http, Options options, std::function<void()> changed,
                std::function<void(std::string)> message = {});
  ~HistoryLoader();
  HistoryLoader(const HistoryLoader &) = delete;
  HistoryLoader & operator=(const HistoryLoader &) = delete;

  // What is wanted now for `period`, most wanted first. It replaces what was wanted before: edges scrolled past are not
  // loaded. An edge with no provider, that its provider cannot load, has what the period needs in the store already, or asked
  // for it and failed or was answered, is left out.
  void want(const std::vector<EdgeKey> & edges, Period period = kDefaultPeriod);

  // What loads the history of one dataset's edges (a source hands one over: Source::historyProvider). A refusal stops that provider and
  // not the others.
  void addProvider(std::shared_ptr<HistoryProvider>);

  // Whether a provider's refusal has stopped it for this session.
  bool refused() const;
  // Blocks until nothing is queued or being loaded (for tests).
  void waitIdle();

 private:
  void run();
  // Loads one request of one edge; returns whether bars were written. `network` is set if a request was made (by the count of
  // the requests that went through `counted_`, not by what the provider says).
  bool load(const Ask & ask, bool & network);
  // Whether a request loaded at `loaded` is too old to stand (under the lock).
  bool expired(const Ask &, std::chrono::steady_clock::time_point loaded) const;
  std::shared_ptr<HistoryProvider> providerOf(const std::string & dataset) const;  // under the lock

  std::shared_ptr<Store> store_;
  std::unique_ptr<Http> http_;
  std::unique_ptr<CountedHttp> counted_;  // what the providers get: http_, counting the requests
  Options options_;
  std::function<void()> changed_;
  std::function<void(std::string)> message_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, idle_;
  std::list<Ask> queue_;
  std::unordered_map<Ask, std::chrono::steady_clock::time_point> done_;  // the asks loaded, and when
  std::unordered_set<Ask> failed_;
  std::optional<Ask> loading_;  // the ask being loaded now: not yet done or failed, so want() must not queue it again
  std::vector<std::shared_ptr<HistoryProvider>> providers_;
  std::unordered_set<std::string> blocked_;  // the datasets whose provider has refused
  std::chrono::milliseconds extra_pause_{0};  // a provider's own, after its request (the loader's thread only)
  bool busy_ = false, stop_ = false;
  bool retry_ = false;  // the ask being loaded had no answer: queued again after it (under the lock)
  std::chrono::steady_clock::time_point offline_until_{};
  std::thread thread_;  // last: it starts in the constructor
};
