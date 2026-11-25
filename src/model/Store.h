#pragma once

#include "model/FactWriter.h"
#include "model/Facts.h"
#include "model/Summary.h"
#include "model/SummaryWriter.h"
#include "model/Types.h"
#include "portfolio/Transaction.h"

#include <immer/flex_vector.hpp>
#include <immer/map.hpp>
#include <immer/set.hpp>

#include <array>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>

// One immutable version of everything the application knows. A snapshot never changes: a reader that holds one reads it
// without locks, for as long as it likes, and sees one consistent world (the candles, books, news and
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
  // The fat edge's listing: what a dataset says of the instrument on the market, whether it was said before or after the
  // edge's data (an edge is seen only once it has data).
  std::optional<Listing> listing(const EdgeKey &) const;
  // What the market calls the base instrument there, else the instrument's own symbol.
  std::string symbol(const EdgeKey &) const;
  // The first edge to have data that a market's `symbol` is for: the instrument a market calls "NOKIA", whoever said it.
  std::optional<EdgeKey> findEdge(const std::string & symbol, MarketId) const;
  // The object of one predicate of a node, if it is said: an instrument's icon, its number of shares, a market's country.
  std::optional<Value> attribute(NodeRef, const std::string & predicate) const;
  // The same with its time: when a source last said it (`updated`), whether or not the value was the same as before, in
  // milliseconds UTC as the store took it where the write happened, and only to within `StoreOptions::recheck_interval`
  // (an hour by default): a value said again sooner than that after the time here leaves it as it is.
  std::optional<Attribute> fact(NodeRef, const std::string & predicate) const;
  std::optional<InstrumentId> findInstrumentByIsin(const std::string & isin) const;  // if anything is said of it
  // A market is its MIC, named by a fact. The market with this MIC, if anything
  // is said of it.
  std::optional<MarketId> findMarket(const std::string &mic) const;
  // The name said of a node (an instrument's); a market's is its MIC and a
  // dataset's its code until a name is said. Empty where none is.
  std::string name(NodeRef) const;
  // What is said about any node, in the order it was first said, with the times of each; and the node, of the type, that says the text `predicate`
  // is `value` (the one made first, if several do). Only text is found by its value.
  std::vector<Attribute> attributes(NodeRef) const;
  std::optional<NodeRef> findByAttribute(NodeType, const std::string & predicate, const std::string & value) const;
  // The nodes of a type that say `predicate` is a link to `target` ("who has this person as `ceo`"), by key. A link is found by the
  // same index as a text; the links to a node across all predicates ("is the CEO of") are not indexed.
  std::vector<NodeRef> findLinked(NodeType, const std::string & predicate, NodeKey target) const;
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

  // The edges that have any data and have this instrument as their base, or as their quote: what a price and a rate are looked up by.
  // Sorted by key.
  std::vector<EdgeKey> edgesOf(InstrumentId base) const;
  std::vector<EdgeKey> edgesTo(InstrumentId quote) const;

  // The edges that have a last price or a daily queue, in the order they got the first: what the table lists. An edge never leaves, so
  // this only grows, and a reader that has seen the first `n` asks for what came after (`quoteRowsSince(n)`) instead of listing
  // everything again to find what is new: `quoteRowCount()` is the one number that says whether anything is.
  size_t quoteRowCount() const { return quoteRows_.size(); }
  std::vector<EdgeKey> quoteRowsSince(size_t n) const { return std::vector<EdgeKey>(quoteRows_.begin() + std::min(n, quoteRows_.size()), quoteRows_.end()); }
  // Whether the edge is one of those rows.
  bool listed(const EdgeKey &) const;

  // The summary of an edge (Summary.h): not the data below, which a source writes and which stays in memory only, but what is kept of each
  // price for the tables, the sparklines and the portfolios.
  // The edge's quote, if anything has said one. `last` is what the table shows as the price.
  std::optional<Quote> quote(const EdgeKey &) const;
  // The bars of the queue of `resolution`, oldest first, as they were stored (see SummaryRecords.h); empty where there is none or it is
  // not in memory (read from the disk when it is wanted: `Store::want`). A row whose queue is not here shows its price and no trace.
  Seq<Candle> queue(const EdgeKey &, Resolution) const;
  // What is known of the queue whether it is in memory or on the disk: when it was fetched, how many bars, the keys of its first and last.
  std::optional<QueueCoverage> coverage(const EdgeKey &, Resolution) const;
  bool queueResident(const EdgeKey &, Resolution) const;
  // The resolutions the edge has a queue in, in memory or on the disk, shortest first.
  std::vector<Resolution> queues(const EdgeKey &) const;
  // What the queues in memory hold, in bytes (what `StoreOptions::queue_budget` is measured by).
  size_t queueBytes() const { return queueBytes_; }
  // How long a window the queues of a resolution keep for a dataset, in seconds measured back from the last bar; 0: no queue.
  int64_t keep(DatasetId, Resolution) const;

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

  // Portfolios. A portfolio is a minted key (NodeKey.h) with no record: it exists when something is said of it, a fact (its name, its
  // `currency`, its `ledger`) or a transaction, and one that nothing is said of is not listed. By name, then key; one with no name
  // has its key, shortened, as its name.
  std::vector<PortfolioId> portfolios() const;
  // What this dataset says the portfolio did, by day and then id. The holdings are derived from one dataset (portfolio/Portfolio.h).
  Seq<portfolio::Transaction> transactions(const LedgerKey &) const;
  // The datasets that say anything of what a portfolio did. Sorted.
  std::vector<DatasetId> ledgers(PortfolioId) const;

  // The snapshots in `range`, oldest first.
  Seq<BookSnapshot> books(const EdgeKey &, TimeRange range = {}) const;
  // The snapshot standing at `time`: the latest at or before it.
  std::optional<BookSnapshot> bookAt(const EdgeKey &, double time) const;

  // The news that `query` selects, newest first (the same time: by dataset, then id). Values, not references into the store.
  std::vector<NewsItem> news(const NewsQuery & query = {}) const;
  // How many items there are, in every dataset.
  size_t newsCount() const;
  Seq<CorporateAction> actions(const EventKey &, TimeRange range = {}) const;
  // The datasets that have corporate actions for an instrument. Sorted.
  std::vector<EventKey> events(InstrumentId) const;

  // Changes, for a store to make on its own copy before publishing it. Nothing that holds a published snapshot
  // can call them, because it holds it const. Each one that changes something bumps the version.
  InstrumentId addInstrument(const std::string & symbol, const std::string & name, InstrumentKind, const std::string & isin, const std::string & type);
  InstrumentId resolveInstrument(const std::vector<Attribute> & identifying, const std::vector<Attribute> & rest);
  void setListing(const Listing &);
  MarketId addMarket(const std::string &mic, const std::string &name);
  void setAttribute(NodeRef, const Attribute &);
  void setAttributes(NodeRef, const std::vector<Attribute> &);
  DatasetId addDataset(const std::string & code, const std::string & name);
  void appendCandles(const EdgeKey &, Resolution, const std::vector<Candle> &);
  void setPreviousClose(const EdgeKey &, int64_t day, double close);
  // The edge's quote, replacing the one it had; the newest says: one with an earlier time than the one there is left out.
  void setQuote(const EdgeKey &, const Quote &);
  void setRetention(DatasetId, Resolution, std::optional<std::chrono::seconds>);
  void appendBooks(const EdgeKey &, const std::vector<BookSnapshot> &);
  void appendNews(DatasetId, const std::vector<NewsItem> &);
  void appendActions(const EventKey &, const std::vector<CorporateAction> &);
  void removeAttribute(NodeRef, const std::string & predicate);
  void removePortfolio(PortfolioId);
  void appendTransactions(const LedgerKey &, const std::vector<portfolio::Transaction> &);
  void removeTransactions(const LedgerKey &, const std::vector<uint64_t> & ids);

  // For a store to make on its own copy, with the changes above. `setTime` is the time of the write in progress: what
  // `updated` becomes. `load` gives it what an earlier run said: the facts of nodes, which makes no version and no node but
  // what is said, each fact with the `updated` the disk had. Built in bulk (the lookup indexes are made once for the whole of it, not for
  // each fact), and what a source has said of a node since stands: the disk only adds what was not said. May be called again, with more. `revision` is the version plus the writes of facts that were only said again, which no version counts: a
  // store publishes a copy when it moves. `takeFacts` is what the writes since the last call ask of the disk.
  void setTime(TimestampMs now) { now_ = now; }
  // Whether an instrument with no ISIN is numbered in the private namespace as it is asked for (ZZ999999999 down, the same text the same
  // number), for tests and the made-up data; else it is refused and told of: the adapters give their own ISINs (source/KnownIsins.h).
  void setInventIsins(bool invent) { invent_ = invent; }
  void setRecheckInterval(std::chrono::milliseconds interval) { recheck_ = interval; }
  // Makes the changes since the version was `before` one version: a batch of writes is one change to a reader.
  void settleVersion(uint64_t before) {
    if (version_ != before) version_ = before + 1;
  }
  void load(std::vector<StoredFact> facts);  // by value: a caller that is done with them moves them in
  // The transactions an earlier run kept: no version, no write for the disk; one that cannot be read, or that is not tradable, is left out.
  void loadEvents(const std::vector<StoredEvent> &);
  // What an earlier run kept of edges: their listing, quote and close before the session, and the coverage of their queues. It makes the
  // edges, but no bar and no write for the disk, and what a source has said since stands (it is newer than the disk).
  void loadEdges(const std::vector<StoredEdge> &);
  // A queue read from the disk: the bars said since it was last in memory are put into it and win. A version, so that what shows it is
  // drawn again; nothing for the disk, since it came from there.
  void loadQueue(const StoredQueue &);
  // Puts a queue out of memory (what the file has of it is kept as coverage). Not a version: nothing that is shown changes.
  void evictQueue(const EdgeKey &, Resolution);
  // While the files are being read, what a source says of a queue that is not in memory is not told from what the file has until it is:
  // `settleRestored` says every file has been read, and the queues they had none of are what was said.
  void setRestoring(bool restoring) {
    if (restoring_ == restoring) return;
    restoring_ = restoring;
    checks_++;  // published, though nothing that is shown changes
  }
  void settleRestored();
  // What the writes since the last call ask of the summary files.
  SummaryBatch takeSummaries();
  // The edges that were made since the last call (what a store reads the facts of their nodes for, if it has not yet).
  std::vector<EdgeKey> takeNewEdges() { return std::exchange(newEdges_, {}); }
  uint64_t revision() const { return version_ + checks_; }
  FactBatch takeFacts() { return std::exchange(outbox_, FactBatch{}); }
  // What the writes since the last call refused to say, in words: an ISIN that
  // is not valid, a code or a MIC that does not fit.
  std::vector<std::string> takeNotices() { return std::exchange(notices_, {}); }

 private:
  bool hasData(const EdgeKey &) const;
  void indexSymbol(const EdgeKey &);
  void addEdge(const EdgeKey &);
  void noteRow(const EdgeKey &);
  bool deriveSummary(const EdgeKey &, Resolution, const std::vector<Candle> &);
  void putListing(const Listing &, bool fromDisk);

  uint64_t version_ = 0;
  uint64_t checks_ = 0;
  TimestampMs now_ = 0;
  std::chrono::milliseconds recheck_{0};
  bool invent_ = true;
  immer::map<std::string, std::string> numbered_;  // the ISINs numbered as asked for, by the text they were asked by
  FactBatch outbox_;                                              // what to keep, for the store that made this copy
  std::vector<std::string> notices_;                              // what was refused, for the store that made this copy
  immer::map<EdgeKey, Listing> listings_;
  immer::map<std::string, EdgeKey> edgeBySymbol_;  // "market \0 symbol" to the first edge there that has it
  immer::map<NodeRef, std::vector<Attribute>> attributes_;
  immer::map<std::string, immer::set<uint64_t>> byAttribute_;  // "type predicate \0 value" to the keys that say it
  immer::map<SeriesKey, Seq<Candle>> series_;
  immer::map<EdgeKey, immer::set<int>> resolutions_;
  immer::flex_vector<EdgeKey> quoteRows_;  // the edges with a last price or a daily queue, in the order they got one
  immer::set<EdgeKey> rows_;               // the same, to know one is there
  immer::map<EdgeKey, Quote> quotes_;
  immer::map<SeriesKey, QueueState> queues_;
  immer::map<DatasetId, std::array<int64_t, 11>> retention_;  // seconds by resolution, for a dataset that has its own rules
  size_t queueBytes_ = 0;
  bool restoring_ = false;
  // What the writes since the last `takeSummaries` ask of the files: edges to put, queues in memory to put, and the bars said of
  // queues that are not in memory, to merge.
  std::set<EdgeKey> dirtyEdges_;
  std::set<SeriesKey> putQueues_;
  std::map<SeriesKey, std::vector<Candle>> mergeQueues_;
  std::map<SeriesKey, TimestampMs> touchQueues_;  // said again and the same: only when it was fetched moves
  std::vector<EdgeKey> newEdges_;                  // the edges made since the last `takeNewEdges`
  immer::map<EdgeKey, std::pair<int64_t, double>> previousClose_;  // the session's day number and the close before it
  immer::map<EdgeKey, Seq<BookSnapshot>> books_;
  immer::map<MarketId, immer::set<EdgeKey>> edgesByMarket_;
  immer::map<InstrumentId, immer::set<EdgeKey>> edgesByBase_, edgesByQuote_;
  immer::map<DatasetId, Seq<NewsItem>> news_;                 // per dataset, by time
  immer::map<DatasetId, immer::set<std::string>> newsIds_;    // the ids it has said
  size_t newsCount_ = 0;
  immer::map<EventKey, Seq<CorporateAction>> actions_;
  immer::map<InstrumentId, immer::set<EventKey>> eventsByInstrument_;
  immer::map<LedgerKey, Seq<portfolio::Transaction>> ledgers_;
  immer::map<PortfolioId, immer::set<DatasetId>> ledgersOf_;  // the datasets that have transactions
  immer::set<PortfolioId> portfolios_;                        // those that anything is said of
  void settle(PortfolioId);                                   // lists or forgets the portfolio, by whether anything is said of it
  void forget(uint64_t node, const std::string * predicate);  // takes what is waiting for the disk of the node back
};

