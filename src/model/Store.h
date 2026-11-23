#pragma once

#include "model/FactWriter.h"
#include "model/Facts.h"
#include "model/Types.h"

#include <immer/map.hpp>
#include <immer/set.hpp>

#include <functional>
#include <memory>
#include <utility>

// One immutable version of everything the application knows. A snapshot never changes: a reader that holds one reads it
// without locks, for as long as it likes, and sees one consistent world (the candles, books, announcements and
// actions as of one moment). Copying a snapshot is cheap and shares all of its storage; a write makes a new snapshot
// that shares almost everything with the one before. Slices a snapshot returns share storage with it too.
class Snapshot {
 public:
  // Which version of the store this is; each change that changed something makes the next number.
  uint64_t version() const { return version_; }

  // A node is what is said of it, and none of these is a record: each reads facts (an instrument's ISIN is its key).
  // Whether anything is said of a node.
  bool known(NodeRef) const;
  // The text said of a node under a predicate, else empty.
  std::string said(NodeRef, const std::string & predicate) const;
  std::string symbol(InstrumentId) const;  // the symbol said of the instrument itself (a currency's code); the ones markets give are on edges
  InstrumentKind kind(InstrumentId) const;
  std::string isin(InstrumentId) const;  // an instrument with an ISIN is that ISIN; empty for a currency or a unit
  // By a symbol said of the instrument itself (a currency's code).
  std::optional<InstrumentId> findInstrument(const std::string & symbol) const;
  // The fat edge's listing: what a dataset says of the instrument on the market. Only an edge that has data has one.
  std::optional<Listing> listing(const EdgeKey &) const;
  // What the market calls the base instrument there, else the instrument's own symbol.
  std::string symbol(const EdgeKey &) const;
  // The first edge made that a market's `symbol` is for: the instrument a market calls "NOKIA", whoever said it.
  std::optional<EdgeKey> findEdge(const std::string & symbol, MarketId) const;
  // The object of one predicate of a node, if it is said: an instrument's icon, its number of shares, a market's country.
  std::optional<Value> attribute(NodeRef, const std::string & predicate) const;
  // The same with its time: when a source last said it (`updated`), whether or not the value was the same as before, in
  // milliseconds UTC as the store took it where the write happened.
  std::optional<Attribute> fact(NodeRef, const std::string & predicate) const;
  std::optional<InstrumentId> findInstrumentByIsin(const std::string & isin) const;  // if anything is said of it
  // A market is its key: found by what is said of it, and named by a fact.
  std::optional<MarketId> findMarket(const std::string & name) const;  // by its name attribute
  std::optional<MarketId> findMarketByAttribute(const std::string & predicate, const std::string & value) const;
  // The name said of a node (a market's, an instrument's); a dataset's is its code until a name is said. Empty where none is.
  std::string name(NodeRef) const;
  // What is said about any node, in the order it was first said, with the times of each; and the node, of the type, that says the text `predicate`
  // is `value` (the one made first, if several do). Only text is found by its value.
  std::vector<Attribute> attributes(NodeRef) const;
  std::optional<NodeRef> findByAttribute(NodeType, const std::string & predicate, const std::string & value) const;
  // The dataset with this code, if anything is said of it.
  std::optional<DatasetId> findDataset(const std::string & code) const;
  // The markets that have an edge, by name and then by key.
  std::vector<MarketId> edgeMarkets() const;
  // How many nodes of a type have anything said of them: what a test or a diagnostic wants, and no view of the nodes.
  size_t nodeCount(NodeType) const;

  // The edges that have any data (candles or books) in a market: what a preset needs. Sorted by key, which means nothing: a
  // reader that shows them puts them in an order of its own. An instrument with no edge is not seen: nothing selects it.
  std::vector<EdgeKey> edges(MarketId) const;
  // The same, only those whose base instrument `keep` accepts (by its kind, its type, ...).
  std::vector<EdgeKey> edges(MarketId, const std::function<bool(InstrumentId)> & keep) const;

  // The bars of the edge at `resolution` in `range` (in slot numbers, or day numbers for a date resolution), oldest first;
  // an empty series if there are none.
  CandleSeries candles(const EdgeKey &, Resolution resolution, TimeRange range = {}) const;
  // The newest `n` bars at `resolution`, oldest first: the table's latest price, change and trace.
  CandleSeries lastCandles(const EdgeKey &, Resolution resolution, size_t n) const;
  // Whether the edge has any bars at `resolution`: cheap, for listing.
  bool hasCandles(const EdgeKey &, Resolution) const;
  // The resolutions the edge has bars in, shortest first.
  std::vector<Resolution> resolutions(const EdgeKey &) const;
  // The close before the session of `day` (a day number), as a provider says it: the base of a day's change. Only for the
  // day it was said for, so a value left from an earlier session is never used for a later one.
  std::optional<double> previousClose(const EdgeKey &, int64_t day) const;

