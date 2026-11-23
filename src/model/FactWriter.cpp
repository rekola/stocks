#include "model/FactWriter.h"

#include <algorithm>

FactWriter::FactWriter(std::shared_ptr<FactStore> facts, Options options) : facts_(std::move(facts)), options_(options) {
  thread_ = std::thread([this] { run(); });
}

FactWriter::~FactWriter() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void FactWriter::note(Clock::time_point due) { due_ = std::min(due_, due); }

void FactWriter::post(FactBatch batch) {
  if (batch.empty()) return;
  const auto now = Clock::now();
  {
    std::lock_guard lock(mutex_);
    posted_++;
    for (StoredFact & f : batch.changed) {
      auto key = std::make_pair(f.node, f.predicate);
      waiting_[std::move(key)] = std::move(f);
      note(now + options_.changed_delay);
    }
    for (StoredFact & f : batch.rechecked) {
      auto key = std::make_pair(f.node, f.predicate);
      waiting_[std::move(key)] = std::move(f);
      note(now + options_.rechecked_delay);
    }
  }
  wake_.notify_all();
}

void FactWriter::flush() {
  std::unique_lock lock(mutex_);
  const uint64_t until = posted_;
  flushing_ = true;
  wake_.notify_all();
  written_.wait(lock, [&] { return done_ >= until; });
}

void FactWriter::run() {
  std::unique_lock lock(mutex_);
  for (;;) {
    const bool anything = !waiting_.empty();
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
    FactBatch batch;
    for (auto & entry : waiting_) batch.changed.push_back(std::move(entry.second));
    waiting_.clear();
    due_ = Clock::time_point::max();
    const uint64_t taken = posted_;
    lock.unlock();
    try {
      facts_->apply(batch);
    } catch (...) {
      // A FactStore says what it could not do itself; one that lets an exception out must not end the thread.
      lock.lock();
      failed_++;
      lock.unlock();
    }
    lock.lock();
    done_ = taken;
    if (done_ == posted_) flushing_ = false;
    written_.notify_all();
  }
}