// The writes a store takes, called one at a time on a `Store` (each its own change, published at once) or together in a
// `Batch`.
class StoreWrites {
 public:
  virtual ~StoreWrites() = default;

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
  // The market with this MIC (ISO 10383: four characters of A-Z0-9), made if it
  // is new; `name` is what a person reads (the MIC until one is said), and one
  // already said stands where none is given. Nothing else is a market: a code
  // that is no MIC makes none (the id is none and it is told of), and a source
  // that cannot name a row's market skips the row. The two that are no venue
  // are kOffExchange (XOFF: listed instruments traded off an exchange, the
  // funds) and kNoMarket (XXXX: an index, a spot price, a currency pair).
  virtual MarketId addMarket(const std::string &mic,
                             const std::string &name = {}) = 0;
  // Sets one attribute of any node: the same for all of them.
  virtual void setAttribute(NodeRef, const Attribute &) = 0;
  // The listing of an edge, replacing the one it had.
  virtual void setListing(const Listing &) = 0;
  // Says several things of a node in one write (an adapter's whole row), each as setAttribute does.
  virtual void setAttributes(NodeRef, const std::vector<Attribute> &) = 0;
  // The dataset with this code, made if it is new; `name` is what a person reads (the code until one is said), and one already said
  // stands where none is given. A code that is not one to ten letters or digits makes none: the id is none, and it is told of.
  virtual DatasetId addDataset(const std::string & code, const std::string & name = {}) = 0;

