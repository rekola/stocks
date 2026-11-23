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

// One request of one edge: the edge and which of the requests there are (0 the three months daily, 1 the day, 2 the week, 3 the
// year; 100 whatever the edge's dataset has a provider for).
struct HistoryAsk {
  EdgeKey edge;
  int request = 0;
  friend bool operator==(const HistoryAsk &, const HistoryAsk &) = default;
};
template <>
struct std::hash<HistoryAsk> {
  size_t operator()(const HistoryAsk & a) const { return hashCombine(std::hash<EdgeKey>()(a.edge), static_cast<size_t>(a.request)); }
};

// Loads price history for Nordnet's edges on a thread of its own, from Nordnet's price time series, for the edges that are
// wanted: a caller says which (want(): the rows on screen and a margin, most wanted first) and the loader fills them in, one
// request at a time with a pause between them, writing the bars to the store as they arrive.
//
// How much: the interactive use needs a small snapshot, the trace a row draws for the period the table shows, so by default
// (Recent) an edge is one request for that period (a request is remembered with the edge and the time it was loaded, and what the
// store has already for the period is used without one):
//
//   day      DAY_1 at MIN_5: the last session's 5-minute bars (about 100 for a stock, 10-15 KB) and the previous close
//   week     WEEK_1 at HOUR_1: about 45 hourly bars (5-8 KB)
//   month    MONTH_3 at DAY, the request of the quarter: it is shared, so a month then a quarter is one request (3-10 KB)
//   quarter  MONTH_3 at DAY: about 60 to 90 bars (3-10 KB)
//   year     YEAR_1 at WEEK: about 53 weekly bars (5-8 KB); the daily bars of a year would be five times that for a trace
//            that has 22 cells
//
// A fund has a NAV once a day and no intraday bars, so its day and week are the daily request. The whole history is for a server that
// backfills once and keeps it (Full): the whole period, and where Nordnet answers with a coarser resolution for it (a
// commodity's whole period comes weekly, back to 1997) that is stored too and the longest period that gives days is asked for
// next (the last ten years, five, three). Full exists and is tested; nothing in the interactive program uses it.
// The resolution that is stored is the one the answer says it returned, never the one asked for: Nordnet substitutes
// silently. A fund's series is its NAV (a bar with only a close).
//
// The day, week and year series fill in over time, so each kind of request has a lifetime (Options): once it is older, an edge that
// is still wanted may be fetched again. Only want() queues an expired request, so only the rows on screen and the margin are
// fetched again, never in the background, and a row scrolled away is not. The quarter's daily bars never expire: discovery
// keeps today's bar. What this adds, for a screen of 40 rows on the day: 40 requests every 15 minutes, 160 an hour (one every
// 22 seconds, about 1,400 over an 8.5-hour session), a round of 20 seconds at the 500 ms pause, 10-15 KB each. The screen
// after and the one before are wanted as well, so with them up to 120 requests (a minute at that pause) every 15 minutes. The
// week adds the same every hour and the year every day; only the period shown is asked for. A request that failed (404) or was refused is not asked again, as before.
//
//   Failures: a 404 (any other 4xx) is remembered for the session for that edge. A refusal (403, 429) stops all requests for
//   the session. No answer or a 5xx pauses requests for a minute, and the edge is asked for again afterwards.
class HistoryLoader {
 public:
  using Ask = HistoryAsk;
  enum class Depth { Recent, Full };
  struct Options {
    Depth depth = Depth::Recent;                     // the last three months, or all there is
    std::chrono::milliseconds pause{500};            // between two requests
    std::chrono::milliseconds offline_pause{60000};  // after no answer
    // How long a loaded request stays good before a wanted edge is asked for again; zero: never.
    std::chrono::milliseconds day_lifetime{15 * 60 * 1000};       // DAY_1 at MIN_5
    std::chrono::milliseconds week_lifetime{60 * 60 * 1000};      // WEEK_1 at HOUR_1
    std::chrono::milliseconds year_lifetime{24 * 60 * 60 * 1000}; // YEAR_1 at WEEK
    std::chrono::milliseconds quarter_lifetime{0};                // MONTH_3 at DAY: discovery keeps today's bar
  };
  // `changed` runs on the loader's thread after bars were written; a program hands it to the UI thread. `message` says
  // what went wrong (a refusal).
  HistoryLoader(std::shared_ptr<Store> store, std::unique_ptr<Http> http, Options options, std::function<void()> changed,
                std::function<void(std::string)> message = {});
  ~HistoryLoader();
  HistoryLoader(const HistoryLoader &) = delete;
  HistoryLoader & operator=(const HistoryLoader &) = delete;

  // What is wanted now for `period`, most wanted first. It replaces what was wanted before: edges scrolled past are not
  // loaded. An edge that is not Nordnet's, has no order book, has the period's bars already, or asked for them and failed
  // or was answered, is left out. (Full depth loads the whole history, whatever the period.)
  void want(const std::vector<EdgeKey> & edges, Period period = kDefaultPeriod);

  // Another dataset's history: the edges of the provider's dataset that are wanted are loaded by it, in the same queue (one at a time, the most
  // wanted first), once each per session (they do not expire), whatever the period. A refusal stops that provider and not the others.
  void addProvider(std::shared_ptr<HistoryProvider>);

  // Whether a refusal has stopped the network for this session.
  bool refused() const;
  // Blocks until nothing is queued or being loaded (for tests).
  void waitIdle();

  // One answer of the price time series, as the bars it holds.
  struct Series {
    Resolution resolution = Resolution::Day;  // the one the answer returned
    std::vector<Candle> bars;                 // oldest first: by slot number (intraday), else by day number (a period's first day for the coarser)
    double previousClose = kUndefined;        // the close before the session, as the answer gives it
    std::vector<std::string> periods;         // the periods it offers
    bool supported = false;                   // the resolution is one we store
  };
  // False, with `error` set, when `json` is not an answer. `nav`: the points are a fund's, with only a value.
  static bool parse(const std::string & json, bool nav, Series & out, std::string & error);

 private:
  void run();
  // Loads one edge; returns whether bars were written. `network` is set if a request was made.
  bool load(const Ask & ask, bool & network);
  // Whether a request loaded at `loaded` is too old to stand (under the lock).
  bool expired(const Ask &, std::chrono::steady_clock::time_point loaded) const;
  bool loadProvided(const Ask & ask, bool & network);
  std::shared_ptr<HistoryProvider> providerOf(const std::string & dataset) const;  // under the lock
  // One request; false if it did not give an answer (`outcome` says why).
  enum class Outcome { Ok, Missing, Refused, Offline };
  Outcome request(const std::string & book, const std::string & period, const std::string & resolution, bool nav, Series & out);

  std::shared_ptr<Store> store_;
  std::unique_ptr<Http> http_;
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
  bool busy_ = false, stop_ = false, refused_ = false;
  bool retry_ = false;  // the ask being loaded had no answer: queued again after it (under the lock)
  std::chrono::steady_clock::time_point offline_until_{};
  std::thread thread_;  // last: it starts in the constructor
};
