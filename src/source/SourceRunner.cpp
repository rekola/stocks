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
    std::vector<Clock::time_point> due(sources.size(), Clock::time_point::max());  // max: not again

    auto load = [&](size_t i) {
      sources[i]->load(*store, *http, listener);
      auto interval = sources[i]->refreshInterval();
      if (interval.count() > 0) due[i] = Clock::now() + interval;
      else due[i] = Clock::time_point::max();
    };
    for (size_t i = 0; i < sources.size(); i++) {  // each once, in order
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
      for (size_t i = 0; i < sources.size() && !stop_; i++)
        if (due[i] <= Clock::now()) load(i);
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