  // What is said of an edge or an event needs all of its parts: a key with an
  // instrument, a market or a dataset that is none (one that was refused) is a
  // bug in the caller, and every call below throws std::invalid_argument for
  // it, and writes nothing.
  //
  // Candles. An edge has a series for each resolution it has data in, as the
  // provider gives it: nothing is derived from another. A bar at a time already
  // in the series replaces it (a correction, or today's bar updated); the rest
  // are inserted in order. The bars' times are slot numbers, or day numbers for
  // a date resolution.
  virtual void appendCandles(const EdgeKey &, Resolution, const std::vector<Candle> &) = 0;
  // The close before the session of `day`, which a provider gives beside the day's bars; replaces what was said.
  virtual void setPreviousClose(const EdgeKey &, int64_t day, double close) = 0;
  // The edge's quote (Summary.h): the last price, bid and ask and the time they are for, replacing the one it had without touching a queue. The
  // newest says: a quote with an earlier time than the one there is left out. A source that gives only bars still gives a price: a bar newer
  // than the quote's time sets its last price to the bar's close and its time to the bar's.
  virtual void setQuote(const EdgeKey &, const Quote &) = 0;
  // Order book snapshots, time-ordered; one at a time already in the series replaces it.
  virtual void appendBooks(const EdgeKey &, const std::vector<BookSnapshot> &) = 0;
  // News, by the dataset that says it. An item is its dataset and its id: one that is there already is left as it was (a dataset
  // that says a story again changes nothing, and two datasets with the same story have two items); one with no id is refused
  // with std::invalid_argument and nothing of the call is written. `dataset` of an item is set to the dataset given.
  virtual void appendNews(DatasetId, const std::vector<NewsItem> &) = 0;
  // Corporate actions of an instrument, per dataset, time-ordered. An action at the same time of the same kind replaces the one
  // there; anything else at the same instant is kept beside it.
  virtual void appendActions(const EventKey &, const std::vector<CorporateAction> &) = 0;