  // The snapshots in `range`, oldest first.
  Seq<BookSnapshot> books(const EdgeKey &, TimeRange range = {}) const;
  // The snapshot standing at `time`: the latest at or before it.
  std::optional<BookSnapshot> bookAt(const EdgeKey &, double time) const;

  Seq<Announcement> announcements(const EventKey &, TimeRange range = {}) const;
  Seq<CorporateAction> actions(const EventKey &, TimeRange range = {}) const;
  // The datasets that have announcements or actions for an instrument. Sorted.
  std::vector<EventKey> events(InstrumentId) const;

  // Changes, for a store to make on its own copy before publishing it. Nothing that holds a published snapshot
  // can call them, because it holds it const. Each one that changes something bumps the version.
  InstrumentId addInstrument(const std::string & symbol, const std::string & name, InstrumentKind, const std::string & isin, const std::string & type);
  InstrumentId resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest);
  void setListing(const Listing &);
  MarketId resolveMarket(const std::vector<Attribute> &);
  void setAttribute(NodeRef, const Attribute &);
  void setAttributes(NodeRef, const std::vector<Attribute> &);
  DatasetId addDataset(const std::string & code, const std::string & name);
  void appendCandles(const EdgeKey &, Resolution, const std::vector<Candle> &);
  void setPreviousClose(const EdgeKey &, int64_t day, double close);
  void appendBooks(const EdgeKey &, const std::vector<BookSnapshot> &);
  void appendAnnouncements(const EventKey &, const std::vector<Announcement> &);
  void appendActions(const EventKey &, const std::vector<CorporateAction> &);

  // For a store to make on its own copy, with the changes above. `setTime` is the time of the write in progress: what
  // `updated` becomes. `load` gives it what an earlier run said: the facts of nodes, which makes no version and no node but
  // what is said. `revision` is the version plus the writes of facts that were only said again, which no version counts: a
  // store publishes a copy when it moves. `takeFacts` is what the writes since the last call ask of the disk.
  void setTime(TimestampMs now) { now_ = now; }
  void setKeyMaker(std::function<uint64_t()> * mint) { mint_ = mint; }  // for the keys of markets
  void load(const std::vector<StoredFact> &);
  uint64_t revision() const { return version_ + checks_; }
  FactBatch takeFacts() { return std::exchange(outbox_, FactBatch{}); }
  // What the writes since the last call refused to say, in words (`isUnique()`): a MIC or an id at a source that another market
  // already has is not given to a second one.
  std::vector<std::string> takeNotices() { return std::exchange(notices_, {}); }

 private:
  uint64_t version_ = 0;
  uint64_t checks_ = 0;
  TimestampMs now_ = 0;
  std::function<uint64_t()> * mint_ = nullptr;                    // set by the store for the length of a write
  FactBatch outbox_;                                              // what to keep, for the store that made this copy
  std::vector<std::string> notices_;                              // what was refused, for the store that made this copy
  immer::map<EdgeKey, Listing> listings_;
  immer::map<std::string, EdgeKey> edgeBySymbol_;  // "market \0 symbol" to the first edge there that has it
  immer::map<NodeRef, std::vector<Attribute>> attributes_;
  immer::map<std::string, immer::set<uint64_t>> byAttribute_;  // "type predicate \0 value" to the keys that say it
  immer::map<SeriesKey, Seq<Candle>> series_;
  immer::map<EdgeKey, immer::set<int>> resolutions_;
  immer::map<EdgeKey, std::pair<int64_t, double>> previousClose_;  // the session's day number and the close before it
  immer::map<EdgeKey, Seq<BookSnapshot>> books_;
  immer::map<MarketId, immer::set<EdgeKey>> edgesByMarket_;
  immer::map<EventKey, Seq<Announcement>> announcements_;
  immer::map<EventKey, Seq<CorporateAction>> actions_;
  immer::map<InstrumentId, immer::set<EventKey>> eventsByInstrument_;
};

