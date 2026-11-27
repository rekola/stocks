#include "model/BarWriter.h"

#include <algorithm>

BarWriter::BarWriter(std::shared_ptr<BarStore> bars, Options options) : bars_(std::move(bars)), options_(options) {
  thread_ = std::thread([this] { run(); });
}

BarWriter::~BarWriter() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void BarWriter::post(BarBatch batch) {
  if (batch.empty()) return;
  {
    std::lock_guard lock(mutex_);
    posted_++;
    for (StoredBars & series : batch.series) {
      auto & waiting = waiting_[SeriesKey{series.edge, series.resolution}];
      for (const Candle & c : series.bars) waiting[c.time] = c;
    }
    due_ = std::min(due_, Clock::now() + options_.delay);
  }
  wake_.notify_all();
}

void BarWriter::flush() {
  std::unique_lock lock(mutex_);
  const uint64_t until = posted_;
  flushing_ = true;
  wake_.notify_all();
  written_.wait(lock, [&] { return done_ >= until; });
}

uint64_t BarWriter::failed() const {
  std::lock_guard lock(mutex_);
  return failed_;
}

void BarWriter::run() {
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
    BarBatch batch;
    for (auto & [key, waiting] : waiting_) {
      StoredBars series;
      series.edge = key.edge;
      series.resolution = key.resolution;
      for (auto & entry : waiting) series.bars.push_back(entry.second);
      batch.series.push_back(std::move(series));
    }
    waiting_.clear();
    due_ = Clock::time_point::max();
    const uint64_t taken = posted_;
    lock.unlock();
    bool failed = false;
    try {
      bars_->apply(batch);
    } catch (...) {
      failed = true;  // a BarStore says what it could not do itself; one that lets an exception out must not end the thread
    }
    lock.lock();
    if (failed) failed_++;
    done_ = taken;
    if (done_ == posted_) flushing_ = false;
    written_.notify_all();
  }
}