  // Takes back what was said of a portfolio under a predicate: its facts are the user's own, so they can be taken back (a
  // source's cannot, and says std::invalid_argument for a node that is no portfolio's). Nothing said: nothing changes.
  virtual void removeAttribute(NodeRef, const std::string & predicate) = 0;
  // Takes back everything said of a portfolio: its facts and every transaction in every dataset. It is not listed after.
  virtual void removePortfolio(PortfolioId) = 0;
  // Puts transactions in a portfolio's ledger (what this dataset says it did), each replacing the one with the same id. A transaction
  // that cannot be bought (`portfolio::tradable`: an index, a spot price, a currency pair or a rate, whose market is XXXX) is
  // refused: std::invalid_argument says which and why, and nothing of the call is written. So is a key or a transaction with a
  // part that is none.
  virtual void appendTransactions(const LedgerKey &, const std::vector<portfolio::Transaction> &) = 0;
  // Takes back the transactions with these ids; ids not there change nothing.
  virtual void removeTransactions(const LedgerKey &, const std::vector<uint64_t> & ids) = 0;
};

// What `Store::batch()` gives its function: the writes above, applied to one copy of the store that nothing sees until the
// function returns, and what the batch has written so far to read (a source looks up before it adds). Ids come back at once,
// as from the store's own calls. Valid only inside the function; do not keep it.
class Batch : public StoreWrites {
 public:
  // The store as the batch has made it so far.
  virtual const Snapshot & view() const = 0;
};

