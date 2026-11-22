#include "model/Store.h"

#include "model/NodeIdentity.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
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
std::optional<Value> Snapshot::attribute(NodeRef n, const std::string & predicate) const {
  const std::vector<Attribute> * all = attributes_.find(n);
  if (!all) return std::nullopt;
  for (const Attribute & a : *all)
    if (a.predicate == predicate) return a.value;
  return std::nullopt;
}
std::optional<Attribute> Snapshot::fact(NodeRef n, const std::string & predicate) const {
  const std::vector<Attribute> * all = attributes_.find(n);
  if (!all) return std::nullopt;
  for (const Attribute & a : *all)
    if (a.predicate == predicate) return a;
  return std::nullopt;
}
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
std::optional<double> Snapshot::previousClose(const EdgeKey & k, int64_t day) const {
  const auto * p = previousClose_.find(k);
  if (!p || p->first != day) return std::nullopt;
  return p->second;
}
void Snapshot::setPreviousClose(const EdgeKey & k, int64_t day, double close) {
  if (!isDefined(close)) return;
  const auto * p = previousClose_.find(k);
  if (p && *p == std::make_pair(day, close)) return;
  previousClose_ = previousClose_.set(k, {day, close});
  version_++;
}
bool Snapshot::hasCandles(const EdgeKey & k, Resolution r) const {
  const auto * s = series_.find(SeriesKey{k, r});
  return s && !s->empty();
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

namespace {
// An empty text, an undefined number and nothing say nothing.
bool says(const Value & v) {
  if (std::holds_alternative<std::monostate>(v)) return false;
  if (const auto * t = std::get_if<std::string>(&v)) return !t->empty();
  if (const auto * d = std::get_if<double>(&v)) return !std::isnan(*d);
  return true;
}
std::string textOf(const Value & v) {
  const auto * t = std::get_if<std::string>(&v);
  return t ? *t : std::string();
}
}  // namespace

void Snapshot::setAttribute(NodeRef n, const Attribute & a) { setAttributes(n, {a}); }

namespace {
std::string heldKey(NodeType t, const std::string & key) { return std::string(1, static_cast<char>('0' + static_cast<int>(t))) + key; }

StoredFact stored(NodeType t, const std::string & key, const Attribute & a) {
  return StoredFact{t, key, a.predicate, a.value, a.updated};
}
}  // namespace

void Snapshot::hold(const std::vector<StoredFact> & facts) {
  std::map<std::string, std::vector<Attribute>> grouped;
  for (const StoredFact & f : facts) {
    if (!says(f.value) || f.predicate.empty() || f.node.empty()) continue;
    Attribute a(f.predicate, f.value);
    a.updated = f.updated;
    grouped[heldKey(f.type, f.node)].push_back(std::move(a));
  }
  for (auto & [key, attrs] : grouped) {
    const auto * old = held_.find(key);
    if (old) attrs.insert(attrs.begin(), old->begin(), old->end());
    held_ = held_.set(key, std::move(attrs));
  }
}

void Snapshot::setAttributes(NodeRef n, const std::vector<Attribute> & in) {
  const size_t count = n.type == NodeType::Instrument ? instruments_.size()
                       : n.type == NodeType::Market   ? markets_.size()
                                                      : datasets_.size();
  if (n.id == 0 || n.id > count) return;
  std::vector<Attribute> said;
  for (const Attribute & a : in)
    if (says(a.value) && !a.predicate.empty()) said.push_back(a);
  if (said.empty()) return;

  std::vector<Attribute> attrs = attributes(n);
  const auto find = [&](const std::string & predicate) {
    return std::find_if(attrs.begin(), attrs.end(), [&](const Attribute & x) { return x.predicate == predicate; });
  };
  const auto unindex = [&](const Attribute & a) {
    if (!std::holds_alternative<std::string>(a.value)) return;
    const std::string key = indexKey(n.type, a.predicate, textOf(a.value));
    if (const auto * ids = byAttribute_.find(key)) {
      auto rest = ids->erase(n.id);
      byAttribute_ = rest.size() ? byAttribute_.set(key, rest) : byAttribute_.erase(key);
    }
  };
  const auto index = [&](const Attribute & a) {
    if (!std::holds_alternative<std::string>(a.value)) return;
    const std::string key = indexKey(n.type, a.predicate, textOf(a.value));
    const auto * ids = byAttribute_.find(key);
    byAttribute_ = byAttribute_.set(key, (ids ? *ids : immer::set<uint32_t>{}).insert(n.id));
  };

  // What an earlier run said of this node, now that it is known by what it was kept under: it stands where this run has
  // not said otherwise, with the times it had, so a value said again is a check and not a change.
  bool adopted = false;
  {
    std::vector<Attribute> after = attrs;
    for (const Attribute & a : said) {
      auto at = std::find_if(after.begin(), after.end(), [&](const Attribute & x) { return x.predicate == a.predicate; });
      if (at != after.end()) at->value = a.value;
      else after.push_back(a);
    }
    const std::string key = nodeKey(n.type, after);
    if (!key.empty())
      if (const auto * held = held_.find(heldKey(n.type, key))) {
        for (const Attribute & h : *held) {
          if (find(h.predicate) != attrs.end()) continue;
          attrs.push_back(h);
          index(h);
          adopted = true;
        }
        held_ = held_.erase(heldKey(n.type, key));
      }
  }

  std::vector<size_t> changed, rechecked;  // positions in attrs
  for (const Attribute & a : said) {
    auto at = find(a.predicate);
    if (at == attrs.end()) {
      Attribute fresh = a;
      fresh.updated = now_;
      attrs.push_back(fresh);
      index(fresh);
      changed.push_back(attrs.size() - 1);
    } else if (at->value == a.value) {
      if (at->updated != now_) {
        at->updated = now_;
        rechecked.push_back(static_cast<size_t>(at - attrs.begin()));
      }
    } else {
      // The old text no longer says it of this node; the new, if it is text, does.
      unindex(*at);
      at->value = a.value;
      at->updated = now_;
      index(*at);
      changed.push_back(static_cast<size_t>(at - attrs.begin()));
    }
  }
  if (changed.empty() && rechecked.empty() && !adopted) return;

  // What goes to the disk: the facts of this write under the node's key; and where the node has just got a key, or a
  // stronger one, all of its facts under it, and none under the old.
  const std::string key = nodeKey(n.type, attrs);
  const std::string * known = keyOf_.find(n);
  if (!key.empty()) {
    if (!known || *known != key) {
      if (known) outbox_.forget.push_back({n.type, *known});
      for (const Attribute & a : attrs) outbox_.changed.push_back(stored(n.type, key, a));
      keyOf_ = keyOf_.set(n, key);
    } else {
      for (size_t i : changed) outbox_.changed.push_back(stored(n.type, key, attrs[i]));
      for (size_t i : rechecked) outbox_.rechecked.push_back(stored(n.type, key, attrs[i]));
    }
  }
  attributes_ = attributes_.set(n, attrs);
  if (changed.empty() && !adopted) {
    checks_++;  // only times moved: no new version
    return;
  }

  // The view the app reads is made from the attributes.
  auto value = [&](const char * p) {
    for (const Attribute & x : attrs)
      if (x.predicate == p) return textOf(x.value);
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
    if (auto f = findByAttribute(NodeType::Instrument, a.predicate, textOf(a.value))) {
      n = *f;
      found = true;
      break;
    }
  if (!found) {
    n.id = static_cast<uint32_t>(instruments_.size() + 1);
    instruments_ = instruments_.push_back(Instrument{{n.id}, {}, {}, InstrumentKind::Other, {}, {}});
    version_++;
  }
  setAttributes(n, identifying);  // together, so the node is known by the strongest of them from the first
  setAttributes(n, rest);
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
    if (auto f = findByAttribute(NodeType::Market, a.predicate, textOf(a.value))) {
      n = *f;
      found = true;
      break;
    }
  if (!found) {
    n.id = static_cast<uint32_t>(markets_.size() + 1);
    markets_ = markets_.push_back(Market{{n.id}, {}});
    version_++;
  }
  setAttributes(n, attributes);
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

void Snapshot::setListing(const Listing & l) {
  const EdgeKey & k = l.key;
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
  explicit MemoryStore(StoreOptions options) : current_(std::make_shared<const Snapshot>()), clock_(std::move(options.clock)) {
    if (!clock_)
      clock_ = [] {
        return static_cast<TimestampMs>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
      };
    if (options.facts) {
      // What was kept is held until its nodes are made; it makes none. Read before any source can write.
      Snapshot first;
      first.hold(options.facts->loadAll());
      current_.store(std::make_shared<const Snapshot>(std::move(first)));
      writer_ = std::make_unique<FactWriter>(std::move(options.facts), options.writer);
    }
  }

  void flush() override {
    if (writer_) writer_->flush();
  }

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
  void setAttributes(NodeRef n, const std::vector<Attribute> & a) override {
    write([&](Snapshot & s) { s.setAttributes(n, a); });
  }
  void setListing(const Listing & l) override {
    write([&](Snapshot & s) { s.setListing(l); });
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
  void setPreviousClose(const EdgeKey & k, int64_t day, double close) override {
    write([&](Snapshot & s) { s.setPreviousClose(k, day, close); });
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
  // Change a copy of the current snapshot and publish it if the version moved, or if only the times of facts said again did
  // (the version is the same then, so nothing that watches it wakes). What the change asks of the disk is handed on
  // in the order the writes were made.
  template <class F>
  std::invoke_result_t<F, Snapshot &> write(F change) {
    std::lock_guard lock(mutex_);
    std::shared_ptr<const Snapshot> old = current_.load();
    Snapshot next = *old;
    next.setTime(clock_());
    const auto finish = [&] {
      FactBatch facts = next.takeFacts();
      if (writer_) writer_->post(std::move(facts));
      if (next.revision() != old->revision()) current_.store(std::make_shared<const Snapshot>(std::move(next)));
    };
    if constexpr (std::is_void_v<decltype(change(next))>) {
      change(next);
      finish();
    } else {
      auto result = change(next);
      finish();
      return result;
    }
  }

  std::mutex mutex_;
  SnapshotSlot current_;
  std::function<TimestampMs()> clock_;
  std::unique_ptr<FactWriter> writer_;  // last: its thread ends first, with everything written
};

}  // namespace

std::unique_ptr<Store> makeMemoryStore() { return std::make_unique<MemoryStore>(StoreOptions{}); }
std::unique_ptr<Store> makeMemoryStore(StoreOptions options) { return std::make_unique<MemoryStore>(std::move(options)); }
