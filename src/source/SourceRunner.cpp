#include "source/SourceRunner.h"

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
    Listener listener(stop_, std::move(onChanged), std::move(onMessage));
    std::unique_ptr<Http> http = makeHttp();
    for (auto & source : sources) {
      if (stop_) return;
      source->load(*store, *http, listener);
    }
  });
}

void SourceRunner::wait() {
  if (thread_.joinable()) thread_.join();
}

SourceRunner::~SourceRunner() {
  stop_ = true;
  wait();
}
