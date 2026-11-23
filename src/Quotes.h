#pragma once

#include "icons/IconLoader.h"
#include "model/Period.h"
#include "model/Store.h"

#include <cellui/Table.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

// One table row is one edge: what one unit of `base` costs in `quote`, as one dataset reports it on one market.
struct QuoteRow {
  EdgeKey key;
  std::string base, quote, name, market, dataset;
  std::string icon_url;  // of the base instrument; empty for none
  double price = 0;
  double change = kUndefined;  // in percent, over the period; undefined where the bars do not give one
  std::vector<float> history;  // the period's closes, oldest first
};

// The rows of a market: one per edge that has candles, in the order the store lists them, with the change and the closes of
// `period`.
std::vector<QuoteRow> makeRows(const Snapshot & snapshot, MarketId market, Period period = kDefaultPeriod);
// The rows of every market, in market order.
std::vector<QuoteRow> makeAllRows(const Snapshot & snapshot, Period period = kDefaultPeriod);

// The edges that have daily bars, in a market or in every market (in market order): what a table shows.
std::vector<EdgeKey> quoteEdges(const Snapshot & snapshot, MarketId market);
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
  // Shows other edges, or the same from a newer snapshot: the caches go.
  void reset(std::shared_ptr<const Snapshot> snapshot, std::vector<EdgeKey> keys);
  // The same edges from a newer snapshot: the caches go, the rows and their order stay.
  void setSnapshot(std::shared_ptr<const Snapshot> snapshot);
  const std::vector<EdgeKey> & edges() const { return keys_; }

  int rowCount() const override { return static_cast<int>(keys_.size()); }
  std::string text(int r, int c) const override;
  // The edge a row shows: its identity.
  const EdgeKey & edge(int r) const { return keys_[static_cast<size_t>(r)]; }
  // The icon URL of a row's base instrument, empty for none.
  std::string iconUrl(int r) const;
  std::shared_ptr<const cellui::TableIcon> icon(int r, int c) const override;
  std::span<const float> trace(int r, int) const override;
  std::optional<cellui::Color> color(int r, int c) const override;
  // Prices and changes sort by their values, not their rounded text; the others by their text, letters of either case alike.
  std::optional<double> number(int r, int c) const override;
  int compare(int a, int b, int column) const override;

 private:
  double price(int r) const;
  double change(int r) const;  // over the period, in percent; undefined for no number
  const std::string & marketLabel(MarketId) const;
  const std::string & datasetName(DatasetId) const;

  Period period_ = kDefaultPeriod;
  std::shared_ptr<IconLoader> icons_;
  std::shared_ptr<const Snapshot> snapshot_;
  std::vector<EdgeKey> keys_;
  mutable std::unordered_map<uint64_t, std::string> marketLabels_, datasetNames_;
  mutable std::unordered_map<int, std::vector<float>> traces_;  // the rows drawn lately, for the span trace() gives
  mutable std::vector<double> numbers_[2];                      // the sort keys of price and change
  mutable std::vector<std::string> texts_[HISTORY];             // and of the text columns, in lower case
  mutable bool built_[HISTORY + 1] = {};
  void build(int column) const;
};
