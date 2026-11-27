#include "model/Store.h"

#include "model/Dates.h"
#include "model/Isin.h"
#include "model/Period.h"
#include "model/SummaryRecords.h"
#include "portfolio/Portfolio.h"

#include <immer/map_transient.hpp>
#include <immer/set_transient.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
  dirtyEdges_.insert(k);
  version_++;
  refreshChanges(k);
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

void Snapshot::load(std::vector<StoredFact> facts) {
  std::unordered_map<NodeRef, std::vector<Attribute>> grouped;
  grouped.reserve(facts.size() / 8 + 1);
  for (StoredFact & f : facts) {
    const NodeKey key{f.node};
    if (!key.valid() || !says(f.value) || f.predicate.empty()) continue;
    Attribute a(std::move(f.predicate), std::move(f.value));
    a.updated = f.updated;
    grouped[refOf(key)].push_back(std::move(a));
  }
  // Built in bulk, on transients: the node's attributes go in whole, and the index of each text and link is added to once for all the
  // nodes that say it, not once for each fact.
  auto attrs = attributes_.transient();
  std::unordered_map<std::string, std::vector<uint64_t>> added;  // index key -> the nodes that say it
  std::vector<PortfolioId> portfolios;
  for (auto & [n, list] : grouped) {
    const std::vector<Attribute> * old = attrs.find(n);
    if (old) {
      // A source has said something of the node since: that stands, and the disk adds what was not said.
      std::vector<Attribute> merged = *old;
      for (Attribute & a : list)
        if (std::none_of(merged.begin(), merged.end(), [&](const Attribute & x) { return x.predicate == a.predicate; })) {
          if (const auto value = indexedValue(a.value)) added[indexKey(n.type, a.predicate, *value)].push_back(n.id);
          merged.push_back(std::move(a));
        }
      attrs.set(n, std::move(merged));
    } else {
      for (const Attribute & a : list)
        if (const auto value = indexedValue(a.value)) added[indexKey(n.type, a.predicate, *value)].push_back(n.id);
      attrs.set(n, std::move(list));
    }
    if (n.type == NodeType::Portfolio) portfolios.push_back(PortfolioId{n.id});
  }
  attributes_ = attrs.persistent();
  auto index = byAttribute_.transient();
  for (auto & [key, ids] : added) {
    const auto * existing = index.find(key);
    auto nodes = (existing ? *existing : immer::set<uint64_t>{}).transient();
    for (uint64_t id : ids) nodes.insert(id);
    index.set(key, nodes.persistent());
  }
  byAttribute_ = index.persistent();
  for (PortfolioId id : portfolios) settle(id);
  if (!grouped.empty()) checks_++;  // published, but no version: restoring makes none
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
    if (!key) notices_.push_back("instrument: the code " + symbol + " is not one to eleven capitals, digits, hyphens, periods or hashes, so it is not kept");
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
    notices_.push_back("dataset: the code " + code + " is not one to eleven capitals, digits, hyphens, periods or hashes, so it is not kept");
    return DatasetId{};
  }
  const DatasetId id{key->value};
  if (!name.empty())
    setAttributes(NodeRef(id), {{predicate::name, name}});
  else if (!fact(NodeRef(id), predicate::name))
    setAttributes(NodeRef(id), {{predicate::name, text(*key)}});  // the code as the key has it: its case is not kept
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
  newEdges_.push_back(k);
  edgesByMarket_ = edgesByMarket_.set(k.market, (set ? *set : immer::set<EdgeKey>{}).insert(k));
  const auto * of = edgesByBase_.find(k.base);
  edgesByBase_ = edgesByBase_.set(k.base, (of ? *of : immer::set<EdgeKey>{}).insert(k));
  const auto * to = edgesByQuote_.find(k.quote);
  edgesByQuote_ = edgesByQuote_.set(k.quote, (to ? *to : immer::set<EdgeKey>{}).insert(k));
  indexSymbol(k);
}

void Snapshot::setListing(const Listing & l) {
  require(l.key);
  putListing(l, false);
}

// The listing of an edge. One that is read from the disk does not replace what a source has said, and asks nothing of the disk.
void Snapshot::putListing(const Listing & l, bool fromDisk) {
  const EdgeKey & k = l.key;
  const Listing * old = listings_.find(k);
  if (old && (fromDisk || *old == l)) return;
  if (old && old->symbol != l.symbol) {
    const std::string key = symbolKey(k.market, old->symbol);
    if (const EdgeKey * holder = edgeBySymbol_.find(key); holder && *holder == k) edgeBySymbol_ = edgeBySymbol_.erase(key);
  }
  listings_ = listings_.set(k, l);
  indexSymbol(k);
  if (!fromDisk) dirtyEdges_.insert(k);
  version_++;
}

void Snapshot::appendCandles(const EdgeKey & k, Resolution r, const std::vector<Candle> & in, QueueKind kind) {
  require(k);
  if (in.empty())
    return;
  // The data: the bars as written, which only memory keeps.
  const SeriesKey key{k, r};
  const auto * old = series_.find(key);
  Seq<Candle> bars = old ? *old : Seq<Candle>{};
  bool changed = false;
  for (const Candle & c : in) bars = upsert(bars, c, [](const Candle &, const Candle &) { return true; }, sameBar, changed);
  // The summary: the quote and the queue of the resolution, derived from the same bars.
  const bool summarised = deriveSummary(k, r, in, kind);
  if (!changed && !summarised) return;  // the same bars again: no new version, nothing for a reader to refresh
  if (changed) {
    auto & pending = pendingBars_[key];
    pending.insert(pending.end(), in.begin(), in.end());
    series_ = series_.set(key, bars);
    const auto * known = resolutions_.find(k);
    resolutions_ = resolutions_.set(k, (known ? *known : immer::set<int>{}).insert(static_cast<int>(r)));
  }
  addEdge(k);
  version_++;
}

