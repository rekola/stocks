#include "model/Store.h"

#include "model/NodeIdentity.h"
#include "model/Isin.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <map>
#include <random>
#include <set>
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

namespace {

// The text of a predicate among the facts of a node, else empty.
std::string textFact(const std::vector<Attribute> & attrs, const char * predicate) {
  for (const Attribute & a : attrs)
    if (a.predicate == predicate)
      if (const auto * text = std::get_if<std::string>(&a.value)) return *text;
  return {};
}

std::string indexKey(NodeType t, const std::string & p, const std::string & v) {
  return std::string(1, static_cast<char>('0' + static_cast<int>(t))) + p + '\0' + v;
}

// The node of a key: an instrument or a currency is an instrument.
NodeRef refOf(NodeKey key) {
  NodeRef n;
  n.id = key.value;
  n.type = key.kind() == NodeKind::Market ? NodeType::Market : key.kind() == NodeKind::Dataset ? NodeType::Dataset
                                                                                               : NodeType::Instrument;
  return n;
}

std::optional<NodeRef> lowest(const immer::set<uint64_t> * ids, NodeType t) {
  if (!ids || ids->size() == 0) return std::nullopt;
  uint64_t first = std::numeric_limits<uint64_t>::max();
  for (uint64_t id : *ids) first = std::min(first, id);
  NodeRef n;
  n.type = t;
  n.id = first;
  return n;
}

// A key with all of its parts: an instrument that was refused (no ISIN, a code that does not fit) has none, and what is said of
// an edge or an event that has it is said of nothing.
bool whole(const EdgeKey & k) { return k.base.valid() && k.quote.valid() && k.market.valid() && k.dataset.valid(); }
bool whole(const EventKey & k) { return k.instrument.valid() && k.dataset.valid(); }

std::string symbolKey(MarketId m, const std::string & symbol) { return std::to_string(m.value) + '\0' + symbol; }

}  // namespace

// A node is what is said of it: these read its facts, and no record is stored.
bool Snapshot::known(NodeRef n) const { return attributes_.find(n) != nullptr; }
std::string Snapshot::said(NodeRef n, const std::string & predicate) const {
  const auto * attrs = attributes_.find(n);
  return attrs ? textFact(*attrs, predicate.c_str()) : std::string();
}
std::string Snapshot::symbol(InstrumentId id) const { return said(NodeRef(id), predicate::symbol); }
InstrumentKind Snapshot::kind(InstrumentId id) const { return kindOfName(said(NodeRef(id), predicate::kind)); }
std::string Snapshot::isin(InstrumentId id) const { return NodeKey{id.value}.kind() == NodeKind::Instrument ? text(NodeKey{id.value}) : std::string(); }
std::string Snapshot::name(NodeRef n) const {
  const auto * attrs = attributes_.find(n);
  std::string name = attrs ? textFact(*attrs, predicate::name) : std::string();
  if (name.empty() && n.type == NodeType::Dataset && attrs) return text(NodeKey{n.id});  // the code, until a name is said
  return name;
}
std::optional<InstrumentId> Snapshot::findInstrument(const std::string & s) const {
  auto n = findByAttribute(NodeType::Instrument, predicate::symbol, s);
  return n ? std::optional<InstrumentId>(InstrumentId{n->id}) : std::nullopt;
}
std::optional<Listing> Snapshot::listing(const EdgeKey & k) const {
  const Listing * l = listings_.find(k);
  return l ? std::optional<Listing>(*l) : std::nullopt;
}
std::string Snapshot::symbol(const EdgeKey & k) const {
  if (const Listing * l = listings_.find(k); l && !l->symbol.empty()) return l->symbol;
  return symbol(k.base);
}
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
std::optional<InstrumentId> Snapshot::findInstrumentByIsin(const std::string & isin) const {
  const auto key = instrumentKey(isin);
  if (!key || !attributes_.find(refOf(*key))) return std::nullopt;
  return InstrumentId{key->value};
}
std::optional<MarketId> Snapshot::findMarket(const std::string & s) const {
  return findMarketByAttribute(predicate::name, s);
}
std::optional<MarketId> Snapshot::findMarketByAttribute(const std::string & p, const std::string & v) const {
  auto n = findByAttribute(NodeType::Market, p, v);
  return n ? std::optional<MarketId>(MarketId{n->id}) : std::nullopt;
}
std::vector<Attribute> Snapshot::attributes(NodeRef n) const {
  const auto * a = attributes_.find(n);
  return a ? *a : std::vector<Attribute>{};
}
std::optional<NodeRef> Snapshot::findByAttribute(NodeType t, const std::string & p, const std::string & v) const {
  return lowest(byAttribute_.find(indexKey(t, p, v)), t);
}
std::optional<DatasetId> Snapshot::findDataset(const std::string & code) const {
  const auto key = datasetKey(code);
  if (!key || !attributes_.find(refOf(*key))) return std::nullopt;
  return DatasetId{key->value};
}
size_t Snapshot::nodeCount(NodeType type) const {
  size_t n = 0;
  for (const auto & entry : attributes_) n += entry.first.type == type;
  return n;
}
std::vector<MarketId> Snapshot::edgeMarkets() const {
  std::vector<std::pair<std::string, MarketId>> named;
  for (const auto & [market, edges] : edgesByMarket_)
    if (edges.size() > 0) {
      named.emplace_back(name(NodeRef(market)), market);
    }
  std::sort(named.begin(), named.end());
  std::vector<MarketId> out;
  for (const auto & entry : named) out.push_back(entry.second);
  return out;
}

