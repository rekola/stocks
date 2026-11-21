#include "model/Store.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <functional>
#include <mutex>

namespace {

// The index of the first element not before `t`: a Timestamp, a day number or a floating-point time, as the elements
// are keyed.
size_t lowerIndex(const auto & v, auto t) {
  auto it = std::lower_bound(v.begin(), v.end(), t, [](const auto & a, auto x) { return a.time < x; });
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
std::optional<Instrument> Snapshot::findInstrumentByIsin(const std::string & isin) const {
  return byName(instruments_, instrumentByIsin_, isin);
}
std::optional<Market> Snapshot::findMarket(const std::string & s) const {
  return findMarketByAttribute(predicate::name, s);
}
std::optional<Market> Snapshot::findMarketByAttribute(const std::string & p, const std::string & v) const {
  return byName(markets_, marketByAttribute_, p + '\0' + v);
}
std::optional<Dataset> Snapshot::findDataset(const std::string & s) const {
  return byName(datasets_, datasetByName_, s);
}

std::vector<EdgeKey> Snapshot::edges(MarketId m) const { return sorted(edgesByMarket_.find(m)); }
std::vector<EdgeKey> Snapshot::edges(MarketId m, const std::function<bool(const Instrument &)> & keep) const {
  std::vector<EdgeKey> out;
  for (const EdgeKey & k : edges(m)) {
    auto base = instrument(k.base);
    if (base && keep(*base)) out.push_back(k);
  }
  return out;
}
std::vector<EventKey> Snapshot::events(InstrumentId i) const { return sorted(eventsByInstrument_.find(i)); }

CandleSeries Snapshot::candles(const EdgeKey & k, Resolution r, TimeRange range) const {
  const auto * bars = series_.find({k, r});
  return {r, bars ? slice(*bars, range) : Seq<Candle>{}};
}
CandleSeries Snapshot::lastCandles(const EdgeKey & k, Resolution r, size_t n) const {
  const auto * bars = series_.find({k, r});
  return {r, bars ? bars->drop(bars->size() - std::min(n, bars->size())) : Seq<Candle>{}};
}
std::vector<Resolution> Snapshot::resolutions(const EdgeKey & k) const {
  std::vector<int> codes;
  if (const auto * set = resolutions_.find(k)) codes.assign(set->begin(), set->end());
  std::sort(codes.begin(), codes.end());
  std::vector<Resolution> out;
  for (int c : codes) out.push_back(static_cast<Resolution>(c));
  return out;
}

Seq<BookSnapshot> Snapshot::books(const EdgeKey & k, TimeRange r) const {
  const auto * s = books_.find(k);
  return s ? slice(*s, r) : Seq<BookSnapshot>{};
}
std::optional<BookSnapshot> Snapshot::bookAt(const EdgeKey & k, double t) const {
  const auto * s = books_.find(k);
  if (!s) return std::nullopt;
  // The first snapshot after `t`, and the one before it.
  auto after = std::upper_bound(s->begin(), s->end(), t, [](double x, const BookSnapshot & b) { return x < b.time; });
  if (after == s->begin()) return std::nullopt;
  return *(after - 1);
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
  // An instrument with an ISIN is that ISIN; one without is its symbol.
  const InstrumentId * known = i.isin.empty() ? instrumentBySymbol_.find(i.symbol) : instrumentByIsin_.find(i.isin);
  if (known) return instruments_[known->value - 1];
  i.id.value = static_cast<uint32_t>(instruments_.size() + 1);
  instruments_ = instruments_.push_back(i);
  if (!instrumentBySymbol_.find(i.symbol)) instrumentBySymbol_ = instrumentBySymbol_.set(i.symbol, i.id);
  if (!i.isin.empty()) instrumentByIsin_ = instrumentByIsin_.set(i.isin, i.id);
  version_++;
  return i;
}
void Snapshot::setAttribute(MarketId id, const Attribute & a) {
  if (!id.valid() || id.value > markets_.size() || a.value.empty()) return;
  const std::string key = a.predicate + '\0' + a.value;
  if (const MarketId * holder = marketByAttribute_.find(key)) {
    if (*holder != id) return;  // another market says it
  }
  Market m = markets_[id.value - 1];
  auto old = std::find_if(m.attributes.begin(), m.attributes.end(), [&](const Attribute & x) { return x.predicate == a.predicate; });
  if (old != m.attributes.end()) {
    if (old->value == a.value) return;
    marketByAttribute_ = marketByAttribute_.erase(a.predicate + '\0' + old->value);
    old->value = a.value;
  } else {
    m.attributes.push_back(a);
  }
  if (a.predicate == predicate::name) m.name = a.value;
  markets_ = markets_.set(id.value - 1, m);
  marketByAttribute_ = marketByAttribute_.set(key, id);
  version_++;
}
Market Snapshot::resolve(const std::vector<Attribute> & attributes) {
  for (const Attribute & a : attributes)
    if (auto found = findMarketByAttribute(a.predicate, a.value)) {
      for (const Attribute & b : attributes) {
        bool has = false;
        for (const Attribute & x : found->attributes) has = has || x.predicate == b.predicate;
        if (!has) setAttribute(found->id, b);
      }
      return *market(found->id);
    }
  Market m;
  m.id.value = static_cast<uint32_t>(markets_.size() + 1);
  markets_ = markets_.push_back(m);
  version_++;
  for (const Attribute & a : attributes) setAttribute(m.id, a);
  return *market(m.id);
}
Dataset Snapshot::add(Dataset d) {
  size_t before = datasets_.size();
  Dataset out = addNamed(datasets_, datasetByName_, d, d.name);
  if (datasets_.size() != before) version_++;
  return out;
}

void Snapshot::appendCandles(const EdgeKey & k, Resolution r, const std::vector<Candle> & in) {
  if (in.empty()) return;
  const SeriesKey key{k, r};
  const auto * old = series_.find(key);
  Seq<Candle> bars = old ? *old : Seq<Candle>{};
  for (const Candle & c : in) bars = upsert(bars, c, [](const Candle &, const Candle &) { return true; });
  series_ = series_.set(key, bars);
  const auto * known = resolutions_.find(k);
  resolutions_ = resolutions_.set(k, (known ? *known : immer::set<int>{}).insert(static_cast<int>(r)));
  const auto * set = edgesByMarket_.find(k.market);
  edgesByMarket_ = edgesByMarket_.set(k.market, (set ? *set : immer::set<EdgeKey>{}).insert(k));
  version_++;
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
// The current snapshot, read and replaced atomically: std::atomic<std::shared_ptr> where the
// library has it (C++20), else the shared_ptr atomic functions it replaced (Apple's libc++ has
// only those).
class SnapshotSlot {
 public:
  explicit SnapshotSlot(std::shared_ptr<const Snapshot> s) : p_(std::move(s)) {}
#if defined(__cpp_lib_atomic_shared_ptr)
  std::shared_ptr<const Snapshot> load() const { return p_.load(); }
  void store(std::shared_ptr<const Snapshot> s) { p_.store(std::move(s)); }

 private:
  std::atomic<std::shared_ptr<const Snapshot>> p_;
#else
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
  std::shared_ptr<const Snapshot> load() const { return std::atomic_load(&p_); }
  void store(std::shared_ptr<const Snapshot> s) { std::atomic_store(&p_, std::move(s)); }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

 private:
  std::shared_ptr<const Snapshot> p_;
#endif
};

class MemoryStore : public Store {
 public:
  MemoryStore() : current_(std::make_shared<const Snapshot>()) {}

  std::shared_ptr<const Snapshot> snapshot() const override { return current_.load(); }

  Instrument addInstrument(Instrument i) override {
    return write([&](Snapshot & s) { return s.add(std::move(i)); });
  }
  Market resolveMarket(const std::vector<Attribute> & a) override {
    return write([&](Snapshot & s) { return s.resolve(a); });
  }
  void setMarketAttribute(MarketId id, const Attribute & a) override {
    write([&](Snapshot & s) { s.setAttribute(id, a); });
  }
  Dataset addDataset(Dataset d) override {
    return write([&](Snapshot & s) { return s.add(std::move(d)); });
  }
  void appendCandles(const EdgeKey & k, Resolution r, const std::vector<Candle> & c) override {
    write([&](Snapshot & s) { s.appendCandles(k, r, c); });
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
  SnapshotSlot current_;
};

}  // namespace

std::unique_ptr<Store> makeMemoryStore() { return std::make_unique<MemoryStore>(); }
