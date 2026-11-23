#pragma once

#include "model/Period.h"
#include "model/Store.h"
#include "source/Http.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

// What loads the history of the edges of one dataset: the one place that knows where that history is (its endpoints, headers, formats
// and how much of it a period needs). A source hands its own to the program (Source::historyProvider); the history loader queues the edges
// that are wanted, the ones on the screen first, paces, retries and remembers, and asks the provider for each, one at a time, on its own
// thread. The loader names no provider.
//
// What the loader asks of an edge is a request: an id the provider defines (Nordnet's day, its week; one for a fund company), the same
// id for the periods that share one. An edge and a request are asked for once per session, again only once the request has expired.
class HistoryProvider {
 public:
  enum class Outcome {
    Ok,       // bars were written (or there are none to write): the request is done
    Missing,  // the provider has nothing for this edge: it is not asked again this session
    Refused,  // the provider has refused (403, 429): it is not asked again this session, for any edge
    Offline,  // no answer: the edge is asked for again after a pause
  };
  // What an edge needs for the period on the screen.
  struct Need {
    int request = 0;      // which request gives it
    bool stored = false;  // the store has that already (loaded before, or from elsewhere): not asked for until it has expired
  };
  // Waits the loader's pause between two requests of one load; false if the loader is stopping and the load is to return.
  using Wait = std::function<bool()>;

  virtual ~HistoryProvider() = default;
  // The code of the dataset whose edges it loads.
  virtual std::string dataset() const = 0;
  // What the edge needs for `period`; none if it cannot load the edge (it has not what it needs of it).
  virtual std::optional<Need> need(const Snapshot &, const EdgeKey &, Period) const { return Need{}; }
  // How long a loaded request stays good before a wanted edge is asked for again; zero (the default): once per session.
  virtual std::chrono::milliseconds lifetime(int /*request*/) const { return std::chrono::milliseconds(0); }
  // Loads the request: writes its bars to the store, and what else it learns of the instrument.
  virtual Outcome load(Store &, Http &, const EdgeKey &, int request, const Wait &) = 0;
  // The wait after a request of its own, if longer than the loader's.
  virtual std::chrono::milliseconds pause() const { return std::chrono::milliseconds(0); }
};
