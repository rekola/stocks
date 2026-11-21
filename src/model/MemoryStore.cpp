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
  auto n = findByAttribute(NodeType::Instrument, predicate::symbol, s);
  return n ? instrument({n->id}) : std::nullopt;
}
std::optional<Listing> Snapshot::listing(const EdgeKey & k) const {
  const Listing * l = listings_.find(k);
  return l ? std::optional<Listing>(*l) : std::nullopt;
}
std::string Snapshot::symbol(const EdgeKey & k) const {
  if (const Listing * l = listings_.find(k); l && !l->symbol.empty()) return l->symbol;
  auto i = instrument(k.base);
  return i ? i->symbol : std::string();
}
namespace {
std::string symbolKey(MarketId m, const std::string & symbol) { return std::to_string(m.value) + '\0' + symbol; }
}  // namespace
std::optional<EdgeKey> Snapshot::findEdge(const std::string & symbol, MarketId m) const {
  const EdgeKey * k = edgeBySymbol_.find(symbolKey(m, symbol));
  return k ? std::optional<EdgeKey>(*k) : std::nullopt;
}
#define STOCKS_FACTS(NAME, MEMBER, TYPE)                             \
  std::optional<TYPE> Snapshot::NAME(const FactKey & k) const {      \
    const TYPE * f = MEMBER.find(k);                                 \
    return f ? std::optional<TYPE>(*f) : std::nullopt;               \
  }
STOCKS_FACTS(instrumentFacts, instrumentFacts_, InstrumentFacts)
STOCKS_FACTS(companyFacts, companyFacts_, CompanyFacts)
STOCKS_FACTS(fundFacts, fundFacts_, FundFacts)
STOCKS_FACTS(certificateFacts, certificateFacts_, CertificateFacts)
#undef STOCKS_FACTS
std::optional<Instrument> Snapshot::findInstrumentByIsin(const std::string & isin) const {
  auto n = findByAttribute(NodeType::Instrument, predicate::isin, isin);
  return n ? instrument({n->id}) : std::nullopt;
}
std::optional<Market> Snapshot::findMarket(const std::string & s) const {
  return findMarketByAttribute(predicate::name, s);
}
std::optional<Market> Snapshot::findMarketByAttribute(const std::string & p, const std::string & v) const {
  auto n = findByAttribute(NodeType::Market, p, v);
  return n ? market({n->id}) : std::nullopt;
}
std::vector<Attribute> Snapshot::attributes(NodeRef n) const {
  const auto * a = attributes_.find(n);
  return a ? *a : std::vector<Attribute>{};
}
namespace {
std::string indexKey(NodeType t, const std::string & p, const std::string & v) {
  return std::string(1, static_cast<char>('0' + static_cast<int>(t))) + p + '\0' + v;
}
std::optional<NodeRef> lowest(const immer::set<uint32_t> * ids, NodeType t) {
  if (!ids || ids->size() == 0) return std::nullopt;
  uint32_t first = std::numeric_limits<uint32_t>::max();
  for (uint32_t id : *ids) first = std::min(first, id);
  NodeRef n;
  n.type = t;
  n.id = first;
  return n;
}
}  // namespace