// ---- The summary: the quote and the queues, which are not the data above

namespace {

std::array<int64_t, 11> defaultRules() {
  std::array<int64_t, 11> rules{};
  for (int r = 0; r <= static_cast<int>(Resolution::Quarter); r++)
    if (const auto keep = defaultKeep(static_cast<Resolution>(r))) rules[static_cast<size_t>(r)] = keep->count();
  return rules;
}

// The bars of a queue cut to the window measured back from the last one; `changed` where bars were popped.
Seq<Candle> cutToWindow(Seq<Candle> bars, Resolution r, int64_t keep, bool & changed) {
  if (bars.empty() || keep <= 0) return bars;
  const int64_t from = summary::instantOf(r, bars.back().time) - keep;
  size_t popped = 0;
  while (popped < bars.size() && summary::instantOf(r, bars[popped].time) < from) popped++;
  if (!popped) return bars;
  changed = true;
  return bars.drop(popped);
}

QueueCoverage coverageOf(const QueueState & q) {
  if (q.resident) {
    QueueCoverage c;
    c.kind = q.kind;
    c.fetched = q.fetched;
    c.count = static_cast<uint32_t>(q.bars.size());
    if (!q.bars.empty()) {
      c.first_key = q.bars.front().time;
      c.last_key = q.bars.back().time;
      c.last_hash = summary::hashOf(q.kind, q.bars.back());
    }
    return c;
  }
  QueueCoverage c = q.disk;
  c.kind = q.kind;
  c.fetched = std::max(q.disk.fetched, q.fetched);
  if (!q.bars.empty()) {
    // Said since, and not known to be among the file's: an upper bound on the count.
    c.first_key = c.count ? std::min(c.first_key, q.bars.front().time) : q.bars.front().time;
    if (!c.count || q.bars.back().time >= c.last_key) c.last_hash = summary::hashOf(q.kind, q.bars.back());
    c.last_key = c.count ? std::max(c.last_key, q.bars.back().time) : q.bars.back().time;
    c.count += static_cast<uint32_t>(q.bars.size());
  }
  return c;
}

constexpr size_t kBarBytes = sizeof(Candle);

}  // namespace

bool Snapshot::listed(const EdgeKey & k) const { return rows_.count(k) > 0; }
std::optional<Quote> Snapshot::quote(const EdgeKey & k) const {
  const Quote * q = quotes_.find(k);
  return q ? std::optional<Quote>(*q) : std::nullopt;
}
Seq<Candle> Snapshot::queue(const EdgeKey & k, Resolution r) const {
  const QueueState * q = queues_.find(SeriesKey{k, r});
  return q && q->resident ? q->bars : Seq<Candle>{};
}
std::optional<QueueCoverage> Snapshot::coverage(const EdgeKey & k, Resolution r) const {
  const QueueState * q = queues_.find(SeriesKey{k, r});
  if (!q || (q->resident && q->bars.empty())) return std::nullopt;
  return coverageOf(*q);
}
bool Snapshot::queueResident(const EdgeKey & k, Resolution r) const {
  const QueueState * q = queues_.find(SeriesKey{k, r});
  return q && q->resident;
}
std::vector<Resolution> Snapshot::queues(const EdgeKey & k) const {
  std::vector<Resolution> out;
  for (int r = 0; r <= static_cast<int>(Resolution::Quarter); r++)
    if (coverage(k, static_cast<Resolution>(r))) out.push_back(static_cast<Resolution>(r));
  return out;
}
int64_t Snapshot::keep(DatasetId d, Resolution r) const {
  if (const auto * rules = retention_.find(d)) return (*rules)[static_cast<size_t>(r)];
  static const std::array<int64_t, 11> defaults = defaultRules();
  return defaults[static_cast<size_t>(r)];
}

void Snapshot::setRetention(DatasetId d, Resolution r, std::optional<std::chrono::seconds> keep) {
  if (!d.valid()) return;
  std::array<int64_t, 11> rules = retention_.find(d) ? *retention_.find(d) : defaultRules();
  const int64_t seconds = keep ? keep->count() : 0;
  if (rules[static_cast<size_t>(r)] == seconds) return;
  rules[static_cast<size_t>(r)] = seconds;
  retention_ = retention_.set(d, rules);
  checks_++;  // a rule, not something shown: published, no version
}

// An edge is a row of the table once it has a last price or a daily queue.
void Snapshot::noteRow(const EdgeKey & k) {
  if (rows_.count(k)) return;
  const Quote * q = quotes_.find(k);
  const bool row = (q && isDefined(q->last)) || coverage(k, Resolution::Day);
  if (!row) return;
  rows_ = rows_.insert(k);
  quoteRows_ = quoteRows_.push_back(k);
}

void Snapshot::setQuote(const EdgeKey & k, const Quote & in) {
  require(k);
  if (!in.says()) return;
  const Quote * old = quotes_.find(k);
  if (old && in.time != 0 && old->time != 0 && in.time < old->time) return;  // the newest says
  if (old && *old == in) return;
  quotes_ = quotes_.set(k, in);
  dirtyEdges_.insert(k);
  addEdge(k);
  noteRow(k);
  version_++;
  refreshChanges(k);
}

Changes Snapshot::changes(const EdgeKey & k) const {
  const Changes * c = changes_.find(k);
  return c ? *c : Changes{};
}

void Snapshot::setChanges(const EdgeKey & k, const Changes & in) {
  require(k);
  if (!in.says()) return;
  Changes now = changes(k);
  for (size_t i = 0; i < kChangeCount; i++)
    if (isDefined(in.pct[i])) now.pct[i] = in.pct[i];
  if (const Changes * old = changes_.find(k); old ? *old != now : true) {
    changes_ = changes_.set(k, now);
    dirtyEdges_.insert(k);
    addEdge(k);
    version_++;
  }
  refreshChanges(k);
}

