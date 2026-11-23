#include "source/SourceRunner.h"

#include <algorithm>

class SourceRunner::Listener : public SourceListener {
 public:
  Listener(const std::atomic<bool> & stop, std::function<void()> changed, std::function<void(std::string)> message)
      : stop_(stop), changed_(std::move(changed)), message_(std::move(message)) {}
  void changed() override {
    if (changed_) changed_();
  }
  void message(const std::string & text) override {
    if (message_) message_(text);
  }
  bool cancelled() const override { return stop_; }

 private:
  const std::atomic<bool> & stop_;
  std::function<void()> changed_;
  std::function<void(std::string)> message_;
};

SourceRunner::SourceRunner(std::shared_ptr<Store> store, std::vector<std::unique_ptr<Source>> sources,
                           std::function<void()> onChanged, std::function<void(std::string)> onMessage) {
  // A source of its own thread each, with an Http of its own: one that is slow or waits between requests does not hold up the others.
  for (std::unique_ptr<Source> & owned : sources) {
    threads_.emplace_back([this, store, source = std::move(owned), onChanged, onMessage]() {
      using Clock = std::chrono::steady_clock;
      Listener listener(stop_, onChanged, onMessage);
      std::unique_ptr<Http> http = makeHttp();
      // When the source is loaded again, and when it is asked for another batch of its details; max: not again.
      Clock::time_point loadDue = Clock::now(), detailsDue = Clock::time_point::max();
      std::unique_lock lock(mutex_);
      while (!stop_) {
        const Clock::time_point next = std::min(loadDue, detailsDue);
        if (next == Clock::time_point::max()) return;  // nothing is refreshed: done
        wake_.wait_until(lock, next, [this] { return stop_.load(); });
        if (stop_) return;
        lock.unlock();
        if (loadDue <= Clock::now()) {
          source->load(*store, *http, listener);
          const auto interval = source->refreshInterval();
          loadDue = interval.count() > 0 ? Clock::now() + interval : Clock::time_point::max();
          if (detailsDue == Clock::time_point::max()) detailsDue = Clock::now();  // a load may have found instruments to fill in
        }
        if (!stop_ && detailsDue <= Clock::now()) {
          const bool more = source->loadDetails(*store, *http, listener);
          detailsDue = more ? Clock::now() + source->detailsInterval() : Clock::time_point::max();
        }
        lock.lock();
      }
    });
  }
}

void SourceRunner::wait() {
  for (std::thread & t : threads_)
    if (t.joinable()) t.join();
}

SourceRunner::~SourceRunner() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  wait();
}
