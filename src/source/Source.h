#pragma once

#include "model/Store.h"
#include "history/HistoryProvider.h"
#include "source/Http.h"

#include <chrono>
#include <memory>
#include <string>

// What a source tells whoever runs it. Called on the source's thread.
class SourceListener {
 public:
  virtual ~SourceListener() = default;
  virtual void changed() {}                  // the store has new data
  virtual void message(const std::string &) {}  // something to show: a failure, a note
  virtual bool cancelled() const { return false; }  // true: stop soon
};

// A data adapter: reads one provider and writes what it finds into the store, as its own dataset. Blocking; the
// SourceRunner gives it a thread. The user picks which adapters run.
class Source {
 public:
  virtual ~Source() = default;
  virtual std::string name() const = 0;  // what the user types to pick it, e.g. "yahoo"
  virtual std::string description() const = 0;
  virtual void load(Store &, Http &, SourceListener &) = 0;
  // How often to load again, measured from when the last load ended; zero (the default) is once.
  virtual std::chrono::milliseconds refreshInterval() const { return std::chrono::milliseconds(0); }
  // What loads the history of this source's edges when they are wanted, if it does not load it with the rest (null: it does).
  virtual std::shared_ptr<HistoryProvider> historyProvider() { return nullptr; }
};

// Posted to the UI thread when a source has changed the store, and when it has something to say.
struct StoreChanged {};
struct SourceMessage {
  std::string text;
};