Ranges Snapshot::ranges(const EdgeKey & k) const {
  const Ranges * r = ranges_.find(k);
  return r ? *r : Ranges{};
}

void Snapshot::setRanges(const EdgeKey & k, const Ranges & in) {
  require(k);
  if (!in.says()) return;
  Ranges now = ranges(k);
  for (size_t i = 0; i < kChangeCount; i++) {
    if (isDefined(in.high[i])) now.high[i] = in.high[i];
    if (isDefined(in.low[i])) now.low[i] = in.low[i];
  }
  if (const Ranges * old = ranges_.find(k); old ? *old != now : true) {
    ranges_ = ranges_.set(k, now);
    dirtyEdges_.insert(k);
    addEdge(k);
    version_++;
  }
  refreshChanges(k);
}

void Snapshot::refreshChanges(const EdgeKey & k) {
  Changes now = changes(k);
  Ranges range = ranges(k);
  bool anyChange = false, anyRange = false;
  for (size_t p = 0; p < kChangeCount; p++) {
    const PeriodView v = periodView(*this, k, static_cast<Period>(p), false);
    const double c = v.change();
    if (isDefined(c) && !(now.pct[p] == c)) {
      now.pct[p] = c;
      anyChange = true;
    }
    if (isDefined(v.high) && !(range.high[p] == v.high)) {
      range.high[p] = v.high;
      anyRange = true;
    }
    if (isDefined(v.low) && !(range.low[p] == v.low)) {
      range.low[p] = v.low;
      anyRange = true;
    }
  }
  if (!anyChange && !anyRange) return;
  if (anyChange) changes_ = changes_.set(k, now);
  if (anyRange) ranges_ = ranges_.set(k, range);
  dirtyEdges_.insert(k);
  version_++;
}

// The summary of the bars a source writes: the quote moves to the newest bar if that is newer than it, and the queue of the resolution takes
// the bars. True where either changed.
bool Snapshot::deriveSummary(const EdgeKey & k, Resolution r, const std::vector<Candle> & in, QueueKind kind) {
  bool changed = false;
  // A bar newer than the quote sets its last price and its time (the bid and the ask stay): a source that gives only bars gives a price.
  const Candle * newest = nullptr;
  for (const Candle & c : in)
    if (std::isfinite(c.close) && (!newest || c.time >= newest->time)) newest = &c;
  if (newest) {
    const int64_t instant = summary::instantOf(r, newest->time);
    const Quote * old = quotes_.find(k);
    // A quote that says no time is a source's own, and no bar is older than it that we can tell.
    const bool newer = !old || (old->time != 0 ? instant > old->time : !isDefined(old->last));
    if (newer) {
      Quote q = old ? *old : Quote{};
      q.last = newest->close;
      q.time = instant;
      if (!old || !(*old == q)) {
        quotes_ = quotes_.set(k, q);
        dirtyEdges_.insert(k);
        changed = true;
      }
    }
  }
  const int64_t window = keep(k.dataset, r);
  if (window > 0) {
    const SeriesKey key{k, r};
    const QueueState * old = queues_.find(key);
    QueueState state;
    // A queue of the other kind starts over with this one (what the writer says, never mixed): what it had, in memory or on the disk, is gone.
    const bool restart = old && old->kind != kind && std::any_of(in.begin(), in.end(), summary::fits);
    if (restart) {
      queueBytes_ -= std::min(queueBytes_, old->bars.size() * kBarBytes);
      state.kind = kind;
      state.resident = true;
      old = nullptr;
      mergeQueues_.erase(key);
      touchQueues_.erase(key);
      queues_ = queues_.erase(key);
    } else if (old) {
      state = *old;
    } else {
      state.kind = kind;
      state.resident = !restoring_;
      state.unknown = restoring_;
    }
    // The bars that stand: those of the window of what there is and what is said, as stored.
    int64_t last = state.bars.empty() ? std::numeric_limits<int64_t>::min() : summary::instantOf(r, state.bars.back().time);
    if (!state.resident && state.disk.count) last = std::max(last, summary::instantOf(r, state.disk.last_key));
    std::vector<Candle> said;
    for (const Candle & c : in)
      if (summary::fits(c)) said.push_back(summary::normalized(state.kind, c));
    for (const Candle & c : said) last = std::max(last, summary::instantOf(r, c.time));
    std::vector<Candle> put;  // what changed the queue: for the merge of one that is not in memory
    bool queueChanged = restart;
    Seq<Candle> bars = state.bars;
    for (const Candle & c : said) {
      if (summary::instantOf(r, c.time) < last - window) continue;  // older than the window: no bar of it
      // The file's last bar said again: told by its digest, so that a restart does not write the queue of every edge at its first refresh.
      if (!state.resident && !state.unknown && state.disk.count && c.time == state.disk.last_key && summary::hashOf(state.kind, c) == state.disk.last_hash &&
          std::none_of(state.bars.begin(), state.bars.end(), [&](const Candle & p) { return p.time == c.time; }))
        continue;
      bool one = false;
      bars = upsert(bars, c, [](const Candle &, const Candle &) { return true; }, summary::sameBar, one);
      if (one) {
        queueChanged = true;
        put.push_back(c);
      }
    }
    if (queueChanged) {
      bars = cutToWindow(bars, r, window, queueChanged);
      queueBytes_ += bars.size() * kBarBytes;
      queueBytes_ -= std::min(queueBytes_, state.bars.size() * kBarBytes);
      state.bars = std::move(bars);
      state.fetched = now_;
      queues_ = queues_.set(key, std::move(state));
      dirtyEdges_.insert(k);
      if (queues_.find(key)->resident) {
        putQueues_.insert(key);
      } else {
        auto & waiting = mergeQueues_[key];
        for (const Candle & c : put) summary::put(waiting, c);
      }
      touchQueues_.erase(key);
      changed = true;
    } else if (old && !said.empty() && (recheck_.count() <= 0 || now_ - std::max(old->fetched, old->disk.fetched) >= recheck_.count())) {
      // Said again and the same, a fetch that went through: it was fetched now. As a fact said again is, to within the recheck interval, and
      // with no version: nothing that is shown changes.
      QueueState touched = *old;
      touched.fetched = now_;
      queues_ = queues_.set(key, std::move(touched));
      touchQueues_[key] = now_;
      checks_++;
    }
  }
  if (changed) {
    noteRow(k);
    refreshChanges(k);
  }
  return changed;
}