// Where the snapshots come from, and the only way to change them. Data sources write through it and everything else
// reads a snapshot, so a store backed by a server can replace the in-memory one. Any thread may call anything: writes
// are serialised, and `snapshot()` never waits for one.
class Store : public StoreWrites {
 public:
  // The current version. Hold it for as long as a consistent view is needed.
  virtual std::shared_ptr<const Snapshot> snapshot() const = 0;

  // Runs `change` with a batch and applies everything it writes under one lock, published once: one new version and one
  // snapshot however many writes it holds, so a reader never sees half of a row (an edge with bars and no listing) and a
  // source's page is one publication, not thousands. The notices and the facts for the disk of the whole batch go out
  // once, at the end. A call of the store's own is a batch of one write. If `change` throws (a key with a part that is
  // none), nothing of the batch is applied.
  virtual void batch(const std::function<void(Batch &)> & change) = 0;

  // Whether the files are still being read (`StoreOptions::Restore::InBackground`): the store works meanwhile, and what a source says in
  // that time is newer than the disk and stands. `waitRestored` returns when they have all been read.
  virtual bool restoring() const { return false; }
  virtual void waitRestored() {}

  // Returns when what has been said of the nodes and the summaries is on its way to where it is kept (the disk, if a FactStore and a
  // SummaryStores were given).
  virtual void flush() {}

