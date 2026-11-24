#pragma once

#include "icons/IconLoader.h"
#include "model/Period.h"
#include "RowActions.h"
#include "model/Store.h"
#include "portfolio/Portfolio.h"

#include <cellui/Table.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// One table row is one edge: what one unit of `base` costs in `quote`, as one dataset reports it on one market.
// The symbol that stands for an instrument's icon where there is no picture: the one it says itself (iconSymbol), else one of its
// kind (an index 📈, a currency 💱: quoted in another, it is a currency pair); empty for none.
std::string iconSymbolOf(const Snapshot & snapshot, InstrumentId instrument);

struct QuoteRow {
  EdgeKey key;
  std::string base, quote, name, market, dataset;
  std::string icon_url;  // of the base instrument; empty for none
  double price = 0;
  bool rate = false;           // the row is a rate (quoted in per cent): its change is in basis points
  double change = kUndefined;  // over the period, in percent (basis points for a rate); undefined where the bars do not give one
  std::vector<float> history;  // the period's closes, oldest first
};

// The rows of a market: one per edge that has candles, in the order the store lists them, with the change and the closes of
// `period`.
std::vector<QuoteRow> makeRows(const Snapshot & snapshot, MarketId market, Period period = kDefaultPeriod);
// The rows of every market, in market order.
std::vector<QuoteRow> makeAllRows(const Snapshot & snapshot, Period period = kDefaultPeriod);

// The edges that have daily bars, in a market or in every market (in market order): what a table shows.
std::vector<EdgeKey>
quoteEdges(const Snapshot &snapshot, MarketId market,
           std::optional<InstrumentKind> kind = std::nullopt);
std::vector<EdgeKey> allQuoteEdges(const Snapshot & snapshot);

// A number with `decimals` places and a decimal point whatever the locale, with a sign when `sign`.
std::string fixed(double v, int decimals, bool sign = false);

// The headings of the change and the trace columns name the period: "Change 1d", "1 day".
std::string changeTitle(Period);

// A list of edges as a table: base, quote, name, market, dataset, price, the change over a period and a trace of its closes. The rows are not
// built: a cell is read from the snapshot when the table asks for it, which is for the rows in view, so the cost of
// showing tens of thousands of rows, and of showing them again when the store changes, does not grow with their number.
// Only a sort reads a whole column, once, and keeps it until the rows change.
class Quotes : public cellui::TableModel {
 public:
  enum Column { ICON, BASE, QUOTE, NAME, MARKET, DATASET, PRICE, CHANGE, HISTORY };

  // `icons` supplies the pictures of the icon column; without it the column is empty.
  Quotes(std::shared_ptr<const Snapshot> snapshot, std::vector<EdgeKey> keys, std::shared_ptr<IconLoader> icons = nullptr);
  // The period the change column and the trace show: every row at once. The sort keys of the change are read again; the rows,
  // the sort and the scroll position are the table's, which is told with modelChanged().
  void setPeriod(Period);
  Period period() const { return period_; }
  // Which edges a table shows: every market's, or one market's (not found yet: none, and what came meanwhile is read once it is).
  struct Scope {
    bool all = false;
    std::optional<MarketId> market; // only this market's edges, where set
    std::optional<InstrumentKind>
        kind; // only edges whose base is of this kind, where set
    bool resolved() const { return all || market.has_value(); }
    bool has(const Snapshot &snapshot, const EdgeKey &key) const {
      return (all || (market && key.market == *market)) &&
             (!kind || snapshot.kind(key.base) == *kind);
    }
  };
  // Whether the store has rows this model has not looked at: one comparison, so a refresh that brings only new bars for the rows
  // listed does nothing else.
  bool hasNewRows(const Snapshot & snapshot) const { return snapshot.quoteRowCount() != rows_seen_; }
  // Shows a newer snapshot and puts the rows that appeared since, in the scope, after the ones listed (the store says which they
  // are, so nothing is listed again or compared; the rows listed keep their places, and a sort is the user's to ask for).
  // Returns whether there were any. The caches go either way.
  bool addRows(std::shared_ptr<const Snapshot> snapshot, const Scope & scope);
  // Shows other edges, or the same from a newer snapshot: the caches go.
  void reset(std::shared_ptr<const Snapshot> snapshot, std::vector<EdgeKey> keys);
  // The same edges from a newer snapshot: the caches go, the rows and their order stay.
  void setSnapshot(std::shared_ptr<const Snapshot> snapshot);
  const std::vector<EdgeKey> & edges() const { return keys_; }

  int rowCount() const override { return static_cast<int>(keys_.size()); }
  std::string text(int r, int c) const override;
  // The info button is on every row; buy and sell only on a row that can be bought: a venue or a fund, not an index, a spot price, a
  // currency pair or a rate (portfolio::tradable).
  bool buttonShown(int r, int, int button) const override { return button == kInfoButton || portfolio::tradable(*snapshot_, edge(r)); }
  // The edge a row shows: its identity.
  const EdgeKey & edge(int r) const { return keys_[static_cast<size_t>(r)]; }
  // The icon URL of a row's base instrument, empty for none.
  std::string iconUrl(int r) const;
  std::shared_ptr<const cellui::TableIcon> icon(int r, int c) const override;
  // The symbol an instrument says stands for its icon, shown where there is no picture.
  std::string iconFallback(int r, int c) const override;
  std::span<const float> trace(int r, int) const override;
  std::optional<cellui::Color> color(int r, int c) const override;
  // Prices and changes sort by their values, not their rounded text; the others by their text, letters of either case alike.
  // The change is the number as shown: basis points for a rate, per cent for a price. In a preset that mixes rates and prices
  // (All) they compare as raw numbers; there is no other ordering.
  std::optional<double> number(int r, int c) const override;
  int compare(int a, int b, int column) const override;

 private:
  double price(int r) const;
  double change(int r) const;  // over the period, in percent (basis points for a rate); undefined for no number
  const std::string & marketLabel(MarketId) const;
  const std::string & datasetName(DatasetId) const;

  Period period_ = kDefaultPeriod;
  std::shared_ptr<IconLoader> icons_;
  std::shared_ptr<const Snapshot> snapshot_;
  std::vector<EdgeKey> keys_;
  size_t rows_seen_ = 0;  // how many of the store's rows (Snapshot::quoteRowCount()) the rows above are made from
  mutable std::unordered_map<uint64_t, std::string> marketLabels_, datasetNames_;
  mutable std::unordered_map<int, std::vector<float>> traces_;  // the rows drawn lately, for the span trace() gives
  mutable std::vector<double> numbers_[2];                      // the sort keys of price and change
  mutable std::vector<std::string> texts_[HISTORY];             // and of the text columns, in lower case
  mutable bool built_[HISTORY + 1] = {};
  void build(int column) const;
};