std::vector<EdgeKey> Snapshot::edges(MarketId m) const { return sorted(edgesByMarket_.find(m)); }
std::vector<EdgeKey> Snapshot::edges(MarketId m, const std::function<bool(InstrumentId)> & keep) const {
  std::vector<EdgeKey> out;
  for (const EdgeKey & k : edges(m))
    if (known(NodeRef(k.base)) && keep(k.base)) out.push_back(k);
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
  if (!isDefined(close) || !whole(k)) return;
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
StoredFact stored(uint64_t node, const Attribute & a) { return StoredFact{node, a.predicate, a.value, a.updated}; }
}  // namespace

void Snapshot::load(const std::vector<StoredFact> & facts) {
  std::unordered_map<NodeRef, std::vector<Attribute>> grouped;
  for (const StoredFact & f : facts) {
    const NodeKey key{f.node};
    if (!key.valid() || !says(f.value) || f.predicate.empty()) continue;
    Attribute a(f.predicate, f.value);
    a.updated = f.updated;
    grouped[refOf(key)].push_back(std::move(a));
  }
  for (auto & [n, attrs] : grouped) {
    for (const Attribute & a : attrs)
      if (const auto * text = std::get_if<std::string>(&a.value)) {
        const std::string said = indexKey(n.type, a.predicate, *text);
        const auto * ids = byAttribute_.find(said);
        byAttribute_ = byAttribute_.set(said, (ids ? *ids : immer::set<uint64_t>{}).insert(n.id));
      }
    attributes_ = attributes_.set(n, std::move(attrs));
  }
  // What a file says of two nodes that no two have is told, and the lowest key is the node.
  for (const auto & [said, ids] : byAttribute_) {
    const size_t at = said.find('\0');
    const auto type = static_cast<NodeType>(said[0] - '0');
    if (ids.size() > 1 && at != std::string::npos && isUnique(type, said.substr(1, at - 1)))
      notices_.push_back("market: the file says " + said.substr(1, at - 1) + " " + said.substr(at + 1) + " of " + std::to_string(ids.size()) + " markets");
  }
}

void Snapshot::setAttributes(NodeRef n, const std::vector<Attribute> & in) {
  const NodeKey nodeKey{n.id};
  if (!nodeKey.valid() || refOf(nodeKey).type != n.type) return;
  if (nodeKey.kind() != NodeKind::Market && text(nodeKey).empty()) return;  // a number that is no ISIN and no code is no node
  std::vector<Attribute> said;
  for (const Attribute & a : in)
    if (says(a.value) && !a.predicate.empty()) said.push_back(a);
  if (said.empty()) return;

  // What says which node is meant is not said of two: another node that has it keeps it, and the second is refused.
  std::erase_if(said, [&](const Attribute & a) {
    const auto * text = std::get_if<std::string>(&a.value);
    if (!text || !isUnique(n.type, a.predicate)) return false;
    const auto * ids = byAttribute_.find(indexKey(n.type, a.predicate, *text));
    if (!ids || !std::any_of(ids->begin(), ids->end(), [&](uint64_t id) { return id != n.id; })) return false;
    notices_.push_back(std::string(n.type == NodeType::Market ? "market" : n.type == NodeType::Instrument ? "instrument"
                                                                                                          : "dataset") +
                       ": " +
                       a.predicate + " " + *text + " is already said of another, not said of this one too");
    return true;
  });
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
    byAttribute_ = byAttribute_.set(key, (ids ? *ids : immer::set<uint64_t>{}).insert(n.id));
  };

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
  if (changed.empty() && rechecked.empty()) return;

  for (size_t i : changed) outbox_.changed.push_back(stored(n.id, attrs[i]));
  for (size_t i : rechecked) outbox_.rechecked.push_back(stored(n.id, attrs[i]));
  attributes_ = attributes_.set(n, attrs);
  if (changed.empty()) {
    checks_++;  // only times moved: no new version
    return;
  }
  version_++;
}

