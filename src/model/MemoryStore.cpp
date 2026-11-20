#include "model/Store.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>

namespace {

size_t lowerIndex(const auto & v, Timestamp t) {
  auto it = std::lower_bound(v.begin(), v.end(), t, [](const auto & a, Timestamp x) { return a.time < x; });
  return static_cast<size_t>(it - v.begin());
}

// The part of `v` in `r`: shares storage with it.
template <class T>
Seq<T> slice(const Seq<T> & v, TimeRange r) {
  size_t lo = lowerIndex(v, r.from), hi = lowerIndex(v, r.to);
  return v.take(hi).drop(lo);
}

// `v` with `item` put in: in place of an element at the same time that `same` calls the same one, otherwise after the
// elements at that time. The usual case, a new last element, is a push.
template <class T, class Same>
Seq<T> upsert(const Seq<T> & v, const T & item, Same same) {
  if (v.empty() || v.back().time < item.time) return v.push_back(item);
  size_t lo = lowerIndex(v, item.time);
  size_t hi = lo;
  while (hi < v.size() && v[hi].time == item.time) {
    if (same(v[hi], item)) return v.set(hi, item);
    hi++;
  }
  return v.take(hi).push_back(item) + v.drop(hi);
}

template <class K, class V, class Same>
void appendTo(auto & map, const K & key, const std::vector<V> & items, Same same) {
  Seq<V> seq = map.count(key) ? map.at(key) : Seq<V>{};
  for (const V & i : items) seq = upsert(seq, i, same);
  map = map.set(key, seq);
}

template <class Id, class T>
std::optional<T> byId(const Seq<T> & v, Id id) {
  if (!id.valid() || id.value > v.size()) return std::nullopt;
  return v[id.value - 1];
}

template <class Id, class T>
std::optional<T> byName(const Seq<T> & v, const immer::map<std::string, Id> & index, const std::string & name) {
  const Id * id = index.find(name);
  return id ? byId(v, *id) : std::nullopt;
}

template <class T, class Id>
T addNamed(Seq<T> & v, immer::map<std::string, Id> & index, T item, const std::string & name) {
  if (const Id * id = index.find(name)) return v[id->value - 1];
  item.id.value = static_cast<uint32_t>(v.size() + 1);
  v = v.push_back(item);
  index = index.set(name, item.id);
  return item;
}

template <class K>
std::vector<K> sorted(const immer::set<K> * s) {
  std::vector<K> out;
  if (s) out.assign(s->begin(), s->end());
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

std::optional<Instrument> Snapshot::instrument(InstrumentId id) const { return byId(instruments_, id); }
std::optional<Market> Snapshot::market(MarketId id) const { return byId(markets_, id); }
std::optional<Dataset> Snapshot::dataset(DatasetId id) const { return byId(datasets_, id); }
std::optional<Instrument> Snapshot::findInstrument(const std::string & s) const {
  return byName(instruments_, instrumentBySymbol_, s);
}
std::optional<Market> Snapshot::findMarket(const std::string & s) const { return byName(markets_, marketByName_, s); }
std::optional<Dataset> Snapshot::findDataset(const std::string & s) const {
  return byName(datasets_, datasetByName_, s);
}

std::vector<EdgeKey> Snapshot::edges(MarketId m) const { return sorted(edgesByMarket_.find(m)); }
std::vector<EventKey> Snapshot::events(InstrumentId i) const { return sorted(eventsByInstrument_.find(i)); }

CandleSeries Snapshot::candles(const EdgeKey & k, TimeRange r) const {
  const Series * s = candles_.find(k);
  return s ? CandleSeries{s->intervalMs, slice(s->bars, r)} : CandleSeries{};
}
CandleSeries Snapshot::lastCandles(const EdgeKey & k, size_t n) const {
  const Series * s = candles_.find(k);
  return s ? CandleSeries{s->intervalMs, s->bars.drop(s->bars.size() - std::min(n, s->bars.size()))} : CandleSeries{};
}

Seq<BookSnapshot> Snapshot::books(const EdgeKey & k, TimeRange r) const {
  const auto * s = books_.find(k);
  return s ? slice(*s, r) : Seq<BookSnapshot>{};
}
std::optional<BookSnapshot> Snapshot::bookAt(const EdgeKey & k, Timestamp t) const {
  const auto * s = books_.find(k);
  if (!s) return std::nullopt;
  size_t after = t == std::numeric_limits<Timestamp>::max() ? s->size() : lowerIndex(*s, t + 1);
  if (after == 0) return std::nullopt;
  return (*s)[after - 1];
}

Seq<Announcement> Snapshot::announcements(const EventKey & k, TimeRange r) const {
  const auto * s = announcements_.find(k);
  return s ? slice(*s, r) : Seq<Announcement>{};
}
Seq<CorporateAction> Snapshot::actions(const EventKey & k, TimeRange r) const {
  const auto * s = actions_.find(k);
  return s ? slice(*s, r) : Seq<CorporateAction>{};
}

Instrument Snapshot::add(Instrument i) {
  size_t before = instruments_.size();
  Instrument out = addNamed(instruments_, instrumentBySymbol_, i, i.symbol);
  if (instruments_.size() != before) version_++;
  return out;
}
Market Snapshot::add(Market m) {
  size_t before = markets_.size();
  Market out = addNamed(markets_, marketByName_, m, m.name);
  if (markets_.size() != before) version_++;
  return out;
}
Dataset Snapshot::add(Dataset d) {
  size_t before = datasets_.size();
  Dataset out = addNamed(datasets_, datasetByName_, d, d.name);
  if (datasets_.size() != before) version_++;
  return out;
}

bool Snapshot::appendCandles(const EdgeKey & k, int64_t intervalMs, const std::vector<Candle> & in) {
  if (intervalMs <= 0) return false;
  if (in.empty()) return true;
  const Series * old = candles_.find(k);
  if (old && old->intervalMs != intervalMs) return false;
  Series s = old ? *old : Series{intervalMs, {}};
  for (const Candle & c : in) s.bars = upsert(s.bars, c, [](const Candle &, const Candle &) { return true; });
  candles_ = candles_.set(k, s);
  const auto * set = edgesByMarket_.find(k.market);
  edgesByMarket_ = edgesByMarket_.set(k.market, (set ? *set : immer::set<EdgeKey>{}).insert(k));
  version_++;
  return true;
}

void Snapshot::appendBooks(const EdgeKey & k, const std::vector<BookSnapshot> & in) {
  if (in.empty()) return;
  appendTo(books_, k, in, [](const BookSnapshot &, const BookSnapshot &) { return true; });
  const auto * set = edgesByMarket_.find(k.market);
  edgesByMarket_ = edgesByMarket_.set(k.market, (set ? *set : immer::set<EdgeKey>{}).insert(k));
  version_++;
}

void Snapshot::appendAnnouncements(const EventKey & k, const std::vector<Announcement> & in) {
  if (in.empty()) return;
  appendTo(announcements_, k, in,
           [](const Announcement & a, const Announcement & b) { return a.headline == b.headline; });
  const auto * set = eventsByInstrument_.find(k.instrument);
  eventsByInstrument_ = eventsByInstrument_.set(k.instrument, (set ? *set : immer::set<EventKey>{}).insert(k));
  version_++;
}

void Snapshot::appendActions(const EventKey & k, const std::vector<CorporateAction> & in) {
  if (in.empty()) return;
  appendTo(actions_, k, in, [](const CorporateAction & a, const CorporateAction & b) { return a.kind == b.kind; });
  const auto * set = eventsByInstrument_.find(k.instrument);
  eventsByInstrument_ = eventsByInstrument_.set(k.instrument, (set ? *set : immer::set<EventKey>{}).insert(k));
  version_++;
}

namespace {

// Writers take the lock one at a time and publish a new snapshot with one pointer swap; readers only load the pointer.
class MemoryStore : public Store {
 public:
  MemoryStore() : current_(std::make_shared<const Snapshot>()) {}

  std::shared_ptr<const Snapshot> snapshot() const override { return current_.load(); }

  Instrument addInstrument(Instrument i) override {
    return write([&](Snapshot & s) { return s.add(std::move(i)); });
  }
  Market addMarket(Market m) override {
    return write([&](Snapshot & s) { return s.add(std::move(m)); });
  }
  Dataset addDataset(Dataset d) override {
    return write([&](Snapshot & s) { return s.add(std::move(d)); });
  }
  bool appendCandles(const EdgeKey & k, int64_t intervalMs, const std::vector<Candle> & c) override {
    return write([&](Snapshot & s) { return s.appendCandles(k, intervalMs, c); });
  }
  void appendBooks(const EdgeKey & k, const std::vector<BookSnapshot> & b) override {
    write([&](Snapshot & s) { s.appendBooks(k, b); });
  }
  void appendAnnouncements(const EventKey & k, const std::vector<Announcement> & a) override {
    write([&](Snapshot & s) { s.appendAnnouncements(k, a); });
  }
  void appendActions(const EventKey & k, const std::vector<CorporateAction> & a) override {
    write([&](Snapshot & s) { s.appendActions(k, a); });
  }

 private:
  // Change a copy of the current snapshot and publish it if the version moved.
  template <class F>
  std::invoke_result_t<F, Snapshot &> write(F change) {
    std::lock_guard lock(writer_);
    std::shared_ptr<const Snapshot> old = current_.load();
    Snapshot next = *old;
    if constexpr (std::is_void_v<decltype(change(next))>) {
      change(next);
      if (next.version() != old->version()) current_.store(std::make_shared<const Snapshot>(std::move(next)));
    } else {
      auto result = change(next);
      if (next.version() != old->version()) current_.store(std::make_shared<const Snapshot>(std::move(next)));
      return result;
    }
  }

  std::mutex writer_;
  std::atomic<std::shared_ptr<const Snapshot>> current_;
};

}  // namespace

std::unique_ptr<Store> makeMemoryStore() { return std::make_unique<MemoryStore>(); }
