#pragma once

#include "source/Source.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

// Runs sources one after another on a thread of its own, and then again whenever one of them asks to be refreshed (its
// refreshInterval, from the end of its last load). Its callbacks run on that thread: a program hands them to the UI
// thread (cellui's ThreadInbox). The runner says each source's `timeliness()` as a fact of its datasets. The sources that are realtime are loaded again
// all at once, every `realtimePeriod` on the clock (at the multiples of it since the epoch, so that what they say is of the same moment and they
// can be compared), and not by their own intervals. Stops, even in the middle of a load or a request, and joins when destroyed.
class SourceRunner {
 public:
  SourceRunner(std::shared_ptr<Store> store, std::vector<std::unique_ptr<Source>> sources, std::function<void()> onChanged,
               std::function<void(std::string)> onMessage, std::chrono::milliseconds realtimePeriod = std::chrono::seconds(20));
  ~SourceRunner();
  SourceRunner(const SourceRunner &) = delete;
  SourceRunner & operator=(const SourceRunner &) = delete;

  // Blocks until every source has finished: which is never while one of them refreshes, so then only the destructor
  // ends it.
  void wait();

 private:
  class Listener;
  std::atomic<bool> stop_{false};
  std::vector<std::unique_ptr<Http>> https_;  // one a source, cancelled by the destructor
  std::mutex mutex_;
  std::condition_variable wake_;
  std::vector<std::thread> threads_;
};
