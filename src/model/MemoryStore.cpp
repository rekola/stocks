#include "model/Store.h"

#include "model/Isin.h"
#include "portfolio/Portfolio.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>

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
// `changed` is set when the series is not the one it was: a replacement `equal` to the element it replaces changes nothing.
template <class T, class Same, class Equal>
Seq<T> upsert(const Seq<T> & v, const T & item, Same same, Equal equal, bool & changed) {
  if (v.empty() || v.back().time < item.time) {
    changed = true;
    return v.push_back(item);
  }
  size_t lo = lowerIndex(v, item.time);
  size_t hi = lo;
  while (hi < v.size() && v[hi].time == item.time) {
    if (same(v[hi], item)) {
      if (equal(v[hi], item)) return v;
      changed = true;
      return v.set(hi, item);
    }
    hi++;
  }
  changed = true;
  return v.take(hi).push_back(item) + v.drop(hi);
}

// Puts `items` into the series of `key`; false, and the map untouched, where that changed nothing.
template <class K, class V, class Same, class Equal>
bool appendTo(auto & map, const K & key, const std::vector<V> & items, Same same, Equal equal) {
  Seq<V> seq = map.count(key) ? map.at(key) : Seq<V>{};
  bool changed = false;
  for (const V & i : items) seq = upsert(seq, i, same, equal, changed);
  if (changed) map = map.set(key, seq);
  return changed;
}