// Where the snapshots come from, and the only way to change them. Data sources write through it and everything else
// reads a snapshot, so a store backed by a server can replace the in-memory one. Any thread may call anything: writes
// are serialised, and `snapshot()` never waits for one.
class Store {
 public:
  virtual ~Store() = default;

  // The current version. Hold it for as long as a consistent view is needed.
  virtual std::shared_ptr<const Snapshot> snapshot() const = 0;

  // Nodes are found by what is said about them, and what is said can change: a value set again replaces the one it
  // had (the last said stands; with two providers saying different things it is whichever wrote last, until the
  // statements say who says them), and an empty value says nothing. Nothing is made twice.
  //
  // An instrument is its ISIN: it is found by it, and what a market calls it is on its edge there, not on the
  // instrument. One without an ISIN (a currency, whose symbol is its code) is found by its symbol. Its symbol, name,
  // ISIN, type and kind (unless Other) are then set. The id returned is the instrument's key, none if it cannot be given one.
  virtual InstrumentId addInstrument(const std::string & symbol, const std::string & name, InstrumentKind kind, const std::string & isin = {},
                                     const std::string & type = {}) = 0;
  // An instrument by the first of the identifying attributes that finds one, made if none does; then all the
  // attributes, the identifying ones too, are set.
  virtual InstrumentId resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest) = 0;
  // The market that has any of these attributes, tried in the order given; where none has, a market is made. The
  // attributes are then set on it, so a market first seen by its Nordnet id is given its name when it is learned.
  virtual MarketId resolveMarket(const std::vector<Attribute> &) = 0;
  // By name alone.
  MarketId addMarket(const std::string & name) { return resolveMarket({{predicate::name, name}}); }
  // Sets one attribute of any node: the same for all of them.
  virtual void setAttribute(NodeRef, const Attribute &) = 0;
  // The listing of an edge, replacing the one it had. An edge exists once it has data, so one that has none yet takes
  // nothing.
  virtual void setListing(const Listing &) = 0;
  // Says several things of a node in one write (an adapter's whole row), each as setAttribute does.
  virtual void setAttributes(NodeRef, const std::vector<Attribute> &) = 0;
  // The dataset with this code, made if it is new; `name` is what a person reads (the code until one is said), and one already said
  // stands where none is given. A code that is not one to ten letters or digits makes none: the id is none, and it is told of.
  virtual DatasetId addDataset(const std::string & code, const std::string & name = {}) = 0;

  // Candles. An edge has a series for each resolution it has data in, as the provider gives it: nothing is derived from
  // another. A bar at a time already in the series replaces it (a correction, or today's bar updated); the rest are
  // inserted in order. The bars' times are slot numbers, or day numbers for a date resolution.
  virtual void appendCandles(const EdgeKey &, Resolution, const std::vector<Candle> &) = 0;
  // The close before the session of `day`, which a provider gives beside the day's bars; replaces what was said.
  virtual void setPreviousClose(const EdgeKey &, int64_t day, double close) = 0;
  // Order book snapshots, time-ordered; one at a time already in the series replaces it.
  virtual void appendBooks(const EdgeKey &, const std::vector<BookSnapshot> &) = 0;
  // Announcements and corporate actions of an instrument, per dataset, time-ordered. An announcement at the same time
  // with the same headline, or an action at the same time of the same kind, replaces the one there; anything else
  // at the same instant is kept beside it.
  virtual void appendAnnouncements(const EventKey &, const std::vector<Announcement> &) = 0;
  virtual void appendActions(const EventKey &, const std::vector<CorporateAction> &) = 0;

  // Returns when what has been said of the nodes is on its way to where it is kept (the disk, if a FactStore was given).
  virtual void flush() {}
};

struct StoreOptions {
  // Where the node attributes are kept between runs. Read when the store is made, before anything writes to it; every
  // change and every fact said again is written through on a thread of its own. None: nothing is kept.
  std::shared_ptr<FactStore> facts;
  FactWriter::Options writer;
  // The time of a write, for `updated`. The wall clock by default; a test sets its own.
  std::function<TimestampMs()> clock;
  // The sixty bits of the next market's key (NodeKey.h): random by default; a test can count instead.
  std::function<uint64_t()> mint;
  // Told, once for each, what the store refused or found wrong in what it was given or had kept: a MIC or an id at a source
  // that two markets would have. Called with the store's writer lock held, so it must not call back into the store.
  std::function<void(const std::string &)> notice;
};

// An in-memory store.
std::unique_ptr<Store> makeMemoryStore();
std::unique_ptr<Store> makeMemoryStore(StoreOptions);
