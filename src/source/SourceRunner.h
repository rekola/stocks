#pragma once

#include "source/Source.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

// Runs sources one after another on a thread of its own. Its callbacks run on that thread: a program hands them to
// the UI thread (cellui's ThreadInbox). Stops and joins when destroyed.
class SourceRunner {
 public:
  SourceRunner(std::shared_ptr<Store> store, std::vector<std::unique_ptr<Source>> sources, std::function<void()> onChanged,
               std::function<void(std::string)> onMessage);
  ~SourceRunner();
  SourceRunner(const SourceRunner &) = delete;
  SourceRunner & operator=(const SourceRunner &) = delete;

  // Blocks until every source has finished (or the runner was told to stop).
  void wait();

 private:
  class Listener;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};
