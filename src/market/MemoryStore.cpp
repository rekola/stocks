#include "market/Store.h"

#include "model/NodeKey.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <mutex>

namespace market {

namespace {

class MemoryStore : public MarketStore {
 public:
  uint64_t putDataset(const std::string & code, const std::string &) override {
    const auto key = datasetKey(code);
    return key ? key->value : 0;
  }
  uint64_t beginBatch(uint64_t, const std::string &) override {
    std::lock_guard lock(mutex_);
    return ++batches_;
  }
  void putInstrument(const InstrumentRecord & record) override {
    std::lock_guard lock(mutex_);
    instruments_[record.edge] = record;
  }
  void replaceBars(const Edge & edge, Resolution level, const std::vector<Bar> & bars, Timestamp lastBarTime, uint64_t version) override {
    std::lock_guard lock(mutex_);
    bars_[{edge, level}] = bars;
    LevelState state;
    state.level = level;
    state.version = version;
    state.loadedAt = std::time(nullptr);
    state.barCount = static_cast<int64_t>(bars.size());
    if (!bars.empty()) state.firstKey = bars.front().key, state.lastKey = bars.back().key;
    state.lastBarTime = lastBarTime;
    states_[{edge, level}] = state;
  }
  std::vector<InstrumentInfo> instruments() override {
    std::lock_guard lock(mutex_);
    std::vector<InstrumentInfo> out;
    for (const auto & [edge, record] : instruments_) {
      InstrumentInfo info{record, {}};
      for (Resolution level : kLevels)
        if (auto it = states_.find({edge, level}); it != states_.end()) info.levels.push_back(it->second);
      out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const InstrumentInfo & a, const InstrumentInfo & b) { return a.instrument.code < b.instrument.code; });
    return out;
  }
  std::vector<Bar> bars(const Edge & edge, Resolution level, int64_t firstKey, int64_t lastKey) override {
    std::lock_guard lock(mutex_);
    std::vector<Bar> out;
    if (auto it = bars_.find({edge, level}); it != bars_.end())
      for (const Bar & bar : it->second)
        if (bar.key >= firstKey && bar.key <= lastKey) out.push_back(bar);
    return out;
  }
  bool healthy() override { return true; }

 private:
  std::mutex mutex_;
  uint64_t batches_ = 0;
  std::map<Edge, InstrumentRecord> instruments_;
  std::map<std::pair<Edge, Resolution>, std::vector<Bar>> bars_;
  std::map<std::pair<Edge, Resolution>, LevelState> states_;
};

}  // namespace

std::unique_ptr<MarketStore> makeMemoryStore() { return std::make_unique<MemoryStore>(); }

}  // namespace market
