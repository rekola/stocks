#pragma once

#include "model/Types.h"

#include <memory>

namespace stocks {

// Everything the application knows, behind one interface. Data sources write only through it and everything else reads
// through it, so a store backed by a server can replace the in-memory one. Reads return values, never references into
// the store. Not thread-safe: one thread owns a store (the writer posts changes to the UI thread).
class Store {
 public:
  virtual ~Store() = default;

  // Add by name: the same symbol (instruments) or name (markets, datasets) again returns the one already there and
  // keeps its first values. The returned record carries the assigned id.
  virtual Instrument addInstrument(Instrument i) = 0;
  virtual Market addMarket(Market m) = 0;
  virtual Dataset addDataset(Dataset d) = 0;

  virtual std::optional<Instrument> instrument(InstrumentId) const = 0;
  virtual std::optional<Instrument> findInstrument(const std::string & symbol) const = 0;
  virtual std::optional<Market> market(MarketId) const = 0;
  virtual std::optional<Market> findMarket(const std::string & name) const = 0;
  virtual std::optional<Dataset> dataset(DatasetId) const = 0;
  virtual std::optional<Dataset> findDataset(const std::string & name) const = 0;
  virtual std::vector<Instrument> instruments() const = 0;
  virtual std::vector<Market> markets() const = 0;
  virtual std::vector<Dataset> datasets() const = 0;

  // The edges that have any data (candles or books) in a market: what a preset needs.
  virtual std::vector<EdgeKey> edges(MarketId) const = 0;

  // Candles. A bar at a time already in the series replaces it (a correction); the rest are inserted in order. The
  // first append fixes the series' interval, and an append with another interval changes nothing and returns false.
  virtual bool appendCandles(const EdgeKey &, int64_t intervalMs, const std::vector<Candle> &) = 0;
  // The bars in `range`, oldest first; an empty series for an unknown edge.
  virtual CandleSeries candles(const EdgeKey &, TimeRange range = {}) const = 0;
  // The newest `n` bars, oldest first: the table's latest price, change and trace.
  virtual CandleSeries lastCandles(const EdgeKey &, size_t n) const = 0;

  // Order book snapshots, time-ordered; one at a time already in the series replaces it.
  virtual void appendBooks(const EdgeKey &, const std::vector<BookSnapshot> &) = 0;
  // The snapshots in `range`, oldest first.
  virtual std::vector<BookSnapshot> books(const EdgeKey &, TimeRange range = {}) const = 0;
  // The snapshot standing at `time`: the latest at or before it.
  virtual std::optional<BookSnapshot> bookAt(const EdgeKey &, Timestamp time) const = 0;

  // Announcements and corporate actions of an instrument, per dataset, time-ordered; one at a time already in the
  // series replaces it only if it is equal in the time and the headline (announcements) or the kind (actions), so two
  // things on the same instant both stay.
  virtual void appendAnnouncements(const EventKey &, const std::vector<Announcement> &) = 0;
  virtual std::vector<Announcement> announcements(const EventKey &, TimeRange range = {}) const = 0;
  virtual void appendActions(const EventKey &, const std::vector<CorporateAction> &) = 0;
  virtual std::vector<CorporateAction> actions(const EventKey &, TimeRange range = {}) const = 0;
  // The datasets that have announcements or actions for an instrument.
  virtual std::vector<EventKey> events(InstrumentId) const = 0;
};

// An in-memory store.
std::unique_ptr<Store> makeMemoryStore();

}  // namespace stocks
