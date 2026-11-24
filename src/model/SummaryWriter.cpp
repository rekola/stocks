#include "model/SummaryWriter.h"

#include "model/SummaryRecords.h"

#include <algorithm>

SummaryWriter::SummaryWriter(std::shared_ptr<SummaryStores> stores, Options options) : stores_(std::move(stores)), options_(options) {
  thread_ = std::thread([this] { run(); });
}

SummaryWriter::~SummaryWriter() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

std::shared_ptr<SummaryStore> SummaryWriter::store(DatasetId dataset) {
  if (!stores_ || !dataset.valid()) return nullptr;
  std::lock_guard lock(stores_mutex_);
  auto found = opened_.find(dataset.value);
  if (found != opened_.end()) return found->second;
  std::shared_ptr<SummaryStore> s;
  try {
    s = stores_->open(datasetCode(dataset));
  } catch (...) {
  }
  opened_[dataset.value] = s;
  return s;
}

void SummaryWriter::post(SummaryBatch batch) {
  if (batch.empty()) return;
  {
    std::lock_guard lock(mutex_);
    posted_++;
    for (StoredEdge & e : batch.edges) {
      const EdgeKey key = e.listing.key;
      edges_[key] = std::move(e);
    }
    for (StoredQueue & q : batch.queues) {
      const SeriesKey key{q.edge, q.resolution};
      auto it = queues_.find(key);
      if (it == queues_.end()) queues_.emplace(key, std::move(q));
      else summary::fold(it->second, std::move(q));
    }
    due_ = std::min(due_, Clock::now() + options_.delay);
  }
  wake_.notify_all();
}

void SummaryWriter::flush() {
  std::unique_lock lock(mutex_);
  const uint64_t until = posted_;
  flushing_ = true;
  wake_.notify_all();
  written_.wait(lock, [&] { return done_ >= until; });
}

std::vector<StoredQueue> SummaryWriter::pending(const EdgeKey & edge, Resolution resolution) const {
  std::lock_guard lock(mutex_);
  std::vector<StoredQueue> out;
  const SeriesKey key{edge, resolution};
  if (auto it = writing_queues_.find(key); it != writing_queues_.end()) out.push_back(it->second);
  if (auto it = queues_.find(key); it != queues_.end()) out.push_back(it->second);
  return out;
}

uint64_t SummaryWriter::failed() const {
  std::lock_guard lock(mutex_);
  return failed_;
}

void SummaryWriter::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    const bool anything = !edges_.empty() || !queues_.empty();
    if (stop_ && !anything) return;
    if (!anything) {
      done_ = posted_;
      flushing_ = false;
      written_.notify_all();
      wake_.wait(lock, [&] { return stop_ || posted_ != done_; });
      continue;
    }
    if (!stop_ && !flushing_ && Clock::now() < due_) {
      wake_.wait_until(lock, due_);
      continue;
    }
    writing_edges_ = std::move(edges_);
    writing_queues_ = std::move(queues_);
    edges_.clear();
    queues_.clear();
    due_ = Clock::time_point::max();
    const uint64_t taken = posted_;
    lock.unlock();
    // One call to the store of each dataset.
    std::map<uint64_t, SummaryBatch> by_dataset;
    for (auto & entry : writing_edges_) by_dataset[entry.first.dataset.value].edges.push_back(entry.second);
    for (auto & entry : writing_queues_) by_dataset[entry.first.edge.dataset.value].queues.push_back(entry.second);
    uint64_t failures = 0;
    for (auto & [dataset, batch] : by_dataset) {
      try {
        if (auto s = store(DatasetId{dataset})) s->apply(batch);
      } catch (...) {
        // A store says what it could not do itself; one that lets an exception out must not end the thread.
        failures++;
      }
    }
    lock.lock();
    failed_ += failures;
    writing_edges_.clear();
    writing_queues_.clear();
    done_ = taken;
    if (done_ == posted_) flushing_ = false;
    written_.notify_all();
  }
}
