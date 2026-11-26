#include "market/Store.h"

#include "model/NodeKey.h"

#include <algorithm>
#include <ctime>
#include <mutex>
#include <unordered_map>

namespace market {

namespace {

struct Slot {
  Edge edge;
  Level level;
  friend bool operator==(const Slot &, const Slot &) = default;
};
struct SlotHash {
  size_t operator()(const Slot & s) const noexcept { return EdgeHash()(s.edge) * 31 + static_cast<size_t>(s.level); }
};

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
  void removeInstrument(const Edge & edge) override {
    std::lock_guard lock(mutex_);
    instruments_.erase(edge);
    for (Level level : kLevels) bars_.erase({edge, level}), states_.erase({edge, level});
  }
  void replaceBars(const Edge & edge, Level level, const std::vector<Bar> & bars, Timestamp lastBarTime, uint64_t version) override {
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
      for (Level level : kLevels)
        if (auto it = states_.find({edge, level}); it != states_.end()) info.levels.push_back(it->second);
      out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const InstrumentInfo & a, const InstrumentInfo & b) { return a.instrument.code < b.instrument.code; });
    return out;
  }
  void forEachBar(const Edge & edge, Level level, int64_t firstKey, int64_t lastKey, const std::function<void(const Bar &)> & each) override {
    std::vector<Bar> copy;
    {
      std::lock_guard lock(mutex_);
      if (auto it = bars_.find({edge, level}); it != bars_.end())
        for (const Bar & bar : it->second)
          if (bar.key >= firstKey && bar.key <= lastKey) copy.push_back(bar);
    }
    for (const Bar & bar : copy) each(bar);
  }
  bool healthy() override { return true; }

 private:
  std::mutex mutex_;
  uint64_t batches_ = 0;
  std::unordered_map<Edge, InstrumentRecord, EdgeHash> instruments_;
  std::unordered_map<Slot, std::vector<Bar>, SlotHash> bars_;
  std::unordered_map<Slot, LevelState, SlotHash> states_;
};

}  // namespace

std::unique_ptr<MarketStore> makeMemoryStore() { return std::make_unique<MemoryStore>(); }

}  // namespace market