namespace {

bool isCurrencyKind(const std::string & kind) { return kind == kindName(InstrumentKind::Currency) || kind == kindName(InstrumentKind::Unit); }

}  // namespace

// An instrument is its ISIN; a currency or a unit its code; anything else that has no ISIN is given one of our own, made from its
// symbol or else its name (isin.h), the way a thing is given a name nobody else uses. What cannot be given a key is refused.
InstrumentId Snapshot::resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest) {
  std::vector<Attribute> all = identifying;
  all.insert(all.end(), rest.begin(), rest.end());
  const std::string isin = textFact(all, predicate::isin), symbol = textFact(all, predicate::symbol), name = textFact(all, predicate::name);
  std::optional<NodeKey> key;
  if (!isin.empty()) {
    key = instrumentKey(isin);
    if (!key) notices_.push_back("instrument: the ISIN " + isin + " is not valid, so it is not kept");
  } else if (isCurrencyKind(textFact(all, predicate::kind))) {
    key = currencyKey(symbol);
    if (!key) notices_.push_back("instrument: the code " + symbol + " is not one to ten letters or digits, so it is not kept");
  } else {
    for (const std::string & candidate : {symbol, name})
      if (!key && !candidate.empty())
        if (const std::string own = privateIsin(candidate); !own.empty()) key = instrumentKey(own);
    if (!key) notices_.push_back("instrument: " + (symbol.empty() ? name : symbol) + " has no ISIN and none can be made for it, so it is not kept");
  }
  if (!key) return InstrumentId{};
  const InstrumentId id{key->value};
  // An instrument that is an ISIN says it, even one of ours.
  if (key->kind() == NodeKind::Instrument && isin.empty()) all.emplace_back(predicate::isin, text(*key));
  setAttributes(NodeRef(id), all);
  return id;
}
InstrumentId Snapshot::addInstrument(const std::string & symbol, const std::string & name, InstrumentKind kind, const std::string & isin, const std::string & type) {
  std::vector<Attribute> rest = {{predicate::name, name}, {predicate::type, type}};
  if (!symbol.empty()) rest.push_back({predicate::symbol, symbol});
  if (!isin.empty()) rest.push_back({predicate::isin, isin});
  if (kind != InstrumentKind::Other) rest.push_back({predicate::kind, kindName(kind)});
  return resolveInstrument({}, rest);
}

// A market has no identifier all of them have, so its key is minted when it is first said, and it is found again by what is said
// of it (the first of these that a market has, in the order given). What it is said to be that another market is said to be
// too is refused: that is two records of one market, which this does not settle.
MarketId Snapshot::resolveMarket(const std::vector<Attribute> & attributes) {
  std::optional<NodeRef> found;
  for (const Attribute & a : attributes)
    if (const auto * text = std::get_if<std::string>(&a.value); text && !found) found = findByAttribute(NodeType::Market, a.predicate, *text);
  MarketId id{found ? found->id : 0};
  if (!id.valid()) {
    if (!mint_ || !*mint_) return MarketId{};
    for (int tries = 0; tries < 8 && !id.valid(); tries++) {
      const NodeKey candidate = marketKey((*mint_)());
      if (!attributes_.find(refOf(candidate))) id = MarketId{candidate.value};
    }
    if (!id.valid()) return MarketId{};
  }
  setAttributes(NodeRef(id), attributes);
  return id;
}

// A dataset is its code. Its name is a fact of it that is the code until something says otherwise.
DatasetId Snapshot::addDataset(const std::string & code, const std::string & name) {
  const auto key = datasetKey(code);
  if (!key) {
    notices_.push_back("dataset: the code " + code + " is not one to ten letters or digits, so it is not kept");
    return DatasetId{};
  }
  const DatasetId id{key->value};
  if (!name.empty())
    setAttributes(NodeRef(id), {{predicate::name, name}});
  else if (!fact(NodeRef(id), predicate::name))
    setAttributes(NodeRef(id), {{predicate::name, code}});
  return id;
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
  if (in.empty() || !whole(k)) return;
  const SeriesKey key{k, r};
  const auto * old = series_.find(key);
  Seq<Candle> bars = old ? *old : Seq<Candle>{};
  for (const Candle & c : in) bars = upsert(bars, c, [](const Candle &, const Candle &) { return true; });
  series_ = series_.set(key, bars);
  const auto * known = resolutions_.find(k);
  if (r == Resolution::Day && !(known && known->count(static_cast<int>(r)))) quoteRows_ = quoteRows_.push_back(k);
  resolutions_ = resolutions_.set(k, (known ? *known : immer::set<int>{}).insert(static_cast<int>(r)));
  const auto * set = edgesByMarket_.find(k.market);
  edgesByMarket_ = edgesByMarket_.set(k.market, (set ? *set : immer::set<EdgeKey>{}).insert(k));
  version_++;
}

