#pragma once

#include "model/Store.h"
#include "source/Http.h"

#include <chrono>
#include <string>

// What loads the history of the edges of one dataset, where the loader's own provider (Nordnet's price series) is not it. A source that
// has the history of what it found, but not at the time it found it, hands one to the program (Source::historyProvider); the history loader
// asks it for the edges that are wanted, the ones on the screen first. It runs on the loader's thread, one edge at a time.
class HistoryProvider {
 public:
  enum class Outcome {
    Ok,       // bars were written (or there are none to write): the edge is done
    Missing,  // the provider has nothing for this edge: it is not asked again this session
    Refused,  // the provider has refused (403, 429): it is not asked again this session, for any edge
    Offline,  // no answer: the edge is asked for again after a pause
  };
  virtual ~HistoryProvider() = default;
  // The name of the dataset whose edges it loads.
  virtual std::string dataset() const = 0;
  // Whether it can load this edge (it has what it needs of it).
  virtual bool wants(const Snapshot &, const EdgeKey &) const { return true; }
  // Loads the edge: writes its bars to the store, and what else it learns of the fund.
  virtual Outcome load(Store &, Http &, const EdgeKey &) = 0;
  // The wait after a request of its own, if longer than the loader's.
  virtual std::chrono::milliseconds pause() const { return std::chrono::milliseconds(0); }
};