std::optional<NodeRef> Snapshot::findByAttribute(NodeType t, const std::string & p, const std::string & v) const {
  return lowest(byAttribute_.find(indexKey(t, p, v)), t);
}
std::optional<Dataset> Snapshot::findDataset(const std::string & s) const {
  auto n = findByAttribute(NodeType::Dataset, predicate::name, s);
  return n ? dataset({n->id}) : std::nullopt;
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

void Snapshot::setAttribute(NodeRef n, const Attribute & a) {
  const size_t count = n.type == NodeType::Instrument ? instruments_.size()
                       : n.type == NodeType::Market   ? markets_.size()
                                                      : datasets_.size();
  if (n.id == 0 || n.id > count || a.value.empty()) return;
  std::vector<Attribute> attrs = attributes(n);
  auto at = std::find_if(attrs.begin(), attrs.end(), [&](const Attribute & x) { return x.predicate == a.predicate; });
  auto drop = [&](const std::string & key) {
    if (const auto * ids = byAttribute_.find(key)) {
      auto rest = ids->erase(n.id);
      byAttribute_ = rest.size() ? byAttribute_.set(key, rest) : byAttribute_.erase(key);
    }
  };
  if (at != attrs.end()) {
    if (at->value == a.value) return;
    const std::string old = at->value;
    at->value = a.value;
    drop(indexKey(n.type, a.predicate, old));  // the old value no longer says it of this node
  } else {
    attrs.push_back(a);
  }
  attributes_ = attributes_.set(n, attrs);
  {
    const std::string key = indexKey(n.type, a.predicate, a.value);
    const auto * ids = byAttribute_.find(key);
    byAttribute_ = byAttribute_.set(key, (ids ? *ids : immer::set<uint32_t>{}).insert(n.id));
  }

  // The view the app reads is made from the attributes.
  auto value = [&](const char * p) {
    for (const Attribute & x : attrs)
      if (x.predicate == p) return x.value;
    return std::string();
  };
  if (n.type == NodeType::Instrument) {
    instruments_ = instruments_.set(n.id - 1, Instrument{{n.id}, value(predicate::symbol), value(predicate::name),
                                                         kindOfName(value(predicate::kind)), value(predicate::isin),
                                                         value(predicate::type)});
  } else if (n.type == NodeType::Market) {
    markets_ = markets_.set(n.id - 1, Market{{n.id}, value(predicate::name)});
  } else if (n.type == NodeType::Dataset) {
    datasets_ = datasets_.set(n.id - 1, Dataset{{n.id}, value(predicate::name)});
  }
  version_++;
}

Instrument Snapshot::resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest) {
  NodeRef n;
  n.type = NodeType::Instrument;
  bool found = false;
  for (const Attribute & a : identifying)
    if (auto f = findByAttribute(NodeType::Instrument, a.predicate, a.value)) {
      n = *f;
      found = true;
      break;
    }
  if (!found) {
    n.id = static_cast<uint32_t>(instruments_.size() + 1);
    instruments_ = instruments_.push_back(Instrument{{n.id}, {}, {}, InstrumentKind::Other, {}, {}});
    version_++;
  }
  for (const Attribute & a : identifying) setAttribute(n, a);
  for (const Attribute & a : rest) setAttribute(n, a);
  return *instrument({n.id});
}
Instrument Snapshot::add(Instrument i) {
  // An instrument is its ISIN; one without (a currency) is its symbol.
  std::vector<Attribute> rest = {{predicate::name, i.name}, {predicate::type, i.type}};
  if (!i.isin.empty()) rest.push_back({predicate::symbol, i.symbol});
  if (i.kind != InstrumentKind::Other) rest.push_back({predicate::kind, kindName(i.kind)});
  return resolveInstrument({i.isin.empty() ? Attribute{predicate::symbol, i.symbol} : Attribute{predicate::isin, i.isin}}, rest);
}
Market Snapshot::resolve(const std::vector<Attribute> & attributes) {
  NodeRef n;
  n.type = NodeType::Market;
  bool found = false;
  for (const Attribute & a : attributes)
    if (auto f = findByAttribute(NodeType::Market, a.predicate, a.value)) {
      n = *f;
      found = true;
      break;
    }
  if (!found) {
    n.id = static_cast<uint32_t>(markets_.size() + 1);
    markets_ = markets_.push_back(Market{{n.id}, {}});
    version_++;
  }
  for (const Attribute & a : attributes) setAttribute(n, a);
  return *market({n.id});
}
Dataset Snapshot::add(Dataset d) {
  NodeRef n;
  n.type = NodeType::Dataset;
  if (auto f = findByAttribute(NodeType::Dataset, predicate::name, d.name)) {
    n = *f;
  } else {
    n.id = static_cast<uint32_t>(datasets_.size() + 1);
    datasets_ = datasets_.push_back(Dataset{{n.id}, {}});
    version_++;
  }
  setAttribute(n, {predicate::name, d.name});
  return *dataset({n.id});
}

namespace {
bool hasData(const immer::map<MarketId, immer::set<EdgeKey>> & byMarket, const EdgeKey & k) {
  const auto * set = byMarket.find(k.market);
  return set && set->count(k) > 0;
}
}  // namespace

void Snapshot::setListing(const EdgeKey & k, const Listing & l) {
  if (!hasData(edgesByMarket_, k)) return;  // no data yet: no edge
  const Listing * old = listings_.find(k);
  if (old && *old == l) return;
  if (old && old->symbol != l.symbol) {
    const std::string key = symbolKey(k.market, old->symbol);
    if (const EdgeKey * holder = edgeBySymbol_.find(key); holder && *holder == k) edgeBySymbol_ = edgeBySymbol_.erase(key);
  }
  listings_ = listings_.set(k, l);
  if (!l.symbol.empty() && !edgeBySymbol_.find(symbolKey(k.market, l.symbol)))
    edgeBySymbol_ = edgeBySymbol_.set(symbolKey(k.market, l.symbol), k);
  version_++;
}

namespace {
template <class T>
bool setFact(immer::map<FactKey, T> & map, const FactKey & k, const T & v) {
  const T * old = map.find(k);
  if (old && *old == v) return false;
  map = map.set(k, v);
  return true;
}
}  // namespace

void Snapshot::setFacts(const FactKey & k, const InstrumentFacts & f) { version_ += setFact(instrumentFacts_, k, f); }
void Snapshot::setFacts(const FactKey & k, const CompanyFacts & f) { version_ += setFact(companyFacts_, k, f); }
void Snapshot::setFacts(const FactKey & k, const FundFacts & f) { version_ += setFact(fundFacts_, k, f); }
void Snapshot::setFacts(const FactKey & k, const CertificateFacts & f) { version_ += setFact(certificateFacts_, k, f); }

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

  Instrument resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest) override {
    return write([&](Snapshot & s) { return s.resolveInstrument(identifying, rest); });
  }
  Instrument addInstrument(Instrument i) override {
    return write([&](Snapshot & s) { return s.add(std::move(i)); });
  }
  Market resolveMarket(const std::vector<Attribute> & a) override {
    return write([&](Snapshot & s) { return s.resolve(a); });
  }
  void setListing(const EdgeKey & k, const Listing & l) override {
    write([&](Snapshot & s) { s.setListing(k, l); });
  }
  void setFacts(const FactKey & k, const InstrumentFacts & f) override {
    write([&](Snapshot & s) { s.setFacts(k, f); });
  }
  void setFacts(const FactKey & k, const CompanyFacts & f) override {
    write([&](Snapshot & s) { s.setFacts(k, f); });
  }
  void setFacts(const FactKey & k, const FundFacts & f) override {
    write([&](Snapshot & s) { s.setFacts(k, f); });
  }
  void setFacts(const FactKey & k, const CertificateFacts & f) override {
    write([&](Snapshot & s) { s.setFacts(k, f); });
  }
  void setAttribute(NodeRef n, const Attribute & a) override {
    write([&](Snapshot & s) { s.setAttribute(n, a); });
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
