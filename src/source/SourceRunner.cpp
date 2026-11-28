#include "source/SourceRunner.h"

#include <algorithm>
#include <exception>

class SourceRunner::Listener : public SourceListener {
 public:
  Listener(const std::atomic<bool> & stop, std::mutex & mutex, std::condition_variable & wake, std::function<void()> changed,
           std::function<void(std::string)> message)
      : stop_(stop), mutex_(mutex), wake_(wake), changed_(std::move(changed)), message_(std::move(message)) {}
  void changed() override {
    if (changed_) changed_();
  }
  void message(const std::string & text) override {
    if (message_) message_(text);
  }
  bool cancelled() const override { return stop_; }
  // A wait on the runner's condition variable, which the destructor wakes.
  bool pause(std::chrono::milliseconds d) override {
    std::unique_lock lock(mutex_);
    wake_.wait_for(lock, d, [this] { return stop_.load(); });
    return !stop_;
  }

 private:
  const std::atomic<bool> & stop_;
  std::mutex & mutex_;
  std::condition_variable & wake_;
  std::function<void()> changed_;
  std::function<void(std::string)> message_;
};

SourceRunner::SourceRunner(std::shared_ptr<Store> store, std::vector<std::unique_ptr<Source>> sources,
                           std::function<void()> onChanged, std::function<void(std::string)> onMessage,
                           std::chrono::milliseconds realtimePeriod) {
  // What each source says of its datasets' timeliness: a row follows the fastest kind it has (Group.h).
  for (const std::unique_ptr<Source> & source : sources) {
    const char * text = source->timeliness() == Timeliness::Realtime ? "realtime" : source->timeliness() == Timeliness::Daily ? "daily" : "delayed";
    for (const std::string & code : source->datasetCodes()) {
      try {
        store->setAttribute(NodeRef(store->addDataset(code)), {predicate::timeliness, text});
      } catch (const std::exception &) {  // a code that is no dataset's: the source says so when it loads
      }
    }
  }
  // A source of its own thread each, with an Http of its own: one that is slow or waits between requests does not hold up the others.
  for (std::unique_ptr<Source> & owned : sources) {
    https_.push_back(makeHttp());
    threads_.emplace_back([this, store, source = std::move(owned), http = https_.back().get(), onChanged, onMessage, realtimePeriod]() {
      using Clock = std::chrono::steady_clock;
      Listener listener(stop_, mutex_, wake_, onChanged, onMessage);
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
          // A load that throws (a write the store refuses: data with no market) is said, and the schedule goes on: the
          // next load, the details.
          try {
            source->load(*store, *http, listener);
          } catch (const std::exception & e) {
            listener.message(source->name() + ": " + e.what());
          } catch (...) {
            listener.message(source->name() + ": failed");
          }
          const auto interval = source->refreshInterval();
          if (source->timeliness() == Timeliness::Realtime && realtimePeriod.count() > 0) {
            // The next multiple of the period on the clock: the realtime sources start together, whatever each load took.
            const auto sinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
            loadDue = Clock::now() + (realtimePeriod - sinceEpoch % realtimePeriod);
          } else {
            loadDue = interval.count() > 0 ? Clock::now() + interval : Clock::time_point::max();
          }
          if (detailsDue == Clock::time_point::max()) detailsDue = Clock::now();  // a load may have found instruments to fill in
        }
        if (!stop_ && detailsDue <= Clock::now()) {
          bool more = false;  // after a throw, the details are asked for again after the next load
          try {
            more = source->loadDetails(*store, *http, listener);
          } catch (const std::exception & e) {
            listener.message(source->name() + ": " + e.what());
          } catch (...) {
            listener.message(source->name() + ": failed");
          }
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
  for (auto & http : https_) http->cancel();  // a request in progress ends too
  wake_.notify_all();
  wait();
}
