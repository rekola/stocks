#include "history/HistoryLoader.h"

#include <algorithm>
#include <atomic>

// The loader's Http as a provider sees it: every request goes through to it, and is counted, so that the loader knows whether a load
// made one (and pauses after it) without a provider having to say so. Cancelling is the loader's own, on the Http behind, which every
// request reaches: this one's own flag is never set or read.
class CountedHttp : public Http {
 public:
  explicit CountedHttp(Http & inner) : inner_(inner) {}
  HttpResult get(const std::string & url, const HttpHeaders & headers) override {
    requests_++;
    return inner_.get(url, headers);
  }
  int requests() const { return requests_; }

 private:
  Http & inner_;
  std::atomic<int> requests_{0};
};

HistoryLoader::HistoryLoader(std::shared_ptr<Store> store, std::unique_ptr<Http> http, Options options, std::function<void()> changed,
                             std::function<void(std::string)> message)
    : store_(std::move(store)), http_(std::move(http)), counted_(http_ ? std::make_unique<CountedHttp>(*http_) : nullptr), options_(options), changed_(std::move(changed)), message_(std::move(message)) {
  thread_ = std::thread([this] { run(); });
}

HistoryLoader::~HistoryLoader() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  if (http_) http_->cancel();  // a request in progress ends too
  wake_.notify_all();
  if (thread_.joinable()) thread_.join();
}

bool HistoryLoader::refused() const {
  std::lock_guard lock(mutex_);
  return !blocked_.empty();
}

void HistoryLoader::waitIdle() {
  std::unique_lock lock(mutex_);
  idle_.wait(lock, [&] { return queue_.empty() && !busy_; });
}

void HistoryLoader::addProvider(std::shared_ptr<HistoryProvider> provider) {
  if (!provider) return;
  // What the summary keeps of each queue of its dataset's edges is the provider's to say.
  for (const std::string & code : provider->datasets())
    if (const auto dataset = datasetKey(code))
      for (int r = 0; r <= static_cast<int>(Resolution::Quarter); r++)
        store_->setRetention(DatasetId{dataset->value}, static_cast<Resolution>(r), provider->keep(static_cast<Resolution>(r)));
  std::lock_guard lock(mutex_);
  providers_.push_back(std::move(provider));
}

std::shared_ptr<HistoryProvider> HistoryLoader::providerOf(const std::string & dataset) const {
  // By key, not by text: the case of a code is not kept (NodeKey.h), so a provider that says "other" serves the dataset the store calls OTHER.
  const auto key = datasetKey(dataset);
  if (!key) return nullptr;
  for (const auto & p : providers_)
    for (const std::string & code : p->datasets())
      if (datasetKey(code) == key) return p;
  return nullptr;
}

bool HistoryLoader::expired(const Ask & ask, std::chrono::steady_clock::time_point loaded) const {
  auto provider = providerOf(datasetCode(ask.edge.dataset));
  const std::chrono::milliseconds lifetime = provider ? provider->lifetime(ask.request) : std::chrono::milliseconds(0);
  return lifetime.count() > 0 && std::chrono::steady_clock::now() - loaded >= lifetime;
}

bool HistoryLoader::stale(const Ask & ask, TimestampMs fetched) const {
  auto provider = providerOf(datasetCode(ask.edge.dataset));
  const std::chrono::milliseconds lifetime = provider ? provider->lifetime(ask.request) : std::chrono::milliseconds(0);
  if (lifetime.count() <= 0 || fetched <= 0) return false;
  const TimestampMs now = options_.clock ? options_.clock()
                                         : std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  return now - fetched >= lifetime.count();
}

void HistoryLoader::want(const std::vector<EdgeKey> & edges, Period period) {
  // The queues the rows need are read from the disk, if they are not in memory (a thread of the store's own; nothing here waits for it).
  {
    std::vector<SeriesKey> series;
    const std::vector<Resolution> reads = readsOf(period);
    series.reserve(edges.size() * reads.size());
    for (const EdgeKey & k : edges)
      for (Resolution r : reads) series.push_back(SeriesKey{k, r});
    store_->want(series);
  }
  auto snapshot = store_->snapshot();
  {
    std::lock_guard lock(mutex_);
    queue_.clear();
    std::unordered_set<Ask> seen;
    for (const EdgeKey & k : edges) {
      const std::string dataset = datasetCode(k.dataset);  // a dataset is looked up by its code: its name is a fact that can change
      if (dataset.empty() || blocked_.count(dataset)) continue;
      auto provider = providerOf(dataset);
      if (!provider) continue;
      auto need = provider->need(*snapshot, k, period);
      if (!need) continue;
      const Ask ask{k, need->request};
      if (failed_.count(ask) || loading_ == ask || !seen.insert(ask).second) continue;
      // Loaded before: not again until it has expired (then the store's bars are the old ones, so they do not count).
      const auto loaded = done_.find(ask);
      if (loaded != done_.end() && !expired(ask, loaded->second)) continue;
      // What the store has (in memory, or in the summary on the disk) for it, unless it was fetched longer ago than the provider says it stays good.
      if (loaded == done_.end() && need->stored && !stale(ask, need->fetched)) continue;
      queue_.push_back(ask);
    }
  }
  wake_.notify_all();
}