void Snapshot::appendBooks(const EdgeKey & k, const std::vector<BookSnapshot> & in) {
  if (in.empty() || !whole(k)) return;
  appendTo(books_, k, in, [](const BookSnapshot &, const BookSnapshot &) { return true; });
  const auto * set = edgesByMarket_.find(k.market);
  edgesByMarket_ = edgesByMarket_.set(k.market, (set ? *set : immer::set<EdgeKey>{}).insert(k));
  version_++;
}

void Snapshot::appendAnnouncements(const EventKey & k, const std::vector<Announcement> & in) {
  if (in.empty() || !whole(k)) return;
  appendTo(announcements_, k, in,
           [](const Announcement & a, const Announcement & b) { return a.headline == b.headline; });
  const auto * set = eventsByInstrument_.find(k.instrument);
  eventsByInstrument_ = eventsByInstrument_.set(k.instrument, (set ? *set : immer::set<EventKey>{}).insert(k));
  version_++;
}

void Snapshot::appendActions(const EventKey & k, const std::vector<CorporateAction> & in) {
  if (in.empty() || !whole(k)) return;
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
  explicit MemoryStore(StoreOptions options)
      : current_(std::make_shared<const Snapshot>()),
        clock_(std::move(options.clock)),
        mint_(std::move(options.mint)),
        notice_(std::move(options.notice)) {
    if (!mint_)
      mint_ = [] {
        static std::mutex m;
        static std::mt19937_64 random{std::random_device{}()};
        std::lock_guard lock(m);
        return static_cast<uint64_t>(random());
      };
    if (!clock_)
      clock_ = [] {
        return static_cast<TimestampMs>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
      };
    if (options.facts) {
      // What was kept is the facts of its nodes, which makes no version and no edge. Read before any source can write.
      Snapshot first;
      first.load(options.facts->loadAll());
      tell(first.takeNotices());
      current_.store(std::make_shared<const Snapshot>(std::move(first)));
      writer_ = std::make_unique<FactWriter>(std::move(options.facts), options.writer);
    }
  }

  void flush() override {
    if (writer_) writer_->flush();
  }

  std::shared_ptr<const Snapshot> snapshot() const override { return current_.load(); }

  InstrumentId resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest) override {
    return write([&](Snapshot & s) { return s.resolveInstrument(identifying, rest); });
  }
  InstrumentId addInstrument(const std::string & symbol, const std::string & name, InstrumentKind kind, const std::string & isin, const std::string & type) override {
    return write([&](Snapshot & s) { return s.addInstrument(symbol, name, kind, isin, type); });
  }
  MarketId resolveMarket(const std::vector<Attribute> & a) override {
    return write([&](Snapshot & s) { return s.resolveMarket(a); });
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
  DatasetId addDataset(const std::string & code, const std::string & name) override {
    return write([&](Snapshot & s) { return s.addDataset(code, name); });
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
  void tell(const std::vector<std::string> & notices) {
    for (const std::string & text : notices)
      if (notice_ && told_.insert(text).second) notice_(text);
  }

  // Change a copy of the current snapshot and publish it if the version moved, or if only the times of facts said again did
  // (the version is the same then, so nothing that watches it wakes). What the change asks of the disk is handed on
  // in the order the writes were made.
  template <class F>
  std::invoke_result_t<F, Snapshot &> write(F change) {
    std::lock_guard lock(mutex_);
    std::shared_ptr<const Snapshot> old = current_.load();
    Snapshot next = *old;
    next.setTime(clock_());
    next.setKeyMaker(&mint_);
    const auto finish = [&] {
      next.setKeyMaker(nullptr);
      tell(next.takeNotices());
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
  std::function<uint64_t()> mint_;  // the keys of markets
  std::function<void(const std::string &)> notice_;
  std::set<std::string> told_;          // what notice_ has been told, which it is told once
  std::unique_ptr<FactWriter> writer_;  // last: its thread ends first, with everything written
};

}  // namespace

std::unique_ptr<Store> makeMemoryStore() { return std::make_unique<MemoryStore>(StoreOptions{}); }
std::unique_ptr<Store> makeMemoryStore(StoreOptions options) { return std::make_unique<MemoryStore>(std::move(options)); }