void Snapshot::loadEdges(const std::vector<StoredEdge> & edges) {
  bool any = false;
  for (const StoredEdge & e : edges) {
    const EdgeKey & k = e.listing.key;
    if (!k.base.valid() || !k.quote.valid() || !k.market.valid() || !k.dataset.valid()) continue;
    any = true;
    if (e.hasListing()) putListing(e.listing, true);
    if (e.quote.says()) {
      // What a source has said is newer than the disk, and the disk's is taken only where nothing said is as new.
      const Quote * old = quotes_.find(k);
      if (!old || (e.quote.time != 0 && old->time != 0 && e.quote.time > old->time)) quotes_ = quotes_.set(k, e.quote);
    }
    if (e.changes.says() && !changes_.find(k)) changes_ = changes_.set(k, e.changes);  // what a source has said is newer
    if (e.ranges.says() && !ranges_.find(k)) ranges_ = ranges_.set(k, e.ranges);
    if (isDefined(e.previous_close) && !previousClose_.find(k)) previousClose_ = previousClose_.set(k, {e.previous_close_day, e.previous_close});
    for (const auto & [r, c] : e.queues) {
      const SeriesKey key{k, r};
      const QueueState * old = queues_.find(key);
      if (!old) {
        QueueState state;
        state.kind = c.kind;
        state.fetched = c.fetched;
        state.resident = false;
        state.disk = c;
        queues_ = queues_.set(key, std::move(state));
      } else if (old->unknown) {
        QueueState state = *old;
        state.unknown = false;
        if (c.kind == state.kind) {
          state.disk = c;
        } else {
          // The file's queue is of the other kind than the one a source has written since: it starts over, and what was said is the whole of it.
          state.resident = true;
          putQueues_.insert(key);
          mergeQueues_.erase(key);
        }
        queues_ = queues_.set(key, std::move(state));
      }
    }
    addEdge(k);
    noteRow(k);
  }
  if (any) version_++;
}

void Snapshot::settleRestored() {
  std::vector<SeriesKey> unknown;
  for (const auto & [key, state] : queues_)
    if (state.unknown) unknown.push_back(key);
  for (const SeriesKey & key : unknown) {
    QueueState state = *queues_.find(key);
    state.unknown = false;
    state.resident = true;  // the file has none: what was said is the whole of it
    queues_ = queues_.set(key, std::move(state));
    putQueues_.insert(key);
    mergeQueues_.erase(key);
  }
  if (!unknown.empty()) checks_++;
}

void Snapshot::loadQueue(const StoredQueue & in) {
  const SeriesKey key{in.edge, in.resolution};
  const QueueState * old = queues_.find(key);
  if (old && old->resident) return;  // made resident by a write or another read since
  if (old && old->kind != in.kind) return;  // a writer has said the other kind since: the file's queue is of the old one and is not mixed in
  QueueState state;
  if (old) state = *old;
  state.kind = in.kind;
  const int64_t window = keep(in.edge.dataset, in.resolution);
  std::vector<Candle> merged;
  for (const Candle & c : in.bars)
    if (summary::fits(c)) merged.push_back(summary::normalized(in.kind, c));
  for (const Candle & c : state.bars) summary::put(merged, c);  // what was said since wins
  summary::trim(merged, in.resolution, window);
  queueBytes_ -= std::min(queueBytes_, state.bars.size() * kBarBytes);
  state.bars = Seq<Candle>(merged.begin(), merged.end());
  queueBytes_ += state.bars.size() * kBarBytes;
  state.fetched = std::max(state.fetched, in.fetched);
  state.resident = true;
  state.unknown = false;
  queues_ = queues_.set(key, std::move(state));
  noteRow(in.edge);
  version_++;
  refreshChanges(in.edge);
}

void Snapshot::evictQueue(const EdgeKey & k, Resolution r) {
  const SeriesKey key{k, r};
  const QueueState * old = queues_.find(key);
  if (!old || !old->resident) return;
  QueueState state = *old;
  state.disk = coverageOf(*old);
  state.resident = false;
  queueBytes_ -= std::min(queueBytes_, state.bars.size() * kBarBytes);
  state.bars = {};
  queues_ = queues_.set(key, std::move(state));
  checks_++;  // published so that the memory is freed, but nothing that is shown changes
}

BarBatch Snapshot::takeBars() {
  BarBatch out;
  for (auto & [key, bars] : pendingBars_) out.series.push_back({key.edge, key.resolution, std::move(bars)});
  pendingBars_.clear();
  return out;
}