  // The queues that are wanted now (the rows on screen and a margin), most wanted first, which replaces what was wanted before: those not in
  // memory are read from the disk on a thread of its own and published into the store (a version, nothing written back), and those wanted
  // are not dropped from memory when it is over its budget. Never waits for the disk.
  virtual void want(const std::vector<SeriesKey> &) {}
  // How long a window of bars the queues of a resolution keep for a dataset (the provider's knowledge: HistoryProvider::keep), measured
  // back from the last bar; none: no queue for that resolution.
  virtual void setRetention(DatasetId, Resolution, std::optional<std::chrono::seconds>) = 0;
};

// What the queues in memory may hold unless said otherwise: 32 MiB.
inline constexpr size_t kDefaultQueueBudget = size_t{32} << 20;

struct StoreOptions {
  // Whether an instrument that comes with no ISIN is numbered as it comes (the tests, the made-up data: the default), or refused and told of
  // (the program: its adapters give the ISINs they supply from source/KnownIsins.h, so one that has none is a mistake to show).
  bool invent_isins = true;
  // Where the user's own portfolios are kept: a second file, routed by the kind of node, so that a portfolio is never in `facts` and a
  // source cannot write one into it. Read when the store is made; written at once and in the thread of the write, since the writes
  // are rare and by hand. None: portfolios live in memory for the session.
  std::shared_ptr<FactStore> portfolios;
  // Where the node attributes are kept between runs. Read when the store is made, before anything writes to it; every
  // change and every fact said again is written through on a thread of its own. None: nothing is kept.
  std::shared_ptr<FactStore> facts;
  FactWriter::Options writer;
  // Where the summaries of the edges are kept between runs (Summary.h): one store for each dataset. None: they are kept for the session
  // only. The summary is not the data: the bars a source writes are never kept, only the quote and the queues derived from them.
  std::shared_ptr<SummaryStores> summaries;
  SummaryWriter::Options summary_writer;
  // What the queues in memory may hold, in bytes (48 per bar, `sizeof(Candle)`): above it, the ones not wanted for the longest are dropped (their
  // quote stays, and the disk has them). The default holds some 700,000 bars: a screen of rows with a margin is a few thousand.
  size_t queue_budget = kDefaultQueueBudget;
  // Called on the thread that read queues from the disk, or that restores the files, after what it read is published (a program hands it
  // to the UI thread, which redraws once for each).
  std::function<void()> changed;
  // How the files are read when the store is made. Now: before the constructor returns (the tests: what was kept is there when they look).
  // InBackground: the constructor returns at once, with the user's own portfolios, and a thread of the store's reads the rest in chunks, each
  // published as it is read (`changed` after each): first what the portfolios hold, their facts, their edges' listings and quotes, then the
  // rest. The facts read are those of the nodes that have an edge (an instrument no edge has is not restored: nothing selects it, and a
  // source says it again), where summaries are kept; with none, all of them.
  enum class Restore { Now, InBackground };
  Restore restore = Restore::Now;
  // The time of a write, for `updated`. The wall clock by default; a test sets its own.
  std::function<TimestampMs()> clock;
  // `updated` says when a source last said a fact to within this interval: a value said again that was last said less than this
  // ago changes nothing (no new time, no snapshot, nothing for the disk); an older one gets the time of the write. A changed
  // value is written at once. Zero: every write that says a fact again moves its time.
  std::chrono::milliseconds recheck_interval{3'600'000};
  // Told, once for each, what the store refused in what it was given: an ISIN
  // that is not valid, a code or a MIC that does not fit. Called with the
  // store's writer lock held, so it must not call back into the store.
  std::function<void(const std::string &)> notice;
};

// An in-memory store.
std::unique_ptr<Store> makeMemoryStore();
std::unique_ptr<Store> makeMemoryStore(StoreOptions);
