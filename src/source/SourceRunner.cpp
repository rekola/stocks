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
  thread_ = std::thread([this, store = std::move(store), sources = std::move(sources), onChanged = std::move(onChanged),
                         onMessage = std::move(onMessage)]() mutable {
    using Clock = std::chrono::steady_clock;
    Listener listener(stop_, std::move(onChanged), std::move(onMessage));
    std::unique_ptr<Http> http = makeHttp();
    const size_t n = sources.size();
    // When each source is loaded again, then when each is asked for another batch of its details; max: not again.
    std::vector<Clock::time_point> due(2 * n, Clock::time_point::max());
    auto load = [&](size_t i) {
      sources[i]->load(*store, *http, listener);
      auto interval = sources[i]->refreshInterval();
      if (interval.count() > 0) due[i] = Clock::now() + interval;
      else due[i] = Clock::time_point::max();
      if (due[n + i] == Clock::time_point::max()) due[n + i] = Clock::now();  // a load may have found instruments to fill in
    };
    auto details = [&](size_t i) {
      bool more = sources[i]->loadDetails(*store, *http, listener);
      due[n + i] = more ? Clock::now() + sources[i]->detailsInterval() : Clock::time_point::max();
    };
    for (size_t i = 0; i < n; i++) {  // each once, in order
      if (stop_) return;
      load(i);
    }
    std::unique_lock lock(mutex_);
    while (!stop_) {
      Clock::time_point next = *std::min_element(due.begin(), due.end());
      if (next == Clock::time_point::max()) return;  // nothing is refreshed: done
      wake_.wait_until(lock, next, [this] { return stop_.load(); });
      if (stop_) return;
      lock.unlock();
      for (size_t i = 0; i < n && !stop_; i++) {
        if (due[i] <= Clock::now()) load(i);
        if (!stop_ && due[n + i] <= Clock::now()) details(i);
      }
      lock.lock();
    }
  });
}

void SourceRunner::wait() {
  if (thread_.joinable()) thread_.join();
}

SourceRunner::~SourceRunner() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  wait();
}
