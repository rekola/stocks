#pragma once

#include "model/Store.h"
#include "history/HistoryProvider.h"
#include "source/Http.h"

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// What a source tells whoever runs it. Called on the source's thread.
class SourceListener {
 public:
  virtual ~SourceListener() = default;
  virtual void changed() {}                  // the store has new data
  virtual void message(const std::string &) {}  // something to show: a failure, a note
  virtual bool cancelled() const { return false; }  // true: stop soon
  // Waits `d` between two requests; false if the source was cancelled, before or during the wait, and is to return now.
  // The default sleeps; a runner wakes it when it stops.
  virtual bool pause(std::chrono::milliseconds d) {
    if (cancelled()) return false;
    std::this_thread::sleep_for(d);
    return !cancelled();
  }
};

// A data adapter: reads one provider and writes what it finds into the store, as its own dataset. Blocking; the
// SourceRunner gives it a thread. The user picks which adapters run.
class Source {
 public:
  virtual ~Source() = default;
  virtual std::string name() const = 0;  // what the user types to pick it
  virtual std::string description() const = 0;
  // The codes of the datasets this source writes (a code is permanent, and no two sources write the same one: a source that did
  // would be putting its data under another's). Empty for a source that writes none, such as a test's.
  virtual std::vector<std::string> datasetCodes() const { return {}; }
  // Whether it is the application's whole supply of market data, so that no other source runs beside it (the market-data server's client:
  // see `suppliesAllDataClash`).
  virtual bool suppliesAllData() const { return false; }
  virtual void load(Store &, Http &, SourceListener &) = 0;
  // How often to load again, measured from when the last load ended; zero (the default) is once.
  virtual std::chrono::milliseconds refreshInterval() const { return std::chrono::milliseconds(0); }
  // What loads the history of this source's edges when they are wanted, if it does not load it with the rest (null: it does).
  virtual std::shared_ptr<HistoryProvider> historyProvider() { return nullptr; }
  // What a provider says of each instrument beyond its lists (a description, the number of shares, ratings and fees), asked
  // for one instrument at a time. Few sources have any. It fills in one batch, so that the lists are not kept waiting, and
  // says whether more remain; the runner calls it again after `detailsInterval()`, and after every load, which may have
  // found new instruments.
  virtual bool loadDetails(Store &, Http &, SourceListener &) { return false; }
  virtual std::chrono::milliseconds detailsInterval() const { return std::chrono::seconds(30); }
};

// Posted to the UI thread when a source has changed the store, and when it has something to say.
struct StoreChanged {};
struct SourceMessage {
  std::string text;
};

// What is wrong with the sources that are to run together: two of them that write the same dataset. Empty if nothing.
std::string datasetCodeClash(const std::vector<std::unique_ptr<Source>> & sources);

// THE "SERVER SUPPLIES ALL DATA" RULE, in this one place: when a source says it supplies all data (the market-data server's client), it runs
// alone, and every other source in the set is refused, whatever it writes: no provider supplies a row or a bar beside the server, for what it
// has not got either. Empty if the set is fine. Every source there is writes market data (a dataset: datasetCodes() names it), so none may run
// beside it; a source that writes none does not exist yet, and when one does it is allowed by saying so here. A later mode in which the clients
// fetch for the server relaxes this function and nothing else.
std::string suppliesAllDataClash(const std::vector<std::unique_ptr<Source>> & sources);
