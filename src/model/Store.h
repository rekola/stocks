#pragma once

#include "model/Types.h"

#include <immer/map.hpp>
#include <immer/set.hpp>

#include <functional>
#include <memory>

// One immutable version of everything the application knows. A snapshot never changes: a reader that holds one reads it
// without locks, for as long as it likes, and sees one consistent world (the candles, books, announcements and
// actions as of one moment). Copying a snapshot is cheap and shares all of its storage; a write makes a new snapshot
// that shares almost everything with the one before. Slices a snapshot returns share storage with it too.
class Snapshot {
 public:
  // Which version of the store this is; each change that changed something makes the next number.
  uint64_t version() const { return version_; }

  std::optional<Instrument> instrument(InstrumentId) const;
  std::optional<Instrument> findInstrument(const std::string & symbol) const;
  std::optional<Instrument> findInstrumentByIsin(const std::string & isin) const;
  std::optional<Market> market(MarketId) const;
  std::optional<Market> findMarket(const std::string & name) const;  // by its name attribute
  std::optional<Market> findMarketByAttribute(const std::string & predicate, const std::string & value) const;
  // What is said about any node, in the order it was first said; and the node, of the type, that says `predicate` is
  // `value` (the one made first, if several do).
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
  // The resolutions the edge has bars in, shortest first.
  std::vector<Resolution> resolutions(const EdgeKey &) const;

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
  Market resolve(const std::vector<Attribute> &);
  void setAttribute(NodeRef, const Attribute &);
  Dataset add(Dataset);
  void appendCandles(const EdgeKey &, Resolution, const std::vector<Candle> &);
  void appendBooks(const EdgeKey &, const std::vector<BookSnapshot> &);
  void appendAnnouncements(const EventKey &, const std::vector<Announcement> &);
  void appendActions(const EventKey &, const std::vector<CorporateAction> &);

 private:

  uint64_t version_ = 0;
  Seq<Instrument> instruments_;
  Seq<Market> markets_;
  Seq<Dataset> datasets_;
  immer::map<NodeRef, std::vector<Attribute>> attributes_;
  immer::map<std::string, immer::set<uint32_t>> byAttribute_;  // "type predicate \0 value" to the ids that say it
  immer::map<SeriesKey, Seq<Candle>> series_;
  immer::map<EdgeKey, immer::set<int>> resolutions_;
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
  // An instrument with an ISIN is found by it and by nothing else (two with the same symbol and different ISINs stay
  // two, the symbol finds the first); one without is found by its symbol. Its symbol, name, ISIN, type and kind (unless
  // Other) are then set. The returned record carries the assigned id.
  virtual Instrument addInstrument(Instrument) = 0;
  // The market that has any of these attributes, tried in the order given; where none has, a market is made. The
  // attributes are then set on it, so a market first seen by its Nordnet id is given its name when it is learned.
  virtual Market resolveMarket(const std::vector<Attribute> &) = 0;
  // By name alone.
  Market addMarket(Market m) { return resolveMarket({{predicate::name, m.name}}); }
  // Sets one attribute of any node: the same for all of them.
  virtual void setAttribute(NodeRef, const Attribute &) = 0;
  virtual Dataset addDataset(Dataset) = 0;

  // Candles. An edge has a series for each resolution it has data in, as the provider gives it: nothing is derived from
  // another. A bar at a time already in the series replaces it (a correction, or today's bar updated); the rest are
  // inserted in order. The bars' times are slot numbers, or day numbers for a date resolution.
  virtual void appendCandles(const EdgeKey &, Resolution, const std::vector<Candle> &) = 0;
  // Order book snapshots, time-ordered; one at a time already in the series replaces it.
  virtual void appendBooks(const EdgeKey &, const std::vector<BookSnapshot> &) = 0;
  // Announcements and corporate actions of an instrument, per dataset, time-ordered. An announcement at the same time
  // with the same headline, or an action at the same time of the same kind, replaces the one there; anything else
  // at the same instant is kept beside it.
  virtual void appendAnnouncements(const EventKey &, const std::vector<Announcement> &) = 0;
  virtual void appendActions(const EventKey &, const std::vector<CorporateAction> &) = 0;
};

// An in-memory store.
std::unique_ptr<Store> makeMemoryStore();