SummaryBatch Snapshot::takeSummaries() {
  SummaryBatch out;
  const auto hasQueueOp = [&](const EdgeKey & k) {
    for (int r = 0; r <= static_cast<int>(Resolution::Quarter); r++)
      if (putQueues_.count(SeriesKey{k, static_cast<Resolution>(r)}) || mergeQueues_.count(SeriesKey{k, static_cast<Resolution>(r)})) return true;
    return false;
  };
  for (const EdgeKey & k : dirtyEdges_) {
    const Quote * q = quotes_.find(k);
    const auto * previous = previousClose_.find(k);
    const Changes * changes = changes_.find(k);
    const Ranges * ranges = ranges_.find(k);
    if (!q && !previous && !changes && !ranges && !hasQueueOp(k)) continue;  // nothing of a summary yet: an edge with a listing and books is not kept
    StoredEdge e;
    if (const Listing * l = listings_.find(k)) e.listing = *l;
    e.listing.key = k;
    if (q) e.quote = *q;
    if (changes) e.changes = *changes;
    if (ranges) e.ranges = *ranges;
    if (previous) {
      e.previous_close_day = previous->first;
      e.previous_close = previous->second;
    }
    out.edges.push_back(std::move(e));
  }
  for (const SeriesKey & key : putQueues_) {
    const QueueState * q = queues_.find(key);
    if (!q || !q->resident) continue;
    StoredQueue s;
    s.edge = key.edge;
    s.resolution = key.resolution;
    s.kind = q->kind;
    s.fetched = q->fetched;
    s.bars.assign(q->bars.begin(), q->bars.end());
    out.queues.push_back(std::move(s));
  }
  for (auto & [key, bars] : mergeQueues_) {
    const QueueState * q = queues_.find(key);
    StoredQueue s;
    s.edge = key.edge;
    s.resolution = key.resolution;
    s.kind = q ? q->kind : QueueKind::Candle;
    s.fetched = q ? q->fetched : 0;
    s.merge = true;
    s.keep = keep(key.edge.dataset, key.resolution);
    s.bars = std::move(bars);
    out.queues.push_back(std::move(s));
  }
  for (const auto & [key, fetched] : touchQueues_) {
    if (putQueues_.count(key) || mergeQueues_.count(key)) continue;
    const QueueState * q = queues_.find(key);
    if (!q) continue;
    StoredQueue s;
    s.edge = key.edge;
    s.resolution = key.resolution;
    s.kind = q->kind;
    s.fetched = fetched;
    s.merge = true;  // with no bars: only the time moves
    s.keep = keep(key.edge.dataset, key.resolution);
    out.queues.push_back(std::move(s));
  }
  touchQueues_.clear();
  dirtyEdges_.clear();
  putQueues_.clear();
  mergeQueues_.clear();
  return out;
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

namespace {
bool whole(const GroupKey & k) { return k.base.valid() && k.quote.valid() && k.market.valid(); }
StoredMember storedMember(const std::string & list, const GroupKey & k) { return {list, k.base.value, k.quote.value, k.market.value}; }
}  // namespace

std::vector<GroupKey> Snapshot::listMembers(const std::string & list) const { return sorted(lists_.find(list)); }
bool Snapshot::onList(const std::string & list, const GroupKey & k) const {
  const auto * members = lists_.find(list);
  return members && members->count(k) > 0;
}

void Snapshot::addToList(const std::string & list, const std::vector<GroupKey> & edges) {
  if (list.empty()) throw std::invalid_argument("a list needs a code");
  for (const GroupKey & k : edges)
    if (!whole(k)) throw std::invalid_argument("a group on a list needs an instrument, a currency and a market");
  const auto * old = lists_.find(list);
  immer::set<GroupKey> members = old ? *old : immer::set<GroupKey>{};
  bool changed = false;
  for (const GroupKey & k : edges) {
    if (members.count(k)) continue;
    members = members.insert(k);
    changed = true;
    std::erase_if(outbox_.members_removed, [&](const StoredMember & m) { return m == storedMember(list, k); });
    outbox_.members.push_back(storedMember(list, k));
  }
  if (!changed) return;
  lists_ = lists_.set(list, members);
  version_++;
}

void Snapshot::removeFromList(const std::string & list, const std::vector<GroupKey> & edges) {
  const auto * old = lists_.find(list);
  if (!old) return;
  immer::set<GroupKey> members = *old;
  bool changed = false;
  for (const GroupKey & k : edges) {
    if (!members.count(k)) continue;
    members = members.erase(k);
    changed = true;
    std::erase_if(outbox_.members, [&](const StoredMember & m) { return m == storedMember(list, k); });
    outbox_.members_removed.push_back(storedMember(list, k));
  }
  if (!changed) return;
  lists_ = members.size() ? lists_.set(list, members) : lists_.erase(list);
  version_++;
}

void Snapshot::loadMembers(const std::vector<StoredMember> & stored) {
  for (const StoredMember & m : stored) {
    const GroupKey k{MarketId{m.market}, InstrumentId{m.base}, InstrumentId{m.quote}};
    if (m.list.empty() || !whole(k) || NodeKey{m.market}.kind() != NodeKind::Market) continue;
    const auto * old = lists_.find(m.list);
    lists_ = lists_.set(m.list, (old ? *old : immer::set<GroupKey>{}).insert(k));
  }
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
  void appendCandles(const EdgeKey & k, Resolution r, const std::vector<Candle> & c, QueueKind kind) override { s_.appendCandles(k, r, c, kind); }
  void setPreviousClose(const EdgeKey & k, int64_t day, double close) override { s_.setPreviousClose(k, day, close); }
  void setQuote(const EdgeKey & k, const Quote & q) override { s_.setQuote(k, q); }
  void setChanges(const EdgeKey & k, const Changes & c) override { s_.setChanges(k, c); }
  void setRanges(const EdgeKey & k, const Ranges & r) override { s_.setRanges(k, r); }
  void appendBooks(const EdgeKey & k, const std::vector<BookSnapshot> & b) override { s_.appendBooks(k, b); }
  void appendNews(DatasetId d, const std::vector<NewsItem> & n) override { s_.appendNews(d, n); }
  void appendActions(const EventKey & k, const std::vector<CorporateAction> & a) override { s_.appendActions(k, a); }
  void removeAttribute(NodeRef n, const std::string & p) override { s_.removeAttribute(n, p); }
  void removePortfolio(PortfolioId id) override { s_.removePortfolio(id); }
  void appendTransactions(const LedgerKey & k, const std::vector<portfolio::Transaction> & t) override { s_.appendTransactions(k, t); }
  void removeTransactions(const LedgerKey & k, const std::vector<uint64_t> & ids) override { s_.removeTransactions(k, ids); }
  void addToList(const std::string & list, const std::vector<GroupKey> & e) override { s_.addToList(list, e); }
  void removeFromList(const std::string & list, const std::vector<GroupKey> & e) override { s_.removeFromList(list, e); }

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
    budget_ = options.queue_budget;
    changed_ = std::move(options.changed);
    background_ = options.restore == StoreOptions::Restore::InBackground;
    facts_ = options.facts;
    if (options.summaries) summary_ = std::make_unique<SummaryWriter>(std::move(options.summaries), options.summary_writer);
    if (options.bars) bars_ = std::make_unique<BarWriter>(std::move(options.bars), options.bar_writer);
    // The user's own file is small and what the program shows first: read here, before the constructor returns. A portfolio is in its own
    // file and never in the cache: one that is found there anyway is not read. What was kept of the nodes and the edges (the cache and the
    // summaries) is read before any source writes, or, in the background, newer than the sources' words are not allowed to stand over.
    Snapshot first;
    first.setInventIsins(options.invent_isins);
    if (portfolios_) {
      std::vector<StoredFact> mine;
      for (StoredFact & f : portfolios_->loadAll())
        if (NodeKey{f.node}.kind() == NodeKind::Portfolio) mine.push_back(std::move(f));
      first.load(std::move(mine));
      first.loadEvents(portfolios_->loadEvents());
      first.loadMembers(portfolios_->loadMembers());
    }
    const bool restore = facts_ || summary_;
    first.setRestoring(restore && background_);
    tell(first.takeNotices());
    current_.store(std::make_shared<const Snapshot>(std::move(first)));
    if (options.facts) writer_ = std::make_unique<FactWriter>(std::move(options.facts), options.writer);
    if (summary_) reader_ = std::thread([this] { readLoop(); });
    if (!restore) return;
    if (background_) {
      restore_running_ = true;
      restorer_ = std::thread([this] { restoreFiles(); });
    } else {
      restoreFiles();
    }
  }

  ~MemoryStore() override {
    stopping_ = true;
    if (restorer_.joinable()) restorer_.join();
    {
      std::lock_guard lock(want_mutex_);
      stop_ = true;
    }
    read_wake_.notify_all();
    if (reader_.joinable()) reader_.join();
  }

  bool restoring() const override { return restore_running_ || late_pending_ > 0; }
  void waitRestored() override {
    std::unique_lock lock(restore_mutex_);
    restored_.wait(lock, [&] { return !restore_running_; });
  }

  void flush() override {
    if (writer_) writer_->flush();
    if (summary_) summary_->flush();
    if (bars_) bars_->flush();
  }

  uint64_t failedWrites() const override {
    return (writer_ ? writer_->failed() : 0) + (summary_ ? summary_->failed() : 0) + (bars_ ? bars_->failed() : 0);
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
  void appendCandles(const EdgeKey & k, Resolution r, const std::vector<Candle> & c, QueueKind kind) override {
    write([&](Snapshot & s) { s.appendCandles(k, r, c, kind); });
  }
  void setPreviousClose(const EdgeKey & k, int64_t day, double close) override {
    write([&](Snapshot & s) { s.setPreviousClose(k, day, close); });
  }
  void setQuote(const EdgeKey & k, const Quote & q) override {
    write([&](Snapshot & s) { s.setQuote(k, q); });
  }
  void setChanges(const EdgeKey & k, const Changes & c) override {
    write([&](Snapshot & s) { s.setChanges(k, c); });
  }
  void setRanges(const EdgeKey & k, const Ranges & r) override {
    write([&](Snapshot & s) { s.setRanges(k, r); });
  }
  void setRetention(DatasetId d, Resolution r, std::optional<std::chrono::seconds> keep) override {
    write([&](Snapshot & s) { s.setRetention(d, r, keep); });
  }

  void want(const std::vector<SeriesKey> & keys) override {
    if (!summary_) return;
    const std::shared_ptr<const Snapshot> snapshot = current_.load();  // never waits for a writer
    bool any = false;
    {
      std::lock_guard lock(want_mutex_);
      pinned_.clear();
      to_read_.clear();
      last_want_ = keys;
      for (const SeriesKey & key : keys) {
        pinned_.insert(key);
        if (snapshot->queueResident(key.edge, key.resolution)) {
          touch(key);
        } else if (const auto c = snapshot->coverage(key.edge, key.resolution); c && c->count > 0 && !reading_.count(key)) {
          to_read_.push_back(key);
          any = true;
        }
      }
    }
    if (any) read_wake_.notify_all();
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
  void addToList(const std::string & list, const std::vector<GroupKey> & e) override {
    write([&](Snapshot & s) { s.addToList(list, e); });
  }
  void removeFromList(const std::string & list, const std::vector<GroupKey> & e) override {
    write([&](Snapshot & s) { s.removeFromList(list, e); });
  }

 private:
  // The queue as the file has it with what is waiting to be written put over it (read before the file, so that nothing said is older than
  // what is read).
  StoredQueue readQueue(const SeriesKey & key, const std::shared_ptr<const Snapshot> & snapshot) {
    StoredQueue base;
    base.edge = key.edge;
    base.resolution = key.resolution;
    if (const auto c = snapshot->coverage(key.edge, key.resolution)) base.kind = c->kind;
    std::vector<StoredQueue> waiting = summary_->pending(key.edge, key.resolution);
    if (auto store = summary_->store(key.edge.dataset))
      if (auto q = store->readQueue(key.edge, key.resolution)) base = std::move(*q);
    for (StoredQueue & op : waiting) summary::fold(base, std::move(op));
    return base;
  }

  // The thread that reads the queues that are wanted and publishes them into the store: never the thread that draws.
  void readLoop() {
    std::unique_lock lock(want_mutex_);
    for (;;) {
      read_wake_.wait(lock, [&] { return stop_ || !to_read_.empty() || !late_nodes_.empty(); });
      if (stop_) return;
      if (!late_nodes_.empty()) {
        std::vector<uint64_t> nodes;
        while (!late_nodes_.empty() && nodes.size() < 500) {
          nodes.push_back(late_nodes_.front());
          late_nodes_.pop_front();
        }
        lock.unlock();
        std::vector<StoredFact> facts = facts_->loadNodes(nodes);
        std::erase_if(facts, [](const StoredFact & f) { return NodeKey{f.node}.kind() == NodeKind::Portfolio; });
        if (!facts.empty()) {
          write([&](Snapshot & s) { s.load(std::move(facts)); });
          if (changed_) changed_();
        }
        lock.lock();
        late_pending_ -= nodes.size();
        continue;
      }
      std::vector<SeriesKey> batch;
      while (!to_read_.empty() && batch.size() < 16) {
        batch.push_back(to_read_.front());
        to_read_.pop_front();
        reading_.insert(batch.back());
      }
      lock.unlock();
      const std::shared_ptr<const Snapshot> snapshot = current_.load();
      std::vector<StoredQueue> read;
      for (const SeriesKey & key : batch) read.push_back(readQueue(key, snapshot));
      // One publication for the batch: nothing is written back, since it came from there.
      write([&](Snapshot & s) {
        for (const StoredQueue & q : read) {
          s.loadQueue(q);
          std::lock_guard w(want_mutex_);
          touch(SeriesKey{q.edge, q.resolution});
        }
      });
      if (changed_) changed_();
      lock.lock();
      for (const SeriesKey & key : batch) reading_.erase(key);
    }
  }

  // An edge that is new has ends whose facts the restore did not read (it reads those of the edges the disk had): a source made it, and
  // what the disk knows of its instrument (what a slow loader fetched, a description) is read now, on the reader's thread, and what the
  // source has said stands. Without summaries every fact was read at the start.
  void noteNewEdges(const std::vector<EdgeKey> & made) {
    if (!summary_ || !facts_ || made.empty()) return;
    bool any = false;
    {
      std::lock_guard lock(want_mutex_);
      for (const EdgeKey & k : made)
        for (const uint64_t node : {k.base.value, k.quote.value, k.market.value, k.dataset.value})
          if (node && loaded_nodes_.insert(node).second) {
            late_nodes_.push_back(node);
            late_pending_++;
            any = true;
          }
    }
    if (any) read_wake_.notify_all();
  }

  // The queue was used now (under `want_mutex_`): the last to be dropped.
  void touch(const SeriesKey & key) {
    auto it = stamp_of_.find(key);
    if (it != stamp_of_.end()) by_stamp_.erase(it->second);
    stamp_of_[key] = ++clock_stamp_;
    by_stamp_[clock_stamp_] = key;
  }

  // Over the budget, the queues in memory that were wanted the longest ago go (never one that is wanted now); the file has them, or the
  // writer is about to. Under the writer lock, on the copy being made.
  void evict(Snapshot & next, const SummaryBatch & written) {
    {
      std::lock_guard lock(want_mutex_);
      for (const StoredQueue & q : written.queues) {
        const SeriesKey key{q.edge, q.resolution};
        if (!q.merge && !stamp_of_.count(key)) touch(key);
      }
      if (next.queueBytes() <= budget_) return;
      const size_t target = budget_ - budget_ / 10;
      for (auto it = by_stamp_.begin(); it != by_stamp_.end() && next.queueBytes() > target;) {
        const SeriesKey key = it->second;
        if (pinned_.count(key)) {
          ++it;
          continue;
        }
        next.evictQueue(key.edge, key.resolution);
        stamp_of_.erase(key);
        it = by_stamp_.erase(it);
      }
    }
  }

  // What the portfolios hold, by key: restored first.
  struct Held {
    std::unordered_set<uint64_t> instruments, currencies, nodes;
  };
  static Held heldBy(const Snapshot & snapshot) {
    Held held;
    for (PortfolioId id : snapshot.portfolios()) {
      if (const auto currency = portfolio::currencyOf(snapshot, id)) {
        held.currencies.insert(currency->value);
        held.nodes.insert(currency->value);
      }
      for (const portfolio::Position & p : portfolio::holdings(snapshot, id).positions) {
        held.instruments.insert(p.instrument.value);
        held.currencies.insert(p.quote.value);
        held.nodes.insert({p.instrument.value, p.quote.value, p.market.value});
      }
    }
    return held;
  }

  // Reads what an earlier run kept, in chunks, each published as it is read: the facts of the nodes and the edges (with their quotes and
  // the coverage of their queues). What the portfolios hold comes first (their instruments' facts, listings and quotes, and the currencies
  // they are valued through), then the rest, each chunk's facts before its edges so that a row is never seen before what it is called.
  // The facts read are those of the nodes that have an edge; where no summaries are kept there are no edges, so all of them are read.
  // Runs on a thread of its own, or in the constructor.
  void restoreFiles() {
    try {
      constexpr size_t kChunk = 2000;
      if (summary_) {
        const Held held = heldBy(*current_.load());
        std::vector<StoredEdge> edges;
        for (const std::string & code : summary_->datasets())
          if (const auto dataset = datasetKey(code))
            if (auto store = summary_->store(DatasetId{dataset->value}))
              for (StoredEdge & e : store->loadEdges()) edges.push_back(std::move(e));
        const auto isHeld = [&](const StoredEdge & e) { return held.instruments.count(e.listing.key.base.value) || held.currencies.count(e.listing.key.base.value); };
        const size_t heldEnd = static_cast<size_t>(std::stable_partition(edges.begin(), edges.end(), isHeld) - edges.begin());
        for (size_t from = 0; from < edges.size() || (from == 0 && !held.nodes.empty());) {
          if (stopping_) return;
          const size_t to = from < heldEnd ? heldEnd : std::min(edges.size(), from + kChunk);
          std::vector<uint64_t> nodes;
          {
            std::lock_guard lock(want_mutex_);
            const auto want = [&](uint64_t node) {
              if (node && loaded_nodes_.insert(node).second) nodes.push_back(node);
            };
            if (from == 0)
              for (uint64_t node : held.nodes) want(node);
            for (size_t i = from; i < to; i++) {
              const EdgeKey & k = edges[i].listing.key;
              want(k.base.value);
              want(k.quote.value);
              want(k.market.value);
              want(k.dataset.value);
            }
          }
          std::vector<StoredFact> facts = facts_ ? facts_->loadNodes(nodes) : std::vector<StoredFact>{};
          std::erase_if(facts, [](const StoredFact & f) { return NodeKey{f.node}.kind() == NodeKind::Portfolio; });
          std::vector<StoredEdge> chunk(std::make_move_iterator(edges.begin() + static_cast<std::ptrdiff_t>(from)),
                                        std::make_move_iterator(edges.begin() + static_cast<std::ptrdiff_t>(to)));
          publish([&](Snapshot & s) {
            s.load(std::move(facts));
            s.loadEdges(chunk);
          });
          if (to == from) break;
          from = to;
        }
      } else if (facts_) {
        std::vector<StoredFact> all = facts_->loadAll();
        std::erase_if(all, [](const StoredFact & f) { return NodeKey{f.node}.kind() == NodeKind::Portfolio; });
        constexpr size_t kFacts = 100'000;
        for (size_t from = 0; from < all.size();) {
          if (stopping_) return;
          const size_t to = std::min(all.size(), from + kFacts);
          std::vector<StoredFact> chunk(std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(from)),
                                        std::make_move_iterator(all.begin() + static_cast<std::ptrdiff_t>(to)));
          publish([&](Snapshot & s) { s.load(std::move(chunk)); });
          from = to;
        }
      }
    } catch (...) {
      // Whatever went wrong with a file, the store runs with what it has: the stores say what they could not do themselves.
    }
    write([&](Snapshot & s) {
      s.settleRestored();
      s.setRestoring(false);
    });
    {
      std::lock_guard lock(restore_mutex_);
      restore_running_ = false;
    }
    restored_.notify_all();
    if (background_ && changed_) changed_();
    rewant();
  }

  // One chunk of the restore, published; the UI is told, and what it wants is looked at again (the edges are there now).
  template <class F>
  void publish(F change) {
    write(change);
    if (background_ && changed_) changed_();
    rewant();
  }

  void rewant() {
    std::vector<SeriesKey> keys;
    {
      std::lock_guard lock(want_mutex_);
      keys = last_want_;
    }
    if (!keys.empty()) want(keys);
  }

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
      mine.members = std::move(facts.members);
      mine.members_removed = std::move(facts.members_removed);
      const auto portfolioFact = [](const StoredFact & f) { return NodeKey{f.node}.kind() == NodeKind::Portfolio; };
      for (auto * list : {&facts.changed, &facts.rechecked}) {
        auto & to = list == &facts.changed ? mine.changed : mine.rechecked;
        for (StoredFact & f : *list)
          if (portfolioFact(f)) to.push_back(f);
        std::erase_if(*list, portfolioFact);  // what is left is the sources'
      }
      if (portfolios_ && !mine.empty()) portfolios_->apply(mine);
      if (writer_) writer_->post(std::move(facts));
      noteNewEdges(next.takeNewEdges());
      {
        BarBatch bars = next.takeBars();
        if (bars_) bars_->post(std::move(bars));
      }
      SummaryBatch summaries = next.takeSummaries();
      if (summary_) {
        evict(next, summaries);
        summary_->post(std::move(summaries));
      }
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
  std::unique_ptr<FactWriter> writer_;  // its thread ends first, with everything written
  std::unique_ptr<SummaryWriter> summary_;
  std::unique_ptr<BarWriter> bars_;  // ends before the summaries
  std::shared_ptr<FactStore> facts_;  // read by the restore
  bool background_ = false;
  std::atomic<bool> stopping_{false}, restore_running_{false};
  std::mutex restore_mutex_;
  std::condition_variable restored_;
  std::vector<SeriesKey> last_want_;  // under want_mutex_
  std::unordered_set<uint64_t> loaded_nodes_;  // the nodes whose facts are read, or are about to be (under want_mutex_)
  std::deque<uint64_t> late_nodes_;            // the ends of edges a source made, to read
  std::atomic<size_t> late_pending_{0};
  size_t budget_ = kDefaultQueueBudget;
  std::function<void()> changed_;
  // What is wanted and what is read (under `want_mutex_`, which is taken inside the writer lock and never the other way round).
  std::mutex want_mutex_;
  std::condition_variable read_wake_;
  std::set<SeriesKey> pinned_;           // wanted now: not dropped
  std::deque<SeriesKey> to_read_;        // wanted, not in memory, not being read
  std::set<SeriesKey> reading_;          // being read
  std::map<SeriesKey, uint64_t> stamp_of_;  // the queues in memory by when they were last wanted
  std::map<uint64_t, SeriesKey> by_stamp_;
  uint64_t clock_stamp_ = 0;
  bool stop_ = false;
  std::thread reader_;
  std::thread restorer_;  // last: it ends first
};

}  // namespace

std::unique_ptr<Store> makeMemoryStore() { return std::make_unique<MemoryStore>(StoreOptions{}); }
std::unique_ptr<Store> makeMemoryStore(StoreOptions options) { return std::make_unique<MemoryStore>(std::move(options)); }