// Numbers as a bar says them: an undefined one (NaN) is the same as another.
bool sameNumber(double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); }
bool sameBar(const Candle & a, const Candle & b) {
  return a.time == b.time && sameNumber(a.open, b.open) && sameNumber(a.high, b.high) && sameNumber(a.low, b.low) &&
         sameNumber(a.close, b.close) && sameNumber(a.volume, b.volume);
}
bool sameLevels(const std::vector<BookLevel> & a, const std::vector<BookLevel> & b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                    [](const BookLevel & x, const BookLevel & y) { return sameNumber(x.price, y.price) && sameNumber(x.size, y.size); });
}
bool sameBook(const BookSnapshot & a, const BookSnapshot & b) {
  return sameNumber(a.time, b.time) && sameLevels(a.bids, b.bids) && sameLevels(a.asks, b.asks);
}
bool sameAction(const CorporateAction & a, const CorporateAction & b) {
  return a.time == b.time && a.kind == b.kind && sameNumber(a.value, b.value) && a.currency == b.currency && a.note == b.note;
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

NodeRef refOf(NodeKey key) { return nodeOf(key); }

// What an index holds a value as: text as it is, a link as a NUL and the number of the key it names (so that "who has this
// person as `ceo`" is as cheap as "who is called this"). None for the other alternatives, which are not found by their value.
std::optional<std::string> indexedValue(const Value & v) {
  if (const auto * t = std::get_if<std::string>(&v)) return *t;
  if (const auto * k = std::get_if<NodeKey>(&v)) return std::string(1, '\0') + std::to_string(k->value);
  return std::nullopt;
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

// A key with all of its parts. An instrument, a market or a dataset that was
// refused has none, and a caller that goes on to say something of an edge or an
// event with it has a bug: it is told by an exception, not left to write
// nothing or something under key zero. (A source that cannot name a market or
// an instrument skips the row, and says so.)
void require(const EdgeKey &k) {
  if (!k.base.valid() || !k.quote.valid() || !k.market.valid() ||
      !k.dataset.valid())
    throw std::invalid_argument(
        "an edge needs an instrument, a quote, a market and a dataset");
}
void require(const EventKey &k) {
  if (!k.instrument.valid() || !k.dataset.valid())
    throw std::invalid_argument("an event needs an instrument and a dataset");
}

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
  if (n.type == NodeType::Portfolio && name.empty()) return text(NodeKey{n.id}).substr(0, 7);  // its key, shortened, until it is named
  if (name.empty() && attrs && n.type != NodeType::Instrument)
    return text(NodeKey{n.id}); // the code or the MIC, until a name is said
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
std::optional<MarketId> Snapshot::findMarket(const std::string &mic) const {
  const auto key = marketKey(mic);
  if (!key || !attributes_.find(refOf(*key)))
    return std::nullopt;
  return MarketId{key->value};
}
std::vector<Attribute> Snapshot::attributes(NodeRef n) const {
  const auto * a = attributes_.find(n);
  return a ? *a : std::vector<Attribute>{};
}
std::optional<NodeRef> Snapshot::findByAttribute(NodeType t, const std::string & p, const std::string & v) const {
  return lowest(byAttribute_.find(indexKey(t, p, v)), t);
}
std::vector<NodeRef> Snapshot::findLinked(NodeType t, const std::string & p, NodeKey target) const {
  std::vector<NodeRef> out;
  if (const auto * ids = byAttribute_.find(indexKey(t, p, *indexedValue(Value{target}))))
    for (uint64_t id : *ids) {
      NodeRef n;
      n.type = t;
      n.id = id;
      out.push_back(n);
    }
  std::sort(out.begin(), out.end(), [](NodeRef a, NodeRef b) { return a.id < b.id; });
  return out;
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
std::vector<EdgeKey> Snapshot::edgesOf(InstrumentId base) const { return sorted(edgesByBase_.find(base)); }
std::vector<EdgeKey> Snapshot::edgesTo(InstrumentId quote) const { return sorted(edgesByQuote_.find(quote)); }
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
  require(k);
  if (!isDefined(close))
    return;
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

std::vector<NewsItem> Snapshot::news(const NewsQuery & q) const {
  std::vector<NewsItem> out;
  for (const auto & [dataset, items] : news_) {
    const auto first = std::lower_bound(items.begin(), items.end(), q.range.from, [](const NewsItem & i, Timestamp t) { return i.time < t; });
    for (auto it = first; it != items.end() && it->time < q.range.to; ++it) {
      if (q.about && std::find(it->about.begin(), it->about.end(), *q.about) == it->about.end()) continue;
      if (q.keep && !q.keep(*it)) continue;
      out.push_back(*it);
    }
  }
  std::sort(out.begin(), out.end(), [](const NewsItem & a, const NewsItem & b) {
    return std::tie(b.time, a.dataset, a.id) < std::tie(a.time, b.dataset, b.id);  // newest first, then by dataset and id
  });
  if (q.limit && out.size() > q.limit) out.resize(q.limit);
  return out;
}
size_t Snapshot::newsCount() const { return newsCount_; }
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
  if (const auto * k = std::get_if<NodeKey>(&v)) return k->valid();
  return true;
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
      if (const auto value = indexedValue(a.value)) {
        const std::string said = indexKey(n.type, a.predicate, *value);
        const auto * ids = byAttribute_.find(said);
        byAttribute_ = byAttribute_.set(said, (ids ? *ids : immer::set<uint64_t>{}).insert(n.id));
      }
    attributes_ = attributes_.set(n, std::move(attrs));
    if (n.type == NodeType::Portfolio) settle(PortfolioId{n.id});
  }
}

void Snapshot::setAttributes(NodeRef n, const std::vector<Attribute> & in) {
  const NodeKey nodeKey{n.id};
  if (!nodeKey.valid() || refOf(nodeKey).type != n.type) return;
  if (text(nodeKey).empty())
    return; // a number that is no ISIN, no code and no MIC is no node
  std::vector<Attribute> said;
  for (const Attribute & a : in)
    if (says(a.value) && !a.predicate.empty()) said.push_back(a);
  if (said.empty()) return;

  std::vector<Attribute> attrs = attributes(n);
  const auto find = [&](const std::string & predicate) {
    return std::find_if(attrs.begin(), attrs.end(), [&](const Attribute & x) { return x.predicate == predicate; });
  };
  const auto unindex = [&](const Attribute & a) {
    const auto value = indexedValue(a.value);
    if (!value) return;
    const std::string key = indexKey(n.type, a.predicate, *value);
    if (const auto * ids = byAttribute_.find(key)) {
      auto rest = ids->erase(n.id);
      byAttribute_ = rest.size() ? byAttribute_.set(key, rest) : byAttribute_.erase(key);
    }
  };
  const auto index = [&](const Attribute & a) {
    const auto value = indexedValue(a.value);
    if (!value) return;
    const std::string key = indexKey(n.type, a.predicate, *value);
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
      // Said again: `updated` is within the interval, so a value that is not old enough to need saying again changes nothing.
      const bool fresh = recheck_.count() > 0 && at->updated > now_ - recheck_.count();
      if (!fresh && at->updated != now_) {
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
  if (n.type == NodeType::Portfolio) settle(PortfolioId{n.id});
  if (changed.empty()) {
    checks_++;  // only times moved: no new version
    return;
  }
  version_++;
}

namespace {

bool isCurrencyKind(const std::string & kind) { return kind == kindName(InstrumentKind::Currency) || kind == kindName(InstrumentKind::Unit); }

}  // namespace

// An instrument is its ISIN; a currency or a unit its code; anything else that has no ISIN is numbered as it is
// asked for, or refused where the store is told not to (`StoreOptions::invent_isins`: the program's adapters give their own ISINs, from
// source/KnownIsins.h). What cannot be given a key is refused.
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
    const std::string called = symbol.empty() ? name : symbol;
    std::string own;
    if (!called.empty() && invent_) {
      // The same text is the same number, counting down from the top of the private namespace.
      if (const std::string * made = numbered_.find(called)) own = *made;
      else {
        std::string number = std::to_string(999999999 - numbered_.size());
        const std::string first11 = "ZZ" + std::string(9 - number.size(), '0') + number;
        own = first11 + isinCheckDigit(first11);
        numbered_ = numbered_.set(called, own);
      }
    } else if (!called.empty()) {
      notices_.push_back("instrument: " + called + " has no ISIN, so it is not kept");
    }
    if (!own.empty()) key = instrumentKey(own);
    if (!key && called.empty()) notices_.push_back("instrument: one with no ISIN, symbol or name is not kept");
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

// A market is its MIC, the ISO 10383 code: four characters of A-Z0-9, which
// never changes. Nothing else is a market: a code that is no MIC makes none,
// and says why. Its name is a fact of it that is the MIC until something says
// otherwise.
MarketId Snapshot::addMarket(const std::string &mic, const std::string &name) {
  const auto key = marketKey(mic);
  if (!key) {
    notices_.push_back("market: " + mic +
                       " is not a market code (four letters or digits, an ISO "
                       "10383 MIC), so it is not kept");
    return MarketId{};
  }
  const MarketId id{key->value};
  setAttributes(NodeRef(id), {{predicate::mic, mic}});
  if (!name.empty())
    setAttributes(NodeRef(id), {{predicate::name, name}});
  else if (!fact(NodeRef(id), predicate::name))
    setAttributes(NodeRef(id), {{predicate::name, mic}});
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

bool Snapshot::hasData(const EdgeKey & k) const {
  const auto * set = edgesByMarket_.find(k.market);
  return set && set->count(k) > 0;
}

// The edge's symbol finds it, if nothing found by it before: only an edge that has data is found.
void Snapshot::indexSymbol(const EdgeKey & k) {
  const Listing * l = listings_.find(k);
  if (!l || l->symbol.empty() || !hasData(k)) return;
  if (!edgeBySymbol_.find(symbolKey(k.market, l->symbol))) edgeBySymbol_ = edgeBySymbol_.set(symbolKey(k.market, l->symbol), k);
}

// The edge has data: it exists now, and a listing said before is found by its symbol.
void Snapshot::addEdge(const EdgeKey & k) {
  if (hasData(k)) return;
  const auto * set = edgesByMarket_.find(k.market);
  edgesByMarket_ = edgesByMarket_.set(k.market, (set ? *set : immer::set<EdgeKey>{}).insert(k));
  const auto * of = edgesByBase_.find(k.base);
  edgesByBase_ = edgesByBase_.set(k.base, (of ? *of : immer::set<EdgeKey>{}).insert(k));
  const auto * to = edgesByQuote_.find(k.quote);
  edgesByQuote_ = edgesByQuote_.set(k.quote, (to ? *to : immer::set<EdgeKey>{}).insert(k));
  indexSymbol(k);
}

void Snapshot::setListing(const Listing & l) {
  const EdgeKey & k = l.key;
  require(k);
  const Listing * old = listings_.find(k);
  if (old && *old == l) return;
  if (old && old->symbol != l.symbol) {
    const std::string key = symbolKey(k.market, old->symbol);
    if (const EdgeKey * holder = edgeBySymbol_.find(key); holder && *holder == k) edgeBySymbol_ = edgeBySymbol_.erase(key);
  }
  listings_ = listings_.set(k, l);
  indexSymbol(k);
  version_++;
}

void Snapshot::appendCandles(const EdgeKey & k, Resolution r, const std::vector<Candle> & in) {
  require(k);
  if (in.empty())
    return;
  const SeriesKey key{k, r};
  const auto * old = series_.find(key);
  Seq<Candle> bars = old ? *old : Seq<Candle>{};
  bool changed = false;
  for (const Candle & c : in) bars = upsert(bars, c, [](const Candle &, const Candle &) { return true; }, sameBar, changed);
  if (!changed) return;  // the same bars again: no new version, nothing for a reader to refresh
  series_ = series_.set(key, bars);
  const auto * known = resolutions_.find(k);
  if (r == Resolution::Day && !(known && known->count(static_cast<int>(r)))) quoteRows_ = quoteRows_.push_back(k);
  resolutions_ = resolutions_.set(k, (known ? *known : immer::set<int>{}).insert(static_cast<int>(r)));
  addEdge(k);
  version_++;
}

void Snapshot::appendBooks(const EdgeKey & k, const std::vector<BookSnapshot> & in) {
  require(k);
  if (in.empty())
    return;
  if (!appendTo(books_, k, in, [](const BookSnapshot &, const BookSnapshot &) { return true; }, sameBook)) return;
  addEdge(k);
  version_++;
}

void Snapshot::appendNews(DatasetId dataset, const std::vector<NewsItem> & in) {
  if (!dataset.valid() || NodeKey{dataset.value}.kind() != NodeKind::Dataset) throw std::invalid_argument("news needs a dataset");
  for (const NewsItem & item : in)
    if (item.id.empty()) throw std::invalid_argument("an item of news needs the dataset's id for it");
  const auto * oldItems = news_.find(dataset);
  const auto * oldIds = newsIds_.find(dataset);
  Seq<NewsItem> items = oldItems ? *oldItems : Seq<NewsItem>{};
  immer::set<std::string> ids = oldIds ? *oldIds : immer::set<std::string>{};
  size_t added = 0;
  for (NewsItem item : in) {
    if (ids.count(item.id)) continue;  // said before: left as it was
    ids = ids.insert(item.id);
    item.dataset = dataset;
    const size_t at = static_cast<size_t>(std::upper_bound(items.begin(), items.end(), item.time, [](Timestamp t, const NewsItem & i) { return t < i.time; }) - items.begin());
    items = items.insert(at, std::move(item));
    added++;
  }
  if (!added) return;
  news_ = news_.set(dataset, items);
  newsIds_ = newsIds_.set(dataset, ids);
  newsCount_ += added;
  version_++;
}

void Snapshot::appendActions(const EventKey & k, const std::vector<CorporateAction> & in) {
  require(k);
  if (in.empty())
    return;
  if (!appendTo(actions_, k, in, [](const CorporateAction & a, const CorporateAction & b) { return a.kind == b.kind; }, sameAction))
    return;
  const auto * set = eventsByInstrument_.find(k.instrument);
  eventsByInstrument_ = eventsByInstrument_.set(k.instrument, (set ? *set : immer::set<EventKey>{}).insert(k));
  version_++;
}


// ---- Portfolios

namespace {

void requireLedger(const LedgerKey & k) {
  if (!k.portfolio.valid() || NodeKey{k.portfolio.value}.kind() != NodeKind::Portfolio || !k.dataset.valid())
    throw std::invalid_argument("a ledger needs a portfolio and a dataset");
}

// A transaction that can be kept: all of its parts, and a market that something is bought on.
void requireBuyable(const Snapshot & s, const LedgerKey & ledger, const portfolio::Transaction & t) {
  if (t.id == 0 || !t.instrument.valid() || !t.market.valid() || !t.quote.valid())
    throw std::invalid_argument("a transaction needs an id, an instrument, a market and a currency");
  if (!portfolio::tradable(s, EdgeKey{t.instrument, t.quote, t.market, ledger.dataset}))
    throw std::invalid_argument("a transaction on the market " + marketCode(t.market) +
                                " is refused: what is priced there and not bought (an index, a spot price, a currency pair, a rate) cannot be held");
}

// Where a transaction goes in a ledger kept by day and then id.
bool before(const portfolio::Transaction & a, const portfolio::Transaction & b) { return std::tie(a.day, a.id) < std::tie(b.day, b.id); }

StoredEvent storedEvent(const LedgerKey & k, const portfolio::Transaction & t) {
  return StoredEvent{k.portfolio.value, k.dataset.value, t.id, t.day, portfolio::encodeBody(t)};
}

}  // namespace

std::vector<PortfolioId> Snapshot::portfolios() const {
  std::vector<std::pair<std::string, PortfolioId>> named;
  for (PortfolioId id : portfolios_) named.emplace_back(name(NodeRef(id)), id);
  std::sort(named.begin(), named.end(), [](const auto & a, const auto & b) { return std::tie(a.first, a.second) < std::tie(b.first, b.second); });
  std::vector<PortfolioId> out;
  for (const auto & entry : named) out.push_back(entry.second);
  return out;
}
Seq<portfolio::Transaction> Snapshot::transactions(const LedgerKey & k) const {
  const auto * t = ledgers_.find(k);
  return t ? *t : Seq<portfolio::Transaction>{};
}
std::vector<DatasetId> Snapshot::ledgers(PortfolioId id) const { return sorted(ledgersOf_.find(id)); }

void Snapshot::settle(PortfolioId id) {
  const auto * l = ledgersOf_.find(id);
  const bool said = attributes_.find(NodeRef(id)) != nullptr || (l && l->size() > 0);
  const bool listed = portfolios_.count(id) > 0;
  if (said && !listed) portfolios_ = portfolios_.insert(id);
  else if (!said && listed) portfolios_ = portfolios_.erase(id);
}

// What waits for the disk of a node (or of one predicate of it) is not wanted once it is taken back.
void Snapshot::forget(uint64_t node, const std::string * predicate) {
  const auto gone = [&](const StoredFact & f) { return f.node == node && (!predicate || f.predicate == *predicate); };
  std::erase_if(outbox_.changed, gone);
  std::erase_if(outbox_.rechecked, gone);
  if (!predicate) {
    std::erase_if(outbox_.events, [&](const StoredEvent & e) { return e.portfolio == node; });
    std::erase_if(outbox_.events_removed, [&](const StoredEvent & e) { return e.portfolio == node; });
  }
}

void Snapshot::removeAttribute(NodeRef n, const std::string & predicate) {
  if (n.type != NodeType::Portfolio) throw std::invalid_argument("only what is said of a portfolio can be taken back");
  const std::vector<Attribute> * old = attributes_.find(n);
  if (!old) return;
  std::vector<Attribute> attrs = *old;
  const auto at = std::find_if(attrs.begin(), attrs.end(), [&](const Attribute & a) { return a.predicate == predicate; });
  if (at == attrs.end()) return;
  if (const auto value = indexedValue(at->value)) {
    const std::string key = indexKey(n.type, predicate, *value);
    if (const auto * ids = byAttribute_.find(key)) {
      auto rest = ids->erase(n.id);
      byAttribute_ = rest.size() ? byAttribute_.set(key, rest) : byAttribute_.erase(key);
    }
  }
  attrs.erase(at);
  attributes_ = attrs.empty() ? attributes_.erase(n) : attributes_.set(n, std::move(attrs));
  forget(n.id, &predicate);
  outbox_.removed.emplace_back(n.id, predicate);
  settle(PortfolioId{n.id});
  version_++;
}

void Snapshot::removePortfolio(PortfolioId id) {
  if (!id.valid() || NodeKey{id.value}.kind() != NodeKind::Portfolio) throw std::invalid_argument("not a portfolio");
  const NodeRef n(id);
  bool changed = false;
  if (const auto * old = attributes_.find(n)) {
    for (const Attribute & a : *old)
      if (const auto value = indexedValue(a.value)) {
        const std::string key = indexKey(n.type, a.predicate, *value);
        if (const auto * ids = byAttribute_.find(key)) {
          auto rest = ids->erase(n.id);
          byAttribute_ = rest.size() ? byAttribute_.set(key, rest) : byAttribute_.erase(key);
        }
      }
    attributes_ = attributes_.erase(n);
    changed = true;
  }
  for (DatasetId d : ledgers(id)) ledgers_ = ledgers_.erase(LedgerKey{id, d});
  if (ledgersOf_.find(id)) {
    ledgersOf_ = ledgersOf_.erase(id);
    changed = true;
  }
  if (!changed) return;
  forget(id.value, nullptr);
  outbox_.removed_nodes.push_back(id.value);
  settle(id);
  version_++;
}

void Snapshot::appendTransactions(const LedgerKey & k, const std::vector<portfolio::Transaction> & in) {
  requireLedger(k);
  for (const portfolio::Transaction & t : in) requireBuyable(*this, k, t);
  if (in.empty()) return;
  std::vector<portfolio::Transaction> all;
  if (const auto * old = ledgers_.find(k)) all.assign(old->begin(), old->end());
  bool changed = false;
  for (const portfolio::Transaction & t : in) {
    auto at = std::find_if(all.begin(), all.end(), [&](const portfolio::Transaction & x) { return x.id == t.id; });
    if (at != all.end() && *at == t) continue;
    if (at != all.end()) *at = t;
    else all.push_back(t);
    changed = true;
    std::erase_if(outbox_.events, [&](const StoredEvent & e) { return e.portfolio == k.portfolio.value && e.dataset == k.dataset.value && e.id == t.id; });
    outbox_.events.push_back(storedEvent(k, t));
  }
  if (!changed) return;
  std::stable_sort(all.begin(), all.end(), before);
  ledgers_ = ledgers_.set(k, Seq<portfolio::Transaction>(all.begin(), all.end()));
  const auto * of = ledgersOf_.find(k.portfolio);
  ledgersOf_ = ledgersOf_.set(k.portfolio, (of ? *of : immer::set<DatasetId>{}).insert(k.dataset));
  settle(k.portfolio);
  version_++;
}

void Snapshot::removeTransactions(const LedgerKey & k, const std::vector<uint64_t> & ids) {
  requireLedger(k);
  const auto * old = ledgers_.find(k);
  if (!old) return;
  std::vector<portfolio::Transaction> kept;
  bool changed = false;
  for (const portfolio::Transaction & t : *old) {
    if (std::find(ids.begin(), ids.end(), t.id) == ids.end()) {
      kept.push_back(t);
      continue;
    }
    changed = true;
    std::erase_if(outbox_.events, [&](const StoredEvent & e) { return e.portfolio == k.portfolio.value && e.dataset == k.dataset.value && e.id == t.id; });
    outbox_.events_removed.push_back({k.portfolio.value, k.dataset.value, t.id, t.day, {}});
  }
  if (!changed) return;
  if (kept.empty()) {
    ledgers_ = ledgers_.erase(k);
    const auto rest = ledgersOf_.find(k.portfolio)->erase(k.dataset);
    ledgersOf_ = rest.size() ? ledgersOf_.set(k.portfolio, rest) : ledgersOf_.erase(k.portfolio);
  } else {
    ledgers_ = ledgers_.set(k, Seq<portfolio::Transaction>(kept.begin(), kept.end()));
  }
  settle(k.portfolio);
  version_++;
}

void Snapshot::loadEvents(const std::vector<StoredEvent> & events) {
  std::map<LedgerKey, std::vector<portfolio::Transaction>> grouped;
  for (const StoredEvent & e : events) {
    const NodeKey portfolioKey{e.portfolio}, datasetKey{e.dataset};
    if (portfolioKey.kind() != NodeKind::Portfolio || datasetKey.kind() != NodeKind::Dataset) continue;
    const LedgerKey key{PortfolioId{e.portfolio}, DatasetId{e.dataset}};
    auto t = portfolio::decodeTransaction(e.id, e.time, e.body);
    if (!t || t->id == 0 || !t->instrument.valid() || !t->market.valid() || !t->quote.valid() || !portfolio::tradable(t->market)) continue;
    grouped[key].push_back(std::move(*t));
  }
  for (auto & [key, list] : grouped) {
    std::stable_sort(list.begin(), list.end(), before);
    ledgers_ = ledgers_.set(key, Seq<portfolio::Transaction>(list.begin(), list.end()));
    const auto * of = ledgersOf_.find(key.portfolio);
    ledgersOf_ = ledgersOf_.set(key.portfolio, (of ? *of : immer::set<DatasetId>{}).insert(key.dataset));
    settle(key.portfolio);
  }
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

// The writes of a batch, made on the snapshot being built.
class SnapshotBatch : public Batch {
 public:
  explicit SnapshotBatch(Snapshot & s) : s_(s) {}
  const Snapshot & view() const override { return s_; }
  InstrumentId addInstrument(const std::string & symbol, const std::string & name, InstrumentKind kind, const std::string & isin, const std::string & type) override {
    return s_.addInstrument(symbol, name, kind, isin, type);
  }
  InstrumentId resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest) override {
    return s_.resolveInstrument(identifying, rest);
  }
  MarketId addMarket(const std::string & mic, const std::string & name) override { return s_.addMarket(mic, name); }
  void setAttribute(NodeRef n, const Attribute & a) override { s_.setAttribute(n, a); }
  void setAttributes(NodeRef n, const std::vector<Attribute> & a) override { s_.setAttributes(n, a); }
  void setListing(const Listing & l) override { s_.setListing(l); }
  DatasetId addDataset(const std::string & code, const std::string & name) override { return s_.addDataset(code, name); }
  void appendCandles(const EdgeKey & k, Resolution r, const std::vector<Candle> & c) override { s_.appendCandles(k, r, c); }
  void setPreviousClose(const EdgeKey & k, int64_t day, double close) override { s_.setPreviousClose(k, day, close); }
  void appendBooks(const EdgeKey & k, const std::vector<BookSnapshot> & b) override { s_.appendBooks(k, b); }
  void appendNews(DatasetId d, const std::vector<NewsItem> & n) override { s_.appendNews(d, n); }
  void appendActions(const EventKey & k, const std::vector<CorporateAction> & a) override { s_.appendActions(k, a); }
  void removeAttribute(NodeRef n, const std::string & p) override { s_.removeAttribute(n, p); }
  void removePortfolio(PortfolioId id) override { s_.removePortfolio(id); }
  void appendTransactions(const LedgerKey & k, const std::vector<portfolio::Transaction> & t) override { s_.appendTransactions(k, t); }
  void removeTransactions(const LedgerKey & k, const std::vector<uint64_t> & ids) override { s_.removeTransactions(k, ids); }

 private:
  Snapshot & s_;
};

class MemoryStore : public Store {
 public:
  explicit MemoryStore(StoreOptions options)
      : current_(std::make_shared<const Snapshot>()),
        recheck_(options.recheck_interval),
        clock_(std::move(options.clock)),
        notice_(std::move(options.notice)) {
    if (!clock_)
      clock_ = [] {
        return static_cast<TimestampMs>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
      };
    portfolios_ = std::move(options.portfolios);
    if (!options.invent_isins && !options.facts && !portfolios_) {
      Snapshot first;
      first.setInventIsins(false);
      current_.store(std::make_shared<const Snapshot>(std::move(first)));
    }
    if (options.facts || portfolios_) {
      // What was kept is the facts of its nodes (and the user's transactions), which makes no version and no edge. Read before any source
      // can write. A portfolio is in its own file and never in the cache: one that is found there anyway is not read.
      Snapshot first;
      first.setInventIsins(options.invent_isins);
      std::vector<StoredFact> kept = options.facts ? options.facts->loadAll() : std::vector<StoredFact>{};
      std::erase_if(kept, [](const StoredFact & f) { return NodeKey{f.node}.kind() == NodeKind::Portfolio; });
      if (portfolios_) {
        for (StoredFact & f : portfolios_->loadAll())
          if (NodeKey{f.node}.kind() == NodeKind::Portfolio) kept.push_back(std::move(f));
        first.loadEvents(portfolios_->loadEvents());
      }
      first.load(kept);
      tell(first.takeNotices());
      current_.store(std::make_shared<const Snapshot>(std::move(first)));
      if (options.facts) writer_ = std::make_unique<FactWriter>(std::move(options.facts), options.writer);
    }
  }

  void flush() override {
    if (writer_) writer_->flush();
  }

  std::shared_ptr<const Snapshot> snapshot() const override { return current_.load(); }

  void batch(const std::function<void(Batch &)> & change) override {
    write([&](Snapshot & s) {
      SnapshotBatch b(s);
      change(b);
    });
  }

  InstrumentId resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest) override {
    return write([&](Snapshot & s) { return s.resolveInstrument(identifying, rest); });
  }
  InstrumentId addInstrument(const std::string & symbol, const std::string & name, InstrumentKind kind, const std::string & isin, const std::string & type) override {
    return write([&](Snapshot & s) { return s.addInstrument(symbol, name, kind, isin, type); });
  }
  MarketId addMarket(const std::string &mic, const std::string &name) override {
    return write([&](Snapshot &s) { return s.addMarket(mic, name); });
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
  void appendNews(DatasetId d, const std::vector<NewsItem> & n) override {
    write([&](Snapshot & s) { s.appendNews(d, n); });
  }
  void appendActions(const EventKey & k, const std::vector<CorporateAction> & a) override {
    write([&](Snapshot & s) { s.appendActions(k, a); });
  }
  void removeAttribute(NodeRef n, const std::string & p) override {
    write([&](Snapshot & s) { s.removeAttribute(n, p); });
  }
  void removePortfolio(PortfolioId id) override {
    write([&](Snapshot & s) { s.removePortfolio(id); });
  }
  void appendTransactions(const LedgerKey & k, const std::vector<portfolio::Transaction> & t) override {
    write([&](Snapshot & s) { s.appendTransactions(k, t); });
  }
  void removeTransactions(const LedgerKey & k, const std::vector<uint64_t> & ids) override {
    write([&](Snapshot & s) { s.removeTransactions(k, ids); });
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
    next.setRecheckInterval(recheck_);
    const auto finish = [&] {
      next.settleVersion(old->version());
      tell(next.takeNotices());
      FactBatch facts = next.takeFacts();
      // Routed by the kind of node: the user's own go to their file, at once, and never into the cache.
      FactBatch mine;
      mine.removed = std::move(facts.removed);
      mine.removed_nodes = std::move(facts.removed_nodes);
      mine.events = std::move(facts.events);
      mine.events_removed = std::move(facts.events_removed);
      const auto portfolioFact = [](const StoredFact & f) { return NodeKey{f.node}.kind() == NodeKind::Portfolio; };
      for (auto * list : {&facts.changed, &facts.rechecked}) {
        auto & to = list == &facts.changed ? mine.changed : mine.rechecked;
        for (StoredFact & f : *list)
          if (portfolioFact(f)) to.push_back(f);
        std::erase_if(*list, portfolioFact);  // what is left is the sources'
      }
      if (portfolios_ && !mine.empty()) portfolios_->apply(mine);
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
  std::chrono::milliseconds recheck_;
  std::function<TimestampMs()> clock_;
  std::function<void(const std::string &)> notice_;
  std::set<std::string> told_;          // what notice_ has been told, which it is told once
  std::shared_ptr<FactStore> portfolios_;  // the user's file, if there is one
  std::unique_ptr<FactWriter> writer_;  // last: its thread ends first, with everything written
};

}  // namespace

std::unique_ptr<Store> makeMemoryStore() { return std::make_unique<MemoryStore>(StoreOptions{}); }
std::unique_ptr<Store> makeMemoryStore(StoreOptions options) { return std::make_unique<MemoryStore>(std::move(options)); }
