#pragma once

#include "model/Facts.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

// Takes batches of facts from the store's writers and hands them to a FactStore on a thread of its own, so that a write to
// the store never waits for the disk. Batches are put together: what is waiting is one fact per (node, predicate), the last
// said, and it goes in one call.
//
// A fact whose value changed goes after `changed_delay`. A fact only said again goes with the next batch that is due or after
// `rechecked_delay`, whichever comes first, so a source that says ten thousand known things again costs a write, not ten
// thousand. The store says a fact again only once its `updated` is older than its recheck interval (an hour), so these are rare;
// but facts said together fall due together, so when they do it is the whole table at once, which this puts in one transaction.
// The destructor and `flush()` write everything waiting.
class FactWriter {
 public:
  struct Options {
    std::chrono::milliseconds changed_delay{100};
    std::chrono::milliseconds rechecked_delay{10000};
  };
  explicit FactWriter(std::shared_ptr<FactStore> facts) : FactWriter(std::move(facts), Options{}) {}
  FactWriter(std::shared_ptr<FactStore> facts, Options);
  ~FactWriter();
  FactWriter(const FactWriter &) = delete;
  FactWriter & operator=(const FactWriter &) = delete;

  // Never waits for the medium.
  void post(FactBatch);
  // Returns when everything posted before the call has been handed to the FactStore.
  void flush();
  // How many batches the FactStore threw on (a store that cannot write says so by throwing; it is counted here, and nothing else is done).
  uint64_t failed() const;

 private:
  using Clock = std::chrono::steady_clock;
  void run();
  void note(Clock::time_point due);

  std::shared_ptr<FactStore> facts_;
  Options options_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, written_;
  // The waiting facts, one per (node, predicate).
  std::map<std::pair<uint64_t, std::string>, StoredFact> waiting_;
  Clock::time_point due_ = Clock::time_point::max();
  uint64_t posted_ = 0, done_ = 0;  // batches put in, and those handed on
  uint64_t failed_ = 0;             // batches whose FactStore threw (it should not)
  bool flushing_ = false, stop_ = false;
  std::thread thread_;
};
