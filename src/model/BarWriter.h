#pragma once

#include "model/BarStore.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

// Takes batches of bars from the store and hands them to a BarStore on a thread of its own, so that a write to the store never waits for
// the disk. What is waiting is one bar for each (series, time), the last said, and it goes in one call after `delay`. The destructor and
// `flush()` write everything waiting.
class BarWriter {
 public:
  struct Options {
    std::chrono::milliseconds delay{500};
  };
  explicit BarWriter(std::shared_ptr<BarStore> bars) : BarWriter(std::move(bars), Options{}) {}
  BarWriter(std::shared_ptr<BarStore> bars, Options);
  ~BarWriter();
  BarWriter(const BarWriter &) = delete;
  BarWriter & operator=(const BarWriter &) = delete;

  // Never waits for the medium.
  void post(BarBatch);
  // Returns when everything posted before the call has been handed to the BarStore.
  void flush();
  // How many batches the BarStore threw on.
  uint64_t failed() const;

 private:
  using Clock = std::chrono::steady_clock;
  void run();

  std::shared_ptr<BarStore> bars_;
  Options options_;
  mutable std::mutex mutex_;
  std::condition_variable wake_, written_;
  std::map<SeriesKey, std::map<int64_t, Candle>> waiting_;
  Clock::time_point due_ = Clock::time_point::max();
  uint64_t posted_ = 0, done_ = 0, failed_ = 0;
  bool flushing_ = false, stop_ = false;
  std::thread thread_;
};
