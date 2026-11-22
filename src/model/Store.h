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

  std::optional<Instrument> instrument(InstrumentId) const;
  // By a symbol said of the instrument itself (a currency's code).
  std::optional<Instrument> findInstrument(const std::string & symbol) const;
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
  std::optional<Instrument> findInstrumentByIsin(const std::string & isin) const;
  std::optional<Market> market(MarketId) const;
  std::optional<Market> findMarket(const std::string & name) const;  // by its name attribute
  std::optional<Market> findMarketByAttribute(const std::string & predicate, const std::string & value) const;
  // What is said about any node, in the order it was first said, with the times of each; and the node, of the type, that says the text `predicate`
  // is `value` (the one made first, if several do). Only text is found by its value.
  std::vector<Attribute> attributes(NodeRef) const;
  std::optional<NodeRef> findByAttribute(NodeType, const std::string & predicate, const std::string & value) const;
  std::optional<Dataset> dataset(DatasetId) const;
  std::optional<Dataset> findDataset(const std::string & name) const;
  const Seq<Instrument> & instruments() const { return instruments_; }
  const Seq<Market> & markets() const { return markets_; }
  const Seq<Dataset> & datasets() const { return datasets_; }

  // The edges that have any data (candles or books) in a market: what a preset needs. Sorted. An instrument with no
  // edge is not seen: nothing selects it.
  std::vector<EdgeKey> edges(MarketId) const;
  // The same, only those whose base instrument `keep` accepts (by its kind, its type, ...).
  std::vector<EdgeKey> edges(MarketId, const std::function<bool(const Instrument &)> & keep) const;

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
  Instrument add(Instrument);
  Instrument resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest);
  void setListing(const Listing &);
  Market resolve(const std::vector<Attribute> &);
  void setAttribute(NodeRef, const Attribute &);
  void setAttributes(NodeRef, const std::vector<Attribute> &);
  Dataset add(Dataset);
  void appendCandles(const EdgeKey &, Resolution, const std::vector<Candle> &);
  void setPreviousClose(const EdgeKey &, int64_t day, double close);
  void appendBooks(const EdgeKey &, const std::vector<BookSnapshot> &);
  void appendAnnouncements(const EventKey &, const std::vector<Announcement> &);
  void appendActions(const EventKey &, const std::vector<CorporateAction> &);

  // For a store to make on its own copy, with the changes above. `setTime` is the time of the write in progress: what
  // `updated` becomes. `hold` gives it what an earlier run said, kept until its node is made (a node is not
  // made by it). `revision` is the version plus the writes of facts that were only said again, which no version counts: a
  // store publishes a copy when it moves. `takeFacts` is what the writes since the last call ask of the disk.
  void setTime(TimestampMs now) { now_ = now; }
  void hold(const std::vector<StoredFact> &);
  uint64_t revision() const { return version_ + checks_; }
  FactBatch takeFacts() { return std::exchange(outbox_, FactBatch{}); }

 private:

  uint64_t version_ = 0;
  uint64_t checks_ = 0;
  TimestampMs now_ = 0;
  FactBatch outbox_;  // what to keep, for the store that made this copy
  immer::map<std::string, std::vector<Attribute>> held_;  // "type key" to what an earlier run said of that node
  immer::map<NodeRef, std::string> keyOf_;  // the key a node's facts are kept under, once it has one
  Seq<Instrument> instruments_;
  Seq<Market> markets_;
  Seq<Dataset> datasets_;
  immer::map<EdgeKey, Listing> listings_;
  immer::map<std::string, EdgeKey> edgeBySymbol_;  // "market \0 symbol" to the first edge there that has it
  immer::map<NodeRef, std::vector<Attribute>> attributes_;
  immer::map<std::string, immer::set<uint32_t>> byAttribute_;  // "type predicate \0 value" to the ids that say it
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
  // ISIN, type and kind (unless Other) are then set. The returned record carries the assigned id.
  virtual Instrument addInstrument(Instrument) = 0;
  // An instrument by the first of the identifying attributes that finds one, made if none does; then all the
  // attributes, the identifying ones too, are set.
  virtual Instrument resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest) = 0;
  // The market that has any of these attributes, tried in the order given; where none has, a market is made. The
  // attributes are then set on it, so a market first seen by its Nordnet id is given its name when it is learned.
  virtual Market resolveMarket(const std::vector<Attribute> &) = 0;
  // By name alone.
  Market addMarket(Market m) { return resolveMarket({{predicate::name, m.name}}); }
  // Sets one attribute of any node: the same for all of them.
  virtual void setAttribute(NodeRef, const Attribute &) = 0;
  // The listing of an edge, replacing the one it had. An edge exists once it has data, so one that has none yet takes
  // nothing.
  virtual void setListing(const Listing &) = 0;
  // Says several things of a node in one write (an adapter's whole row), each as setAttribute does.
  virtual void setAttributes(NodeRef, const std::vector<Attribute> &) = 0;
  virtual Dataset addDataset(Dataset) = 0;

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
};

// An in-memory store.
std::unique_ptr<Store> makeMemoryStore();
std::unique_ptr<Store> makeMemoryStore(StoreOptions);
