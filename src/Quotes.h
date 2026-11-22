#pragma once

#include "model/Store.h"

#include "icons/IconLoader.h"

#include <cellui/Table.h>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// One table row is one edge: what one unit of `base` costs in `quote`, as one dataset reports it on one market.
struct QuoteRow {
  EdgeKey key;
  std::string base, quote, name, dataset;
  std::string icon_url;  // of the base instrument; empty for none
  double price = 0, change = 0;  // change in percent, over the last interval
  std::vector<float> history;    // recent closes, oldest first
};

// The rows of a market: one per edge that has candles, in the order the store lists them. At most `bars` closes each.
std::vector<QuoteRow> makeRows(const Snapshot & snapshot, MarketId market, size_t bars = 60);

// A number with `decimals` places and a decimal point whatever the locale, with a sign when `sign`.
std::string fixed(double v, int decimals, bool sign = false);

// The rows of a market as a table: base, quote, name, dataset, price, change and a trace of the closes.
class Quotes : public cellui::TableModel {
 public:
  enum Column { ICON, BASE, QUOTE, NAME, DATASET, PRICE, CHANGE, HISTORY };

  // `icons` supplies the pictures of the icon column; without it the column is empty.
  explicit Quotes(std::vector<QuoteRow> r, std::shared_ptr<IconLoader> icons = nullptr) : rows(std::move(r)), icons_(std::move(icons)) {}
  std::vector<QuoteRow> rows;
  int rowCount() const override { return static_cast<int>(rows.size()); }
  std::string text(int r, int c) const override;
  // The edge a row shows: its identity.
  const EdgeKey & edge(int r) const { return at(r).key; }
  std::shared_ptr<const cellui::TableIcon> icon(int r, int c) const override {
    return icons_ && c == ICON && !at(r).icon_url.empty() ? icons_->get(at(r).icon_url) : nullptr;
  }
  std::span<const float> trace(int r, int) const override { return at(r).history; }
  std::optional<cellui::Color> color(int r, int c) const override;
  // Prices and changes sort by their values, not their rounded text.
  std::optional<double> number(int r, int c) const override;

 private:
  std::shared_ptr<IconLoader> icons_;
  const QuoteRow & at(int r) const { return rows[static_cast<size_t>(r)]; }
};