void HistoryLoader::run() {
  std::unique_lock lock(mutex_);
  while (true) {
    wake_.wait(lock, [&] { return stop_ || !queue_.empty(); });
    if (stop_) return;
    if (std::chrono::steady_clock::now() < offline_until_) {
      // No answer lately: wait it out (a destructor cuts it short).
      wake_.wait_until(lock, offline_until_, [&] { return stop_; });
      continue;
    }
    const Ask ask = queue_.front();
    queue_.pop_front();
    // Queued before its answer came: not asked again. A done one that has expired was queued by want() for that: it is loaded.
    const auto loaded = done_.find(ask);
    if (failed_.count(ask) || (loaded != done_.end() && !expired(ask, loaded->second))) {
      if (queue_.empty()) idle_.notify_all();
      continue;
    }
    busy_ = true;
    loading_ = ask;  // want() leaves it out until it is done or failed
    lock.unlock();
    bool network = false;
    bool wrote = false;
    std::string thrown;
    try {
      wrote = load(ask, network);
    } catch (const std::exception & e) {
      thrown = e.what();
    } catch (...) {
      thrown = "failed";
    }
    if (!thrown.empty()) {
      // Counted as missing: not asked for again this session, and the queue goes on.
      if (message_) message_("history: " + thrown);
      std::lock_guard failed(mutex_);
      failed_.insert(ask);
    }
    if (wrote && changed_) changed_();
    lock.lock();
    loading_.reset();
    if (retry_) {
      // No answer: first in the queue, for after the pause. Here and not in load(), so that a want() meanwhile, which left it
      // out as the one being loaded, does not drop it; and only once.
      retry_ = false;
      std::erase(queue_, ask);
      queue_.push_front(ask);
    }
    busy_ = false;
    if (network) {
      const auto pause = std::max(options_.pause, extra_pause_);
      if (pause.count() > 0) wake_.wait_for(lock, pause, [&] { return stop_; });
    }
    if (queue_.empty()) idle_.notify_all();
  }
}

bool HistoryLoader::load(const Ask & wanted, bool & network) {
  extra_pause_ = std::chrono::milliseconds(0);  // a provider's own is set again by its asks
  auto snapshot = store_->snapshot();
  const std::string dataset = datasetCode(wanted.edge.dataset);
  std::shared_ptr<HistoryProvider> provider;
  {
    std::lock_guard lock(mutex_);
    if (!dataset.empty()) provider = providerOf(dataset);
  }
  if (!provider) return false;
  const int before = counted_->requests();
  // Between two requests of one load: the loader's pause, cut short when it stops.
  const HistoryProvider::Wait wait = [this] {
    std::unique_lock lock(mutex_);
    if (options_.pause.count() > 0) wake_.wait_for(lock, options_.pause, [&] { return stop_; });
    return !stop_;
  };
  HistoryProvider::Outcome outcome;
  try {
    outcome = provider->load(*store_, *counted_, wanted.edge, wanted.request, wait);
  } catch (const std::exception & e) {  // as missing: the edge is not asked for again
    if (message_) message_("history: " + snapshot->name(wanted.edge.dataset) + ": " + e.what());
    outcome = HistoryProvider::Outcome::Missing;
  } catch (...) {
    if (message_) message_("history: " + snapshot->name(wanted.edge.dataset) + ": failed");
    outcome = HistoryProvider::Outcome::Missing;
  }
  // Pause after a load that made a request, and only then: one that returned at once (nothing to ask) is followed by the next.
  network = counted_->requests() > before;
  if (network) extra_pause_ = provider->pause();
  switch (outcome) {
    case HistoryProvider::Outcome::Refused: {
      {
        std::lock_guard lock(mutex_);
        blocked_.insert(dataset);
        // Its edges still in the queue are not asked for.
        std::erase_if(queue_, [&](const Ask & a) { return datasetCode(a.edge.dataset) == dataset; });
      }
      if (message_) message_("history: " + snapshot->name(wanted.edge.dataset) + " refused the request, stopping it");
      idle_.notify_all();
      return false;
    }
    case HistoryProvider::Outcome::Offline: {
      std::lock_guard lock(mutex_);
      offline_until_ = std::chrono::steady_clock::now() + options_.offline_pause;
      retry_ = true;  // again afterwards
      return false;
    }
    case HistoryProvider::Outcome::Missing: {
      std::lock_guard lock(mutex_);
      failed_.insert(wanted);
      return false;
    }
    case HistoryProvider::Outcome::Ok: break;
  }
  std::lock_guard lock(mutex_);
  done_[wanted] = std::chrono::steady_clock::now();
  return true;
}
