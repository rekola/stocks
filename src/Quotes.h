#pragma once

#include "icons/IconLoader.h"
#include "model/Group.h"
#include "model/Period.h"
#include "model/Store.h"
#include "portfolio/Portfolio.h"

#include <cellui/Table.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// One table row is one group (model/Group.h): what one unit of `base` costs in `quote`, as the markets and datasets that report it say together.
// The symbol that stands for an instrument's icon where there is no picture: the one it says itself (iconSymbol), else one of its
// kind (an index 📈, a currency 💱: quoted in another, it is a currency pair); empty for none.
std::string iconSymbolOf(const Snapshot & snapshot, InstrumentId instrument);
// The colour the instrument says its symbol has (iconColor); none for the text colour.
std::optional<cellui::Color> iconColorOf(const Snapshot & snapshot, InstrumentId instrument);

// One edge for each group that has a row, with a member in a market or in any (in market order): what a table shows. The edge is the
// first member found, the one that stands for the group where its members have things in common (its base and quote).
std::vector<EdgeKey>
quoteEdges(const Snapshot &snapshot, MarketId market,
           KindSet kinds = {});
std::vector<EdgeKey> allQuoteEdges(const Snapshot & snapshot);

// A number with `decimals` places and a decimal point whatever the locale, with a sign when `sign`.
std::string fixed(double v, int decimals, bool sign = false);

// The headings of the change and the trace columns name the period: "Change 1d", "1 day"; the low and the high columns: "1d low", "1d high".
std::string changeTitle(Period);
std::string highTitle(Period);
std::string lowTitle(Period);

// A list of groups as a table: base, quote, name, price, the change over a period and a trace of its closes. The rows are not
// built: a cell is read from the snapshot when the table asks for it, which is for the rows in view, so the cost of
// showing tens of thousands of rows, and of showing them again when the store changes, does not grow with their number.
// Only a sort reads a whole column, once, and keeps it until the rows change.
class Quotes : public cellui::TableModel {
 public:
  enum Column { ICON, BASE, QUOTE, NAME, PRICE, CHANGE, LOW, HIGH, HISTORY };

  // `icons` supplies the pictures of the icon column; without it the column is empty.
  Quotes(std::shared_ptr<const Snapshot> snapshot, std::vector<EdgeKey> keys, std::shared_ptr<IconLoader> icons = nullptr);
  // The period the change column and the trace show: every row at once. The sort keys of the change are read again; the rows,
  // the sort and the scroll position are the table's, which is told with modelChanged().
  void setPeriod(Period);
  Period period() const { return period_; }
  // A narrow display hides the high and the low columns: the table then has no such columns, and its column `c` from LOW on is this model's column
  // `c + 2` (Browser sets the headings with it). Every call of the table takes the columns it has; the constants above are this model's own.
  void setNarrow(bool narrow) { narrow_ = narrow; }
  bool narrow() const { return narrow_; }
  // The table's column for a model column, or -1 where it is hidden; and the model's column for the table's.
  int tableColumn(int column) const { return !narrow_ || column < LOW ? column : column <= HIGH ? -1 : column - 2; }
  // Which edges a table shows: every market's, or one market's (not found yet: none, and what came meanwhile is read once it is).
  struct Scope {
    bool all = false;
    std::optional<MarketId> market; // only this market's edges, where set
    KindSet kinds; // only edges whose base is of one of these kinds, where set
    bool resolved() const { return all || market.has_value(); }
    bool has(const Snapshot &snapshot, const EdgeKey &key) const {
      return (all || (market && key.market == *market)) &&
             kinds.admits(snapshot.kind(key.base));
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
  // The edge that stands for a row: one member of its group, for what the members have in common (base and quote). Not its identity:
  // that is `group`.
  const EdgeKey & edge(int r) const { return keys_[static_cast<size_t>(r)]; }
  GroupKey group(int r) const { return groupOf(edge(r)); }
  // What the datasets that count say of the row together, and which count (Group.h).
  const Mix & mix(int r) const;
  // The members of the row that count: the edges whose bars it is made from.
  std::vector<EdgeKey> members(int r) const;
  // The market a preset of one market shows: only its members count, so the row says what that market says. None: every market's.
  void setMarket(std::optional<MarketId> market);
  // Which datasets count; empty: all. The user's switches. The caches go.
  void setFilter(DatasetFilter);
  const DatasetFilter & filter() const { return filter_; }
  // The icon URL of a row's base instrument, empty for none.
  std::string iconUrl(int r) const;
  std::shared_ptr<const cellui::TableIcon> icon(int r, int c) const override;
  // The symbol an instrument says stands for its icon, shown where there is no picture.
  std::string iconFallback(int r, int c) const override;
  std::optional<cellui::Color> iconFallbackColor(int r, int c) const override;
  std::span<const float> trace(int r, int) const override;
  std::span<const float> traceLight(int r, int) const override;
  double traceSpan(int r, int) const override;
  std::optional<cellui::Color> color(int r, int c) const override;
  // Prices and changes sort by their values, not their rounded text; the others by their text, letters of either case alike.
  // The change is the number as shown: basis points for a rate, per cent for a price. In a preset that mixes rates and prices
  // (All) they compare as raw numbers; there is no other ordering.
  std::optional<double> number(int r, int c) const override;
  int compare(int a, int b, int column) const override;

 private:
  double price(int r) const;
  double change(int r) const;  // over the period, in percent (basis points for a rate); undefined for no number
  double high(int r) const;    // the highest and the lowest price of the period, in the row's price unit; undefined for no number
  double low(int r) const;
  std::string priceText(int r, double) const;  // a price as the Price column shows it: a rate in per cent

  int own(int c) const { return narrow_ && c >= LOW ? c + 2 : c; }
  bool narrow_ = false;
  Period period_ = kDefaultPeriod;
  std::shared_ptr<IconLoader> icons_;
  std::shared_ptr<const Snapshot> snapshot_;
  std::vector<EdgeKey> keys_;
  size_t rows_seen_ = 0;  // how many of the store's rows (Snapshot::quoteRowCount()) the rows above are made from
  std::optional<MarketId> market_;
  DatasetFilter filter_;
  std::unordered_set<GroupKey> groups_;                  // the groups the rows are
  mutable std::unordered_map<int, Mix> mixes_;           // the rows drawn or sorted lately
  const PeriodView & traceOf(int r) const;  // the row's period view with its trace, kept while it is drawn
  mutable std::unordered_map<int, PeriodView> traces_;  // the rows drawn lately, for the span trace() gives
  mutable std::vector<double> numbers_[4];                      // the sort keys of price, change, high and low
  mutable std::vector<std::string> texts_[HISTORY];             // and of the text columns, in lower case
  mutable bool built_[HISTORY + 1] = {};
  void build(int column) const;
};
