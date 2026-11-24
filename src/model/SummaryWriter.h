#pragma once

#include "model/Summary.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Takes batches of summaries from the store's writers and hands them to the stores of their datasets on a thread of its own, so that a
// write to the store never waits for the disk. Batches are put together: what is waiting is one row per edge and one blob per (edge,
// resolution), the last said (a queue that is merged into what the file has is folded into one that is waiting for the same queue), and
// it goes after `delay`, in one call to each dataset's store. The destructor and `flush()` write everything waiting.
//
// A store is opened on first use (`store()`), by the thread that asks, and kept: the one that reads a queue gets the one that writes it.
class SummaryWriter {
 public:
  struct Options {
    std::chrono::milliseconds delay{200};
  };
  explicit SummaryWriter(std::shared_ptr<SummaryStores> stores) : SummaryWriter(std::move(stores), Options{}) {}
  SummaryWriter(std::shared_ptr<SummaryStores> stores, Options);
  ~SummaryWriter();
  SummaryWriter(const SummaryWriter &) = delete;
  SummaryWriter & operator=(const SummaryWriter &) = delete;

  // Never waits for the medium.
  void post(SummaryBatch);
  // Returns when everything posted before the call has been handed to the stores.
  void flush();
  // The codes of the datasets that have a store already.
  std::vector<std::string> datasets() { return stores_ ? stores_->datasets() : std::vector<std::string>{}; }
  // The store of a dataset (null where there is none).
  std::shared_ptr<SummaryStore> store(DatasetId);
  // What has been said of a queue and is not yet in the file: the writes in progress, then those waiting, in the order they will have been
  // applied. A reader that wants what the file has reads these first (so that what it reads is not older than what was said), then the
  // file, and applies them over it.
  std::vector<StoredQueue> pending(const EdgeKey &, Resolution) const;

  // How many batches the stores threw on (they should not).
  uint64_t failed() const;

 private:
  using Clock = std::chrono::steady_clock;
  void run();

  std::shared_ptr<SummaryStores> stores_;
  Options options_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, written_;
  std::map<EdgeKey, StoredEdge> edges_;
  std::map<SeriesKey, StoredQueue> queues_;
  std::map<EdgeKey, StoredEdge> writing_edges_;  // taken, not yet applied
  std::map<SeriesKey, StoredQueue> writing_queues_;
  Clock::time_point due_ = Clock::time_point::max();
  uint64_t posted_ = 0, done_ = 0, failed_ = 0;
  bool flushing_ = false, stop_ = false;
  std::mutex stores_mutex_;
  std::map<uint64_t, std::shared_ptr<SummaryStore>> opened_;
  std::thread thread_;
};
